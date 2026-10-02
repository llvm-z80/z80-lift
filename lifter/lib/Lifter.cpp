// Emits one IR function per runtime function. Each instruction becomes a call
// to its semantics; each block ends by dispatching on PC to the successors
// known statically, and returns to the caller for anything else.

#include "z80lift/Lifter.h"
#include "z80core/Semantics.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Transforms/IPO/GlobalDCE.h"

using namespace llvm;
using namespace z80core;
using namespace z80lift;

Expected<std::unique_ptr<Lifter>> Lifter::create(Cpu C, const Image &Img,
                                                 LLVMContext &Ctx) {
  auto Sem =
      parseBitcodeFile(MemoryBufferRef(semanticsBitcode(), "semantics"), Ctx);
  if (!Sem)
    return Sem.takeError();
  std::unique_ptr<Lifter> L(new Lifter(C, Img, std::move(*Sem)));
  Module &M = *L->M;
  M.setModuleIdentifier("lifted");

  // The semantics are inlined everywhere and dropped once unused.
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    F.setLinkage(GlobalValue::InternalLinkage);
    F.removeFnAttr(Attribute::NoInline);
    F.addFnAttr(Attribute::AlwaysInline);
  }

  auto Get = [&](StringRef Name) -> Expected<llvm::Function *> {
    if (llvm::Function *F = M.getFunction(Name))
      return F;
    return createStringError("semantics lack %s", Name.str().c_str());
  };
  for (auto [Dst, Name] : {std::pair{&L->GetPC, GetPCFunction},
                           {&L->SetPC, SetPCFunction},
                           {&L->Tick, TickFunction}}) {
    auto F = Get(Name);
    if (!F)
      return F.takeError();
    *Dst = *F;
  }
  for (unsigned Op = 0; Op < numOps(C); ++Op) {
    auto F = Get(semanticsFunction(C, Op));
    if (!F)
      return F.takeError();
    L->Sem.push_back(*F);
  }
  return L;
}

Expected<llvm::Function *> Lifter::lift(uint16_t Entry) {
  if (auto It = Lifted.find(Entry); It != Lifted.end())
    return It->second;
  Expected<CFG> F = recoverCFG(C, Img, Entry);
  if (!F)
    return F.takeError();

  LLVMContext &Ctx = M->getContext();
  auto *Ptr = PointerType::getUnqual(Ctx);
  auto *FTy = FunctionType::get(Type::getVoidTy(Ctx), {Ptr, Ptr}, false);
  auto *Fn = llvm::Function::Create(FTy, GlobalValue::ExternalLinkage,
                                    Img.nameAt(Entry), *M);
  Fn->getArg(0)->setName("state");
  Fn->getArg(1)->setName("mem");
  Fn->addParamAttr(0, Attribute::NoAlias);
  Fn->addParamAttr(1, Attribute::NoAlias);
  Fn->addFnAttr(Attribute::NoUnwind);
  Lifted[Entry] = Fn;

  for (uint16_t Callee : F->Callees)
    if (auto R = lift(Callee); !R)
      return R.takeError();
  if (Error E = buildBody(*F, Fn))
    return std::move(E);
  return Fn;
}

Error Lifter::buildBody(const CFG &F, llvm::Function *Fn) {
  LLVMContext &Ctx = M->getContext();
  IRBuilder<> B(Ctx);
  Value *S = Fn->getArg(0), *Mem = Fn->getArg(1);

  auto *EntryBB = BasicBlock::Create(Ctx, "entry", Fn);
  auto *Exit = BasicBlock::Create(Ctx, "exit", Fn);
  ReturnInst::Create(Ctx, Exit);
  std::map<uint16_t, BasicBlock *> BBs;
  for (const auto &[A, Blk] : F.Blocks)
    BBs[A] = BasicBlock::Create(Ctx, formatv("b{0:x-4}", A).str(), Fn);
  BranchInst::Create(BBs.at(F.Entry), EntryBB);

  // A block of this function, or a tail call into another one.
  auto Succ = [&](uint16_t A) -> Expected<BasicBlock *> {
    if (auto It = BBs.find(A); It != BBs.end())
      return It->second;
    auto Callee = Lifted.find(A);
    if (Callee == Lifted.end())
      return createStringError("%s: no code at 0x%04x",
                               Fn->getName().str().c_str(), A);
    auto *BB = BasicBlock::Create(Ctx, formatv("tail{0:x-4}", A).str(), Fn);
    IRBuilder<> TB(BB);
    TB.CreateCall(Callee->second, {S, Mem});
    TB.CreateRetVoid();
    return BB;
  };

  auto Dispatch = [&](ArrayRef<std::pair<uint16_t, BasicBlock *>> Cases) {
    Value *PC = B.CreateCall(GetPC, {S});
    SwitchInst *Sw = B.CreateSwitch(PC, Exit, Cases.size());
    std::set<uint16_t> Seen;
    for (auto [A, BB] : Cases)
      if (Seen.insert(A).second)
        Sw->addCase(B.getInt32(A), BB);
  };

  for (const auto &[A, Blk] : F.Blocks) {
    B.SetInsertPoint(BBs[A]);
    Value *Stop = B.CreateCall(Tick, {S, B.getInt32(Blk.Insts.size())});
    auto *Body = BasicBlock::Create(Ctx, "", Fn);
    B.CreateCondBr(Stop, Exit, Body);
    B.SetInsertPoint(Body);
    for (const Inst &I : Blk.Insts) {
      B.CreateCall(SetPC, {S, B.getInt32(I.next())});
      B.CreateCall(Sem[I.Op], {S, Mem, B.getInt32(I.Args[0]),
                               B.getInt32(I.Args[1]), B.getInt32(I.Args[2])});
    }

    const Inst &Last = Blk.Insts.back();
    uint16_t Next = Last.next();
    switch (Last.K) {
    case Kind::Seq:
    case Kind::Jump: {
      auto To = Succ(Last.K == Kind::Seq ? Next : Last.Dest);
      if (!To)
        return To.takeError();
      B.CreateBr(*To);
      break;
    }
    case Kind::CondJump: {
      auto Taken = Succ(Last.Dest), Fall = Succ(Next);
      if (!Taken)
        return Taken.takeError();
      if (!Fall)
        return Fall.takeError();
      Dispatch({{Next, *Fall}, {Last.Dest, *Taken}});
      break;
    }
    case Kind::Call:
    case Kind::CondCall: {
      auto Fall = Succ(Next);
      if (!Fall)
        return Fall.takeError();
      if (Last.K == Kind::CondCall) {
        auto *CallBB = BasicBlock::Create(Ctx, "", Fn);
        Dispatch({{Next, *Fall}, {Last.Dest, CallBB}});
        B.SetInsertPoint(CallBB);
      }
      B.CreateCall(Lifted.at(Last.Dest), {S, Mem});
      Dispatch({{Next, *Fall}});
      break;
    }
    case Kind::CondRet: {
      auto Fall = Succ(Next);
      if (!Fall)
        return Fall.takeError();
      Dispatch({{Next, *Fall}});
      break;
    }
    case Kind::Ret:
    case Kind::JumpInd:
    case Kind::Halt: B.CreateBr(Exit); break;
    }
  }
  return Error::success();
}

void Lifter::prune() {
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  ModulePassManager MPM;
  MPM.addPass(GlobalDCEPass());
  MPM.run(*M, MAM);
}

void Lifter::optimize() {
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
  PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2).run(*M, MAM);
}
