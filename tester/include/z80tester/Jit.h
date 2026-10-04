// JIT-compiles lifted functions and the contracts with ORC.

#ifndef Z80TESTER_JIT_H
#define Z80TESTER_JIT_H

#include "z80tester/CallConv.h"

#include "z80core/State.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace z80tester {

/// A function's parameter and result types, read from its IR.
struct Signature {
  std::vector<Ty> Params;
  std::optional<Ty> Ret;
};

/// Calls a function with every argument and the result in 16-byte slots.
using AdapterFn = void (*)(const void *Args, void *Ret);
using LiftedFn = void (*)(z80core::State *, uint8_t *);

class Jit {
public:
  static llvm::Expected<std::unique_ptr<Jit>> create();

  /// Adds a bitcode module with an adapter for each function in Adapt, and
  /// returns the signatures of those in Signatures.
  llvm::Expected<std::map<std::string, Signature>>
  addBitcode(llvm::MemoryBufferRef Bitcode, llvm::ArrayRef<std::string> Adapt,
             llvm::ArrayRef<std::string> Signatures);

  llvm::Error addLifted(std::unique_ptr<llvm::Module> M,
                        std::unique_ptr<llvm::LLVMContext> Ctx);

  llvm::Expected<LiftedFn> lifted(llvm::StringRef Name);
  llvm::Expected<AdapterFn> adapter(llvm::StringRef Name);

private:
  std::unique_ptr<llvm::orc::LLJIT> J;
};

} // namespace z80tester

#endif // Z80TESTER_JIT_H
