// Builds a module where @src holds when `requires` does and @tgt calls the
// lifted function as the tester does and returns whether every `ensures`
// holds, then asks Alive2 whether @tgt refines @src.

#include "z80tester/Prover.h"

#include "z80lift/Lifter.h"

#include "llvm_util/compare.h"
#include "llvm_util/llvm2alive.h"
#include "smt/smt.h"
#include "util/config.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FormatVariadic.h"

#include <chrono>
#include <cstddef>
#include <sstream>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

size_t offsetOf(Reg8 R) {
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
Value *toBits(IRBuilder<> &B, Value *V) {
  Type *T = V->getType();
  if (T->isIntegerTy())
    return V;
  return B.CreateBitCast(V, B.getIntNTy(T->getPrimitiveSizeInBits()));
}

Value *fromBits(IRBuilder<> &B, Value *V, Type *T) {
  if (T->isIntegerTy())
    return B.CreateZExtOrTrunc(V, T);
  return B.CreateBitCast(
      B.CreateZExtOrTrunc(V, B.getIntNTy(T->getPrimitiveSizeInBits())), T);
}

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

/// Builds @tgt(params): runs the lifted function on the arguments and
/// returns whether every `ensures` holds. A call that faults, halts, does not
/// return to the sentinel, leaves SP wrong or clobbers IX is undefined.
Function *buildTarget(Module &M, const TestPlan &P, const Contract &K, size_t I,
                      FunctionType *FT, Function *Lifted,
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
  if (P.Ret)
    Result = L.SRet ? B.CreateAlignedLoad(B.getIntNTy(8 * sizeOf(*P.Ret)),
                                          C.mem(SRetBuf), Align(1))
                    : C.readRegs(L.Ret->Regs);

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
Function *buildSource(Module &M, const Contract &K, size_t I,
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

/// Inlines everything into @src and @tgt and turns their memory into values.
/// Nothing may simplify the control flow: that would drop the checks that
/// end in `unreachable`.
Error simplify(Module &M, Function *Src, Function *Tgt) {
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
  return Error::success();
}

/// Whether F has a cycle, also one with several entries, which code that
/// jumps into a loop makes.
bool hasLoop(Function &F) {
  SmallVector<std::pair<const BasicBlock *, const BasicBlock *>> Backedges;
  FindFunctionBackedges(F, Backedges);
  return !Backedges.empty();
}

/// A function that F calls and whose body Alive2 cannot see.
std::optional<std::string> opaqueCall(Function &F) {
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

/// Reads Alive2's report: why it failed, and a counterexample's arguments.
void readReport(StringRef Text, const Contract &K, ProofResult &R) {
  SmallVector<StringRef> Lines;
  Text.split(Lines, '\n');
  for (size_t I = 0; I < Lines.size(); ++I) {
    StringRef Line = Lines[I].trim();
    if (R.Verdict.empty() && Line.consume_front("ERROR: "))
      R.Verdict = Line.str();
    if (Line != "Example:")
      continue;
    std::vector<std::optional<U128>> Values(K.ParamList.size());
    for (size_t J = I + 1; J < Lines.size() && !Lines[J].trim().empty(); ++J) {
      // <type> %<name> = #x<hex> ...
      auto [Lhs, Rhs] = Lines[J].split(" = ");
      StringRef Name = Lhs.substr(Lhs.find('%') + 1).trim();
      StringRef Hex = Rhs.trim();
      if (!Hex.consume_front("#x"))
        continue;
      Hex = Hex.take_while(isHexDigit);
      for (size_t A = 0; A < K.ParamList.size(); ++A)
        if (K.ParamList[A].Name == Name) {
          U128 V = 0;
          for (char Ch : Hex)
            V = V << 4 | hexDigitValue(Ch);
          Values[A] = V;
        }
    }
    if (llvm::all_of(Values, [](const auto &V) { return V.has_value(); }))
      for (const auto &V : Values)
        R.Inputs.push_back(*V);
    break;
  }
  if (R.Verdict.empty()) {
    for (StringRef Line : Lines)
      if (!Line.trim().empty() && !Line.contains("Transformation")) {
        R.Verdict = Line.trim().str();
        break;
      }
  }
}

} // namespace

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

  if (Error E = simplify(*M, Src, Tgt))
    return std::move(E);
  std::string Broken;
  raw_string_ostream BrokenOS(Broken);
  if (verifyModule(*M, &BrokenOS))
    return createStringError("%s: the proof is not valid IR: %s",
                             P.Where.c_str(), Broken.c_str());
  for (Function *F : {Src, Tgt}) {
    if (auto Callee = opaqueCall(*F))
      return createStringError("%s: the proof calls %s, whose body Alive2 "
                               "cannot see",
                               P.Where.c_str(), Callee->c_str());
    if (!K.Unroll && hasLoop(*F))
      return createStringError("%s: the proof has a loop; give the iterations "
                               "to follow with 'prove unroll N'",
                               P.Where.c_str());
  }

  // Registers hold bits; a value cannot come in as poison or undef.
  util::config::disable_poison_input = true;
  util::config::disable_undef_input = true;
  util::config::src_unroll_cnt = K.Unroll;
  util::config::tgt_unroll_cnt = K.Unroll;
  smt::set_query_timeout(std::to_string(uint64_t(O.Timeout) * 1000));
  smt::set_memory_limit(uint64_t(O.Memory) << 20);

  std::ostringstream Out;
  TargetLibraryInfoWrapperPass TLI(M->getTargetTriple());
  llvm_util::initializer LLVMUtil(Out, M->getDataLayout());
  smt::smt_initializer SMT;
  llvm_util::Verifier V(TLI, SMT, Out);
  auto Start = std::chrono::steady_clock::now();
  try {
    V.compareFunctions(*Src, *Tgt);
  } catch (...) {
    return createStringError("%s: Alive2 failed: %s", P.Where.c_str(),
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
    readReport(Out.str(), K, R);
  return R;
}
