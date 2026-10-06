// Builds a module where @src holds when `requires` does and @tgt calls the
// lifted function as the tester does and returns whether every `ensures`
// holds, then asks Alive2 whether @tgt refines @src. For a rule, @src and @tgt
// run the code before and after it on the same state and memory.

#include "z80tester/Prover.h"

#include "z80lift/Lifter.h"

#include "llvm_util/compare.h"
#include "llvm_util/llvm2alive.h"
#include "smt/smt.h"
#include "util/config.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <sstream>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

static size_t offsetOf(Reg8 R) {
  switch (R) {
  case RA: return offsetof(State, A);
  case RB: return offsetof(State, B);
  case RC: return offsetof(State, C);
  case RD: return offsetof(State, D);
  case RE: return offsetof(State, E);
  case RH: return offsetof(State, H);
  default: return offsetof(State, L);
  }
}

/// The bits of an integer or floating-point value.
static Value *toBits(IRBuilder<> &B, Value *V) {
  Type *T = V->getType();
  if (T->isIntegerTy())
    return V;
  return B.CreateBitCast(V, B.getIntNTy(T->getPrimitiveSizeInBits()));
}

static Value *fromBits(IRBuilder<> &B, Value *V, Type *T) {
  if (T->isIntegerTy())
    return B.CreateZExtOrTrunc(V, T);
  return B.CreateBitCast(
      B.CreateZExtOrTrunc(V, B.getIntNTy(T->getPrimitiveSizeInBits())), T);
}

namespace {

/// The call that @tgt makes, on a state and memory of its own.
struct Call {
  IRBuilder<> &B;
  Value *St, *Mem;

  Value *field(size_t Off) {
    return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), St, Off);
  }
  Value *mem(uint16_t A) {
    return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), Mem, A);
  }
  Value *load8(size_t Off) { return B.CreateLoad(B.getInt8Ty(), field(Off)); }

  /// Z80 memory has no alignment.
  void store(Value *V, uint16_t A) {
    B.CreateAlignedStore(V, mem(A), Align(1));
  }

  /// Any value of the type, as a register holds before the call.
  void any(size_t Off, Type *T) {
    B.CreateStore(B.CreateFreeze(PoisonValue::get(T)), field(Off));
  }
  void anyBool(size_t Off) {
    Value *Bit = B.CreateFreeze(PoisonValue::get(B.getInt1Ty()));
    B.CreateStore(B.CreateZExt(Bit, B.getInt8Ty()), field(Off));
  }
  void set(size_t Off, Type *T, uint64_t V) {
    B.CreateStore(ConstantInt::get(T, V), field(Off));
  }

  /// Puts V in registers, the most significant byte first.
  void writeRegs(ArrayRef<Reg8> Regs, Value *V) {
    Value *Bits = toBits(B, V);
    for (size_t I = 0; I < Regs.size(); ++I) {
      Value *Byte = B.CreateLShr(Bits, 8 * (Regs.size() - 1 - I));
      B.CreateStore(B.CreateTrunc(Byte, B.getInt8Ty()),
                    field(offsetOf(Regs[I])));
    }
  }

  Value *readRegs(ArrayRef<Reg8> Regs) {
    Type *T = B.getIntNTy(8 * Regs.size());
    Value *V = nullptr;
    for (Reg8 R : Regs) {
      Value *Byte = B.CreateZExt(load8(offsetOf(R)), T);
      V = V ? B.CreateOr(B.CreateShl(V, 8), Byte) : Byte;
    }
    return V;
  }

  Value *pair(size_t Hi, size_t Lo) {
    Type *I16 = B.getInt16Ty();
    return B.CreateOr(B.CreateShl(B.CreateZExt(load8(Hi), I16), 8),
                      B.CreateZExt(load8(Lo), I16));
  }

  Value *reg(Reg R) {
    switch (R) {
    case Reg::A: return load8(offsetof(State, A));
    case Reg::B: return load8(offsetof(State, B));
    case Reg::C: return load8(offsetof(State, C));
    case Reg::D: return load8(offsetof(State, D));
    case Reg::E: return load8(offsetof(State, E));
    case Reg::H: return load8(offsetof(State, H));
    case Reg::L: return load8(offsetof(State, L));
    case Reg::BC: return pair(offsetof(State, B), offsetof(State, C));
    case Reg::DE: return pair(offsetof(State, D), offsetof(State, E));
    case Reg::HL: return pair(offsetof(State, H), offsetof(State, L));
    case Reg::IX: return pair(offsetof(State, IXH), offsetof(State, IXL));
    case Reg::IY: return pair(offsetof(State, IYH), offsetof(State, IYL));
    case Reg::SP:
      return B.CreateLoad(B.getInt16Ty(), field(offsetof(State, SP)));
    }
    return nullptr;
  }
};

} // namespace

