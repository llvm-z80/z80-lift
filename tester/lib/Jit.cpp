// JIT-compiles lifted functions and the references with ORC.

#include "z80tester/Jit.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/TargetSelect.h"

using namespace llvm;
using namespace z80tester;

Expected<std::unique_ptr<Jit>> Jit::create() {
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  auto J = orc::LLJITBuilder().create();
  if (!J)
    return J.takeError();
  auto Result = std::make_unique<Jit>();
  Result->J = std::move(*J);
  return Result;
}

namespace {

/// Emits `void @z80tester.adapt.<F>(ptr %args, ptr %ret)` around F.
void emitAdapter(llvm::Function &F) {
  Module &M = *F.getParent();
  LLVMContext &Ctx = M.getContext();
  auto *Ptr = PointerType::getUnqual(Ctx);
  auto *A = llvm::Function::Create(
      FunctionType::get(Type::getVoidTy(Ctx), {Ptr, Ptr}, false),
      GlobalValue::ExternalLinkage, "z80tester.adapt." + F.getName(), M);
  IRBuilder<> B(BasicBlock::Create(Ctx, "", A));
  SmallVector<Value *> Args;
  for (Argument &Arg : F.args()) {
    Value *Slot =
        B.CreateConstGEP1_64(B.getInt8Ty(), A->getArg(0), Arg.getArgNo() * 16);
    Args.push_back(B.CreateLoad(Arg.getType(), Slot));
  }
  CallInst *Call = B.CreateCall(&F, Args);
  Call->setAttributes(F.getAttributes());
  Value *R = Call;
  if (!R->getType()->isVoidTy()) {
    if (R->getType()->isIntegerTy(1))
      R = B.CreateZExt(R, B.getInt8Ty());
    B.CreateStore(R, A->getArg(1));
  }
  B.CreateRetVoid();
}

} // namespace

Expected<std::map<std::string, RefSig>>
Jit::addRefs(ArrayRef<std::string> Names) {
  auto Ctx = std::make_unique<LLVMContext>();
  auto Refs = parseBitcodeFile(MemoryBufferRef(refsBitcode(), "refs"), *Ctx);
  if (!Refs)
    return Refs.takeError();
  Module &M = **Refs;

  std::map<std::string, RefSig> Sigs;
  for (const std::string &Name : Names) {
    llvm::Function *Ref = M.getFunction("ref_" + Name);
    if (!Ref || Ref->isDeclaration())
      return createStringError("refs/ has no ref_%s", Name.c_str());
    RefSig Sig;
    for (Type *P : Ref->getFunctionType()->params()) {
      std::optional<Ty> T = tyFromIR(P);
      if (!T)
        return createStringError("ref_%s: unsupported parameter type",
                                 Name.c_str());
      Sig.Params.push_back(*T);
    }
    if (!Ref->getReturnType()->isVoidTy()) {
      Sig.Ret = tyFromIR(Ref->getReturnType());
      if (!Sig.Ret)
        return createStringError("ref_%s: unsupported result type",
                                 Name.c_str());
    }
    emitAdapter(*Ref);
    if (llvm::Function *Pre = M.getFunction("pre_" + Name);
        Pre && !Pre->isDeclaration()) {
      Sig.HasPre = true;
      emitAdapter(*Pre);
    }
    Sigs[Name] = Sig;
  }

  if (Error E = J->addIRModule(
          orc::ThreadSafeModule(std::move(*Refs), std::move(Ctx))))
    return std::move(E);
  return Sigs;
}

Error Jit::addLifted(std::unique_ptr<Module> M,
                     std::unique_ptr<LLVMContext> Ctx) {
  return J->addIRModule(orc::ThreadSafeModule(std::move(M), std::move(Ctx)));
}

Expected<LiftedFn> Jit::lifted(StringRef Name) {
  auto Addr = J->lookup(Name);
  if (!Addr)
    return Addr.takeError();
  return Addr->toPtr<LiftedFn>();
}

Expected<AdapterFn> Jit::ref(StringRef Name) {
  auto Addr = J->lookup(("z80tester.adapt.ref_" + Name).str());
  if (!Addr)
    return Addr.takeError();
  return Addr->toPtr<AdapterFn>();
}

Expected<AdapterFn> Jit::pre(StringRef Name) {
  auto Addr = J->lookup(("z80tester.adapt.pre_" + Name).str());
  if (!Addr)
    return Addr.takeError();
  return Addr->toPtr<AdapterFn>();
}
