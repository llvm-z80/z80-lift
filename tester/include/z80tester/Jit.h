// JIT-compiles lifted functions and the reference implementations with ORC.

#ifndef Z80TESTER_JIT_H
#define Z80TESTER_JIT_H

#include "z80core/State.h"
#include "z80tester/CallConv.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace z80tester {

/// The references in refs/, compiled to bitcode.
llvm::StringRef refsBitcode();

/// A reference function's signature, read from its IR.
struct RefSig {
  std::vector<Ty> Params;
  std::optional<Ty> Ret;
  bool HasPre = false;
};

/// Calls a reference with every argument and the result in 16-byte slots.
using AdapterFn = void (*)(const void *Args, void *Ret);
using LiftedFn = void (*)(z80core::State *, uint8_t *);

class Jit {
public:
  static llvm::Expected<std::unique_ptr<Jit>> create();

  /// Adds refs/ with adapters for ref_<Name> and pre_<Name> of each name.
  llvm::Expected<std::map<std::string, RefSig>>
  addRefs(llvm::ArrayRef<std::string> Names);

  llvm::Error addLifted(std::unique_ptr<llvm::Module> M,
                        std::unique_ptr<llvm::LLVMContext> Ctx);

  llvm::Expected<LiftedFn> lifted(llvm::StringRef Name);
  llvm::Expected<AdapterFn> ref(llvm::StringRef Name);
  llvm::Expected<AdapterFn> pre(llvm::StringRef Name);

private:
  std::unique_ptr<llvm::orc::LLJIT> J;
};

} // namespace z80tester

#endif // Z80TESTER_JIT_H