/// Builds @tgt(params): runs the lifted function on the arguments and
/// returns whether every `ensures` holds. A call that faults, halts, does not
/// return to the sentinel, leaves SP wrong or clobbers IX is undefined.
static Function *buildTarget(Module &M, const TestPlan &P, const Contract &K,
                             size_t I, FunctionType *FT, Function *Lifted,
                             const ProofOptions &O) {
  LLVMContext &Ctx = M.getContext();
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "tgt", M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
  Type *I8 = B.getInt8Ty(), *I16 = B.getInt16Ty(), *I32 = B.getInt32Ty(),
       *I64 = B.getInt64Ty();
  Call C{B, B.CreateAlloca(ArrayType::get(I8, sizeof(State))),
         B.CreateAlloca(ArrayType::get(I8, 0x10000))};
  cast<AllocaInst>(C.St)->setAlignment(Align(alignof(State)));

  // Every register starts with any value, each field at its own size so that
  // the state becomes plain values.
  for (size_t Off :
       {offsetof(State, A),   offsetof(State, B),   offsetof(State, C),
        offsetof(State, D),   offsetof(State, E),   offsetof(State, H),
        offsetof(State, L),   offsetof(State, IXH), offsetof(State, IXL),
        offsetof(State, IYH), offsetof(State, IYL), offsetof(State, I),
        offsetof(State, R),   offsetof(State, A2),  offsetof(State, F2),
        offsetof(State, B2),  offsetof(State, C2),  offsetof(State, D2),
        offsetof(State, E2),  offsetof(State, H2),  offsetof(State, L2),
        offsetof(State, IM)})
    C.any(Off, I8);
  for (size_t Off :
       {offsetof(State, SF), offsetof(State, ZF), offsetof(State, HF),
        offsetof(State, PVF), offsetof(State, NF), offsetof(State, CF),
        offsetof(State, IFF1), offsetof(State, IFF2)})
    C.anyBool(Off);

  const CallLayout &L = P.Layout;
  uint16_t Base = StackTop - L.StackBytes;
  for (size_t A = 0; A < P.Params.size(); ++A) {
    // A register holds bits, never poison.
    Value *V = B.CreateFreeze(F->getArg(A));
    const Loc &Lc = L.Params[A];
    if (!Lc.Regs.empty())
      C.writeRegs(Lc.Regs, V);
    else
      C.store(toBits(B, V), Base + Lc.Offset);
  }
  if (L.SRet)
    C.store(B.getInt16(SRetBuf), Base);
  uint16_t SP = Base - 2;
  C.store(B.getInt16(Sentinel), SP);
  C.set(offsetof(State, SP), I16, SP);
  C.set(offsetof(State, PC), I16, P.Entry);
  C.set(offsetof(State, Halted), I8, 0);
  C.set(offsetof(State, Steps), I64, 0);
  C.set(offsetof(State, StepLimit), I64, O.StepLimit);
  C.set(offsetof(State, WriteLo), I32, StackLo);
  C.set(offsetof(State, WriteHi), I32, WindowHi);
  C.set(offsetof(State, Fault), I8, 0);
  C.set(offsetof(State, FaultAddr), I16, 0);

  // What old() reads, and IX to compare after the call.
  std::vector<std::vector<Value *>> Old(K.Ensures.size());
  for (size_t E = 0; E < K.Ensures.size(); ++E)
    for (Reg R : K.Ensures[E].OldRegs)
      Old[E].push_back(C.reg(R));
  Value *IXBefore = C.reg(Reg::IX);

  B.CreateCall(Lifted, {C.St, C.Mem});

  Value *Ok = B.CreateICmpEQ(B.CreateLoad(I16, C.field(offsetof(State, PC))),
                             B.getInt16(Sentinel));
  Ok = B.CreateAnd(Ok,
                   B.CreateICmpEQ(C.reg(Reg::SP), B.getInt16(Base + L.Popped)));
  Ok = B.CreateAnd(
      Ok, B.CreateICmpEQ(C.load8(offsetof(State, Fault)), B.getInt8(0)));
  Ok = B.CreateAnd(
      Ok, B.CreateICmpEQ(C.load8(offsetof(State, Halted)), B.getInt8(0)));
  if (P.C == Cpu::Z80 && !P.Placed)
    Ok = B.CreateAnd(Ok, B.CreateICmpEQ(C.reg(Reg::IX), IXBefore));
  BasicBlock *Bad = BasicBlock::Create(Ctx, "bad", F);
  BasicBlock *Done = BasicBlock::Create(Ctx, "done", F);
  B.CreateCondBr(Ok, Done, Bad);
  B.SetInsertPoint(Bad);
  B.CreateUnreachable();
  B.SetInsertPoint(Done);

  Value *Result = nullptr;
  if (const std::optional<Ty> &Ret = P.Ret)
    Result = L.Ret ? C.readRegs(L.Ret->Regs)
                   : B.CreateAlignedLoad(B.getIntNTy(8 * sizeOf(*Ret)),
                                         C.mem(SRetBuf), Align(1));

  Value *All = B.getTrue();
  for (size_t E = 0; E < K.Ensures.size(); ++E) {
    Function *Ens = M.getFunction(ensuresFunction(I, E));
    std::vector<Value *> Args;
    if (Result)
      Args.push_back(fromBits(B, Result, Ens->getArg(0)->getType()));
    for (Argument &A : F->args())
      Args.push_back(&A);
    for (Reg R : K.Ensures[E].Regs)
      Args.push_back(C.reg(R));
    llvm::append_range(Args, Old[E]);
    All = B.CreateAnd(All, B.CreateCall(Ens, Args));
  }
  B.CreateRet(All);
  return F;
}

