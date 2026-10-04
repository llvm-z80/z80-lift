// JIT-compiles lifted functions and the contracts with ORC.

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

Expected<std::map<std::string, Signature>>
Jit::addBitcode(MemoryBufferRef Bitcode, ArrayRef<std::string> Adapt,
                ArrayRef<std::string> Signatures) {
  auto Ctx = std::make_unique<LLVMContext>();
  auto M = parseBitcodeFile(Bitcode, *Ctx);
  if (!M)
    return M.takeError();

  auto Get = [&](StringRef Name) -> Expected<llvm::Function *> {
    llvm::Function *F = (*M)->getFunction(Name);
    if (!F || F->isDeclaration())
      return createStringError("%s: no function %s",
                               Bitcode.getBufferIdentifier().str().c_str(),
                               Name.str().c_str());
    return F;
  };

  std::map<std::string, Signature> Sigs;
  for (const std::string &Name : Signatures) {
    auto F = Get(Name);
    if (!F)
      return F.takeError();
    Signature Sig;
    for (Type *P : (*F)->getFunctionType()->params()) {
      std::optional<Ty> T = tyFromIR(P);
      if (!T)
        return createStringError("%s: unsupported parameter type",
                                 Name.c_str());
      Sig.Params.push_back(*T);
    }
    if (!(*F)->getReturnType()->isVoidTy()) {
      Sig.Ret = tyFromIR((*F)->getReturnType());
      if (!Sig.Ret)
        return createStringError("%s: unsupported result type", Name.c_str());
    }
    Sigs[Name] = Sig;
  }
  for (const std::string &Name : Adapt) {
    auto F = Get(Name);
    if (!F)
      return F.takeError();
    emitAdapter(**F);
  }

  if (Error E =
          J->addIRModule(orc::ThreadSafeModule(std::move(*M), std::move(Ctx))))
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

Expected<AdapterFn> Jit::adapter(StringRef Name) {
  auto Addr = J->lookup(("z80tester.adapt." + Name).str());
  if (!Addr)
    return Addr.takeError();
  return Addr->toPtr<AdapterFn>();
}