/// Builds @src(params): undefined unless `requires` holds, and true.
static Function *buildSource(Module &M, const Contract &K, size_t I,
                             FunctionType *FT) {
  LLVMContext &Ctx = M.getContext();
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, "src", M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
  if (!K.Requires.empty()) {
    std::vector<Value *> Args;
    for (Argument &A : F->args())
      Args.push_back(&A);
    Value *Met = B.CreateCall(M.getFunction(requiresFunction(I)), Args);
    BasicBlock *Unmet = BasicBlock::Create(Ctx, "unmet", F);
    BasicBlock *Body = BasicBlock::Create(Ctx, "met", F);
    B.CreateCondBr(Met, Body, Unmet);
    B.SetInsertPoint(Unmet);
    B.CreateUnreachable();
    B.SetInsertPoint(Body);
  }
  B.CreateRet(B.getTrue());
  return F;
}

namespace {

/// The state a rule starts from, as parameters of @src and @tgt.
struct StartField {
  const char *Name;
  size_t Off;
  unsigned Bits;
};
const StartField StartFields[] = {
    {"A", offsetof(State, A), 8},      {"B", offsetof(State, B), 8},
    {"C", offsetof(State, C), 8},      {"D", offsetof(State, D), 8},
    {"E", offsetof(State, E), 8},      {"H", offsetof(State, H), 8},
    {"L", offsetof(State, L), 8},      {"IXH", offsetof(State, IXH), 8},
    {"IXL", offsetof(State, IXL), 8},  {"IYH", offsetof(State, IYH), 8},
    {"IYL", offsetof(State, IYL), 8},  {"I", offsetof(State, I), 8},
    {"R", offsetof(State, R), 8},      {"A2", offsetof(State, A2), 8},
    {"F2", offsetof(State, F2), 8},    {"B2", offsetof(State, B2), 8},
    {"C2", offsetof(State, C2), 8},    {"D2", offsetof(State, D2), 8},
    {"E2", offsetof(State, E2), 8},    {"H2", offsetof(State, H2), 8},
    {"L2", offsetof(State, L2), 8},    {"IM", offsetof(State, IM), 8},
    {"SP", offsetof(State, SP), 16},   {"SF", offsetof(State, SF), 1},
    {"ZF", offsetof(State, ZF), 1},    {"HF", offsetof(State, HF), 1},
    {"PVF", offsetof(State, PVF), 1},  {"NF", offsetof(State, NF), 1},
    {"CF", offsetof(State, CF), 1},    {"IFF1", offsetof(State, IFF1), 1},
    {"IFF2", offsetof(State, IFF2), 1}};

} // namespace

/// Where each flag sits in F.
static unsigned flagBit(Cpu C, Kept K) {
  if (C == Cpu::SM83)
    switch (K) {
    case Kept::ZF: return 7;
    case Kept::NF: return 6;
    case Kept::HF: return 5;
    default: return 4;
    }
  switch (K) {
  case Kept::SF: return 7;
  case Kept::ZF: return 6;
  case Kept::HF: return 4;
  case Kept::PVF: return 2;
  case Kept::NF: return 1;
  default: return 0;
  }
}

static size_t flagOffset(Kept K) {
  switch (K) {
  case Kept::SF: return offsetof(State, SF);
  case Kept::ZF: return offsetof(State, ZF);
  case Kept::HF: return offsetof(State, HF);
  case Kept::PVF: return offsetof(State, PVF);
  case Kept::NF: return offsetof(State, NF);
  default: return offsetof(State, CF);
  }
}

static Value *kept(Call &S, Cpu C, Kept K) {
  IRBuilder<> &B = S.B;
  switch (K) {
  case Kept::A: return S.reg(Reg::A);
  case Kept::B: return S.reg(Reg::B);
  case Kept::C: return S.reg(Reg::C);
  case Kept::D: return S.reg(Reg::D);
  case Kept::E: return S.reg(Reg::E);
  case Kept::H: return S.reg(Reg::H);
  case Kept::L: return S.reg(Reg::L);
  case Kept::BC: return S.reg(Reg::BC);
  case Kept::DE: return S.reg(Reg::DE);
  case Kept::HL: return S.reg(Reg::HL);
  case Kept::IX: return S.reg(Reg::IX);
  case Kept::IY: return S.reg(Reg::IY);
  case Kept::SP: return S.reg(Reg::SP);
  case Kept::F: {
    Value *F = B.getInt8(0);
    for (Kept Flag :
         {Kept::SF, Kept::ZF, Kept::HF, Kept::PVF, Kept::NF, Kept::CF}) {
      if (C == Cpu::SM83 && (Flag == Kept::SF || Flag == Kept::PVF))
        continue;
      F = B.CreateOr(F,
                     B.CreateShl(S.load8(flagOffset(Flag)), flagBit(C, Flag)));
    }
    return F;
  }
  default: return B.CreateTrunc(S.load8(flagOffset(K)), B.getInt1Ty());
  }
}

namespace {

/// Memory before a rule's code: the byte of the first cell with its address,
/// or else Fill. Both sides return the byte at At, so all of memory must match.
struct MemoryParams {
  Value *At, *Fill;
  std::vector<std::pair<Value *, Value *>> Cells;
};

} // namespace

static MemoryParams memoryParams(Function &F, unsigned Cells) {
  MemoryParams P;
  unsigned A = std::size(StartFields);
  P.At = F.getArg(A++);
  P.At->setName("at");
  P.Fill = F.getArg(A++);
  P.Fill->setName("fill");
  for (unsigned C = 0; C < Cells; ++C, A += 2) {
    F.getArg(A)->setName("addr" + Twine(C));
    F.getArg(A + 1)->setName("byte" + Twine(C));
    P.Cells.push_back({F.getArg(A), F.getArg(A + 1)});
  }
  return P;
}

/// Builds one side of rule I: runs its code from the state in the
/// parameters and returns whether it got to the end, where it went if not,
/// what the rule keeps, the first value in the highest bits, and the byte at
/// `at`.
static Function *buildSide(Module &M, Cpu C, const Rule &R, StringRef Name,
                           FunctionType *FT, Function *Lifted, uint16_t Entry,
                           GlobalVariable *Mem, unsigned Cells) {
  LLVMContext &Ctx = M.getContext();
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage, Name, M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "entry", F));
  Type *I8 = B.getInt8Ty(), *I16 = B.getInt16Ty(), *I32 = B.getInt32Ty(),
       *I64 = B.getInt64Ty();
  Call S{B, B.CreateAlloca(ArrayType::get(I8, sizeof(State))), Mem};
  cast<AllocaInst>(S.St)->setAlignment(Align(alignof(State)));

  for (size_t A = 0; A < std::size(StartFields); ++A) {
    Value *V = B.CreateFreeze(F->getArg(A));
    F->getArg(A)->setName(StartFields[A].Name);
    if (StartFields[A].Bits == 1)
      V = B.CreateZExt(V, I8);
    B.CreateStore(V, S.field(StartFields[A].Off));
  }
  MemoryParams P = memoryParams(*F, Cells);
  S.set(offsetof(State, PC), I16, Entry);
  S.set(offsetof(State, Halted), I8, 0);
  S.set(offsetof(State, Steps), I64, 0);
  S.set(offsetof(State, StepLimit), I64, UINT64_MAX);
  S.set(offsetof(State, WriteLo), I32, 0);
  S.set(offsetof(State, WriteHi), I32, 0x10000);
  S.set(offsetof(State, Fault), I8, 0);
  S.set(offsetof(State, FaultAddr), I16, 0);

  B.CreateCall(Lifted, {S.St, Mem});

  // Every address is writable and the steps are unlimited, so the code cannot
  // fault; it ends at its HALT or leaves.
  Value *Ended = B.CreateICmpNE(S.load8(offsetof(State, Halted)), B.getInt8(0));
  Value *Exit = B.CreateSelect(Ended, B.getInt16(0),
                               B.CreateLoad(I16, S.field(offsetof(State, PC))));
  Type *RT = FT->getReturnType();
  Value *V = B.CreateZExt(Ended, RT);
  auto Append = [&](Value *Part) {
    unsigned Bits = Part->getType()->getIntegerBitWidth();
    V = B.CreateOr(B.CreateShl(V, Bits), B.CreateZExt(Part, RT));
  };
  Append(Exit);
  for (Kept K : R.Keep)
    Append(kept(S, C, K));
  Value *At = B.CreateFreeze(P.At);
  Value *Byte =
      B.CreateLoad(I8, B.CreateInBoundsGEP(I8, Mem, B.CreateZExt(At, I64)));
  if (R.AboveSP) {
    // The 32 KiB below SP, where interrupts may write, need not match.
    Value *Below =
        B.CreateICmpSLT(B.CreateSub(At, S.reg(Reg::SP)), B.getInt16(0));
    Byte = B.CreateSelect(Below, B.getInt8(0), Byte);
  }
  Append(Byte);
  B.CreateRet(V);
  return F;
}

/// The address that P, a pointer into Mem, points to.
static Value *addressIn(IRBuilder<> &B, Value *P, GlobalVariable *Mem,
                        const DataLayout &DL) {
  Value *Addr = B.getInt16(0);
  while (P != Mem) {
    auto *G = dyn_cast<GEPOperator>(P);
    SmallMapVector<Value *, APInt, 4> Vars;
    APInt Const(64, 0);
    if (!G || !G->collectOffset(DL, 64, Vars, Const))
      return nullptr;
    Addr = B.CreateAdd(Addr, B.getInt16(Const.getZExtValue()));
    for (auto &[Var, Scale] : Vars)
      Addr = B.CreateAdd(Addr,
                         B.CreateMul(B.CreateZExtOrTrunc(Var, B.getInt16Ty()),
                                     B.getInt16(Scale.getZExtValue())));
    P = G->getPointerOperand();
  }
  return Addr;
}

/// Turns the loads and stores of F in Mem into values: a load reads the last
/// store to its address, or else the memory that P describes. Without loops,
/// reverse post-order is the order in which the stores run.
static Error rewriteMemory(Function &F, GlobalVariable *Mem,
                           const MemoryParams &P, StringRef Where) {
  const DataLayout &DL = F.getDataLayout();
  SmallVector<WeakTrackingVH> Unused;
  for (Instruction &I : instructions(F))
    if (isInstructionTriviallyDead(&I))
      Unused.push_back(&I);
  RecursivelyDeleteTriviallyDeadInstructionsPermissive(Unused);
  std::vector<Instruction *> Accesses;
  ReversePostOrderTraversal<Function *> RPO(&F);
  for (BasicBlock *BB : RPO)
    for (Instruction &I : *BB)
      if (isa<LoadInst>(I) || isa<StoreInst>(I))
        if (getUnderlyingObject(getLoadStorePointerOperand(&I), 0) == Mem)
          Accesses.push_back(&I);

  // A slot for every byte that a store writes: whether it ran, its address
  // and the byte.
  IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
  Type *I1 = B.getInt1Ty(), *I8 = B.getInt8Ty(), *I16 = B.getInt16Ty();
  struct Slot {
    AllocaInst *Ran, *Addr, *Byte;
  };
  std::vector<Slot> Slots;
  for (Instruction *I : Accesses)
    if (auto *SI = dyn_cast<StoreInst>(I))
      for (uint64_t K = 0;
           K < DL.getTypeStoreSize(SI->getValueOperand()->getType()); ++K) {
        Slot S{B.CreateAlloca(I1), B.CreateAlloca(I16), B.CreateAlloca(I8)};
        B.CreateStore(B.getFalse(), S.Ran);
        B.CreateStore(B.getInt16(0), S.Addr);
        B.CreateStore(B.getInt8(0), S.Byte);
        Slots.push_back(S);
      }

  auto Fail = [&] {
    return createStringError("%s: the proof cannot follow a memory access of "
                             "the rule's code",
                             Where.str().c_str());
  };
  size_t Next = 0;
  SmallVector<WeakTrackingVH> Dead;
  for (Instruction *I : Accesses) {
    B.SetInsertPoint(I);
    Value *Addr = addressIn(B, getLoadStorePointerOperand(I), Mem, DL);
    Type *T = getLoadStoreType(I);
    if (!Addr || !T->isIntegerTy())
      return Fail();
    uint64_t Size = DL.getTypeStoreSize(T);
    if (auto *SI = dyn_cast<StoreInst>(I)) {
      for (uint64_t K = 0; K < Size; ++K, ++Next) {
        Value *Byte =
            B.CreateTrunc(B.CreateLShr(SI->getValueOperand(), 8 * K), I8);
        B.CreateStore(B.getTrue(), Slots[Next].Ran);
        B.CreateStore(B.CreateAdd(Addr, B.getInt16(K)), Slots[Next].Addr);
        B.CreateStore(Byte, Slots[Next].Byte);
      }
    } else {
      Value *V = ConstantInt::get(T, 0);
      for (uint64_t K = 0; K < Size; ++K) {
        Value *At = B.CreateAdd(Addr, B.getInt16(K));
        Value *Byte = P.Fill;
        for (auto [CellAddr, CellByte] : llvm::reverse(P.Cells))
          Byte = B.CreateSelect(B.CreateICmpEQ(CellAddr, At), CellByte, Byte);
        for (const Slot &S : Slots) {
          Value *Hit =
              B.CreateAnd(B.CreateLoad(I1, S.Ran),
                          B.CreateICmpEQ(B.CreateLoad(I16, S.Addr), At));
          Byte = B.CreateSelect(Hit, B.CreateLoad(I8, S.Byte), Byte);
        }
        V = B.CreateOr(V, B.CreateShl(B.CreateZExt(Byte, T), 8 * K));
      }
      I->replaceAllUsesWith(V);
    }
    Dead.push_back(getLoadStorePointerOperand(I));
    I->eraseFromParent();
  }
  RecursivelyDeleteTriviallyDeadInstructionsPermissive(Dead);
  return Error::success();
}

/// Inlines everything into @src and @tgt and turns their memory into values.
/// Nothing may simplify the control flow: that would drop the checks that
/// end in `unreachable`.
static Error simplify(Module &M, Function *Src, Function *Tgt) {
  for (Function &F : M) {
    if (F.isDeclaration() || &F == Src || &F == Tgt)
      continue;
    F.setLinkage(GlobalValue::InternalLinkage);
    F.removeFnAttr(Attribute::NoInline);
    F.removeFnAttr(Attribute::OptimizeNone);
    F.addFnAttr(Attribute::AlwaysInline);
    // Inlining needs matching target attributes, and the noalias scopes it
    // would add are not something Alive2 reads.
    for (StringRef A : {"target-cpu", "target-features", "tune-cpu"})
      F.removeFnAttr(A);
    for (Argument &A : F.args())
      A.removeAttr(Attribute::NoAlias);
  }

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM;
  if (Error E = PB.parsePassPipeline(
          MPM, "always-inline,sroa,early-cse,sroa,early-cse,globaldce"))
    return E;
  MPM.run(M, MAM);

  // Alive2 reads neither type-based nor scoped alias information, which
  // inlining a lifted function into another one adds.
  for (Function *F : {Src, Tgt})
    for (Instruction &I : make_early_inc_range(instructions(*F))) {
      if (auto *II = dyn_cast<IntrinsicInst>(&I);
          II &&
          II->getIntrinsicID() == Intrinsic::experimental_noalias_scope_decl) {
        II->eraseFromParent();
        continue;
      }
      for (unsigned K : {LLVMContext::MD_tbaa, LLVMContext::MD_alias_scope,
                         LLVMContext::MD_noalias})
        I.setMetadata(K, nullptr);
    }
  return Error::success();
}

/// Whether F has a cycle, also one with several entries, which code that
/// jumps into a loop makes.
static bool hasLoop(Function &F) {
  SmallVector<std::pair<const BasicBlock *, const BasicBlock *>> Backedges;
  FindFunctionBackedges(F, Backedges);
  return !Backedges.empty();
}

/// A function that F calls and whose body Alive2 cannot see.
static std::optional<std::string> opaqueCall(Function &F) {
  for (Instruction &Inst : instructions(F))
    if (auto *CB = dyn_cast<CallBase>(&Inst)) {
      Function *Callee = CB->getCalledFunction();
      if (!Callee)
        return std::string("an indirect call");
      if (!Callee->isIntrinsic())
        return Callee->getName().str();
    }
  return std::nullopt;
}

namespace {

/// The two sides of a rule in a copy of the lifted code, inlined and
/// simplified, with memory still in Mem.
struct Sides {
  std::unique_ptr<Module> M;
  std::array<Function *, 2> F;
  GlobalVariable *Mem;
};

} // namespace

static Expected<Sides> buildSides(const Module &Lifted, const Rule &R, Cpu C,
                                  const Image &Img,
                                  const std::array<uint16_t, 2> &Entry,
                                  unsigned Cells) {
  Sides S;
  S.M = CloneModule(Lifted);
  LLVMContext &Ctx = S.M->getContext();
  // The lifted code reads and writes this, until rewriteMemory turns it into
  // values; Alive2 handles symbolic addresses in memory slowly.
  S.Mem = new GlobalVariable(
      *S.M, ArrayType::get(Type::getInt8Ty(Ctx), 0x10000), /*isConstant=*/false,
      GlobalValue::ExternalLinkage, nullptr, "mem");
  S.Mem->setAlignment(Align(1));

  std::vector<Type *> Params;
  for (const StartField &F : StartFields)
    Params.push_back(Type::getIntNTy(Ctx, F.Bits));
  Params.push_back(Type::getInt16Ty(Ctx)); // at
  Params.push_back(Type::getInt8Ty(Ctx));  // fill
  for (unsigned K = 0; K < Cells; ++K) {
    Params.push_back(Type::getInt16Ty(Ctx));
    Params.push_back(Type::getInt8Ty(Ctx));
  }
  unsigned Bits = 1 + 16 + 8;
  for (Kept K : R.Keep)
    Bits += keptBits(K);
  FunctionType *FT =
      FunctionType::get(Type::getIntNTy(Ctx, Bits), Params, false);
  for (bool After : {false, true})
    S.F[After] = buildSide(*S.M, C, R, After ? "tgt" : "src", FT,
                           S.M->getFunction(Img.nameAt(Entry[After])),
                           Entry[After], S.Mem, Cells);
  if (Error E = simplify(*S.M, S.F[0], S.F[1]))
    return std::move(E);
  return S;
}

static U128 hexValue(StringRef Hex) {
  U128 V = 0;
  for (char Ch : Hex.take_while(isHexDigit))
    V = V << 4 | hexDigitValue(Ch);
  return V;
}

/// Reads Alive2's report: why it failed, a counterexample's arguments and
/// what each side returns for them.
static void readReport(StringRef Text, ProofResult &R) {
  SmallVector<StringRef> Lines;
  Text.split(Lines, '\n');
  bool InExample = false;
  for (StringRef Line : Lines) {
    Line = Line.trim();
    if (R.Verdict.empty() && Line.consume_front("ERROR: "))
      R.Verdict = Line.str();
    if (Line == "Example:" && R.Example.empty()) {
      InExample = true;
      continue;
    }
    if (InExample) {
      // <type> %<name> = #x<hex> ...
      auto [Lhs, Rhs] = Line.split(" = ");
      StringRef Hex = Rhs.trim();
      if (Line.empty() || !Hex.consume_front("#x")) {
        InExample = !Line.empty();
        continue;
      }
      R.Example.push_back(
          {Lhs.substr(Lhs.find('%') + 1).trim().str(), hexValue(Hex)});
      continue;
    }
    if (Line.consume_front("Source value: "))
      R.SourceValue = Line.str();
    else if (Line.consume_front("Target value: "))
      R.TargetValue = Line.str();
  }
  if (R.Verdict.empty()) {
    for (StringRef Line : Lines)
      if (!Line.trim().empty() && !Line.contains("Transformation")) {
        R.Verdict = Line.trim().str();
        break;
      }
  }
}

/// Simplifies @src and @tgt and asks Alive2 whether @tgt refines @src.
/// A loop is an error, LoopError, unless Unroll says how far to follow it.
static Expected<ProofResult> verify(Module &M, Function *Src, Function *Tgt,
                                    unsigned Unroll, StringRef Where,
                                    StringRef LoopError,
                                    const ProofOptions &O) {
  if (Error E = simplify(M, Src, Tgt))
    return std::move(E);
  std::string Broken;
  raw_string_ostream BrokenOS(Broken);
  if (verifyModule(M, &BrokenOS))
    return createStringError("%s: the proof is not valid IR: %s",
                             Where.str().c_str(), Broken.c_str());
  for (Function *F : {Src, Tgt}) {
    if (auto Callee = opaqueCall(*F))
      return createStringError("%s: the proof calls %s, whose body Alive2 "
                               "cannot see",
                               Where.str().c_str(), Callee->c_str());
    if (!Unroll && hasLoop(*F))
      return createStringError("%s: %s", Where.str().c_str(),
                               LoopError.str().c_str());
  }

  // Registers hold bits; a value cannot come in as poison or undef.
  util::config::disable_poison_input = true;
  util::config::disable_undef_input = true;
  util::config::src_unroll_cnt = Unroll;
  util::config::tgt_unroll_cnt = Unroll;
  smt::set_query_timeout(std::to_string(uint64_t(O.Timeout) * 1000));
  smt::set_memory_limit(uint64_t(O.Memory) << 20);

  std::ostringstream Out;
  TargetLibraryInfoWrapperPass TLI(M.getTargetTriple());
  llvm_util::initializer LLVMUtil(Out, M.getDataLayout());
  smt::smt_initializer SMT;
  llvm_util::Verifier V(TLI, SMT, Out);
  auto Start = std::chrono::steady_clock::now();
  try {
    V.compareFunctions(*Src, *Tgt);
  } catch (...) {
    return createStringError("%s: Alive2 failed: %s", Where.str().c_str(),
                             Out.str().c_str());
  }
  ProofResult R;
  R.Seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - Start)
          .count();
  if (V.num_correct)
    R.S = ProofResult::Proved;
  else
    R.S = V.num_unsound ? ProofResult::Counterexample : ProofResult::Unproven;
  if (R.S != ProofResult::Proved)
    readReport(Out.str(), R);
  return R;
}

Expected<ProofResult> z80tester::prove(const TestPlan &P, const Contract &K,
                                       size_t I, const Image &Img,
                                       MemoryBufferRef Contracts,
                                       const ProofOptions &O) {
  for (const ParamInfo &Info : P.Info)
    if (Info.Pointer)
      return createStringError("%s: proofs do not take pointer parameters yet",
                               P.Where.c_str());
  if (P.Ret == Ty::Ptr)
    return createStringError("%s: proofs do not take a pointer result yet",
                             P.Where.c_str());

  LLVMContext Ctx;
  auto L = z80lift::Lifter::create(P.C, Img, Ctx);
  if (!L)
    return L.takeError();
  if (Error E = (*L)->lift(P.Entry).takeError())
    return std::move(E);
  (*L)->optimize();
  std::unique_ptr<Module> M = (*L)->takeModule();
  Function *Lifted = M->getFunction(Img.nameAt(P.Entry));

  auto CM = parseBitcodeFile(Contracts, Ctx);
  if (!CM)
    return CM.takeError();
  (*CM)->setTargetTriple(M->getTargetTriple());
  (*CM)->setDataLayout(M->getDataLayout());
  if (Linker::linkModules(*M, std::move(*CM)))
    return createStringError("cannot link the contracts with the lifted code");

  // The arguments take their types and names from the prototype.
  FunctionType *Sig = M->getFunction(signatureFunction(I))->getFunctionType();
  FunctionType *FT =
      FunctionType::get(Type::getInt1Ty(Ctx), Sig->params(), false);
  Function *Src = buildSource(*M, K, I, FT);
  Function *Tgt = buildTarget(*M, P, K, I, FT, Lifted, O);
  for (size_t A = 0; A < K.ParamList.size(); ++A) {
    Src->getArg(A)->setName(K.ParamList[A].Name);
    Tgt->getArg(A)->setName(K.ParamList[A].Name);
  }

  Expected<ProofResult> R =
      verify(*M, Src, Tgt, K.Unroll, P.Where,
             "the proof has a loop; give the iterations to follow with "
             "'prove unroll N'",
             O);
  if (!R)
    return R;
  // The arguments in the order of the parameters.
  for (const ParamInfo &Info : K.ParamList)
    for (const auto &[Name, V] : R->Example)
      if (Name == Info.Name)
        R->Inputs.push_back(V);
  if (R->Inputs.size() != K.ParamList.size())
    R->Inputs.clear();
  return R;
}

Expected<ProofResult> z80tester::proveRule(const Rule &R, size_t I, Cpu C,
                                           const Image &Img,
                                           const ProofOptions &O) {
  LLVMContext Ctx;
  auto L = z80lift::Lifter::create(C, Img, Ctx);
  if (!L)
    return L.takeError();
  // Each side is at its own address, so a return to the address after a
  // `ret cc` must not continue there.
  (*L)->setTakenReturnsLeave(true);
  // A label outside the rule is an address past the image.
  (*L)->setJumpsOutsideLeave(true);
  const char *LoopError =
      "the rule's code has a loop, which a proof cannot follow";
  std::array<uint16_t, 2> Entry;
  for (bool After : {false, true}) {
    std::optional<uint16_t> A = Img.lookup(ruleLabel(I, After));
    if (!A)
      return createStringError("%s: rule %s is not in the image",
                               R.where().c_str(), R.Name.c_str());
    Entry[After] = *A;
    if (Error E = (*L)->lift(*A).takeError())
      return std::move(E);
  }
  (*L)->optimize();
  std::unique_ptr<Module> Lifted = (*L)->takeModule();

  // Once inlined, code without loops runs each load at most once, so a cell
  // for every byte loaded lets memory be anything the code can read.
  Expected<Sides> Probe = buildSides(*Lifted, R, C, Img, Entry, 0);
  if (!Probe)
    return Probe.takeError();
  unsigned Cells = 0;
  for (Function *F : Probe->F) {
    if (hasLoop(*F))
      return createStringError("%s: %s", R.where().c_str(), LoopError);
    for (Instruction &Inst : instructions(*F))
      if (auto *LI = dyn_cast<LoadInst>(&Inst))
        Cells += Lifted->getDataLayout().getTypeStoreSize(LI->getType());
  }

  Expected<Sides> S = buildSides(*Lifted, R, C, Img, Entry, Cells);
  if (!S)
    return S.takeError();
  for (Function *F : S->F)
    if (Error E = rewriteMemory(*F, S->Mem, memoryParams(*F, Cells), R.where()))
      return std::move(E);
  S->Mem->removeDeadConstantUsers();
  if (!S->Mem->use_empty())
    return createStringError("%s: the proof cannot follow a memory access of "
                             "the rule's code",
                             R.where().c_str());
  S->Mem->eraseFromParent();
  return verify(*S->M, S->F[0], S->F[1], /*Unroll=*/0, R.where(), LoopError, O);
}
