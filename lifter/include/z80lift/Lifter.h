// Turns a function's control flow into LLVM IR that calls the semantics.

#ifndef Z80LIFT_LIFTER_H
#define Z80LIFT_LIFTER_H

#include "z80lift/CFG.h"

#include "llvm/IR/Module.h"

#include <map>
#include <memory>
#include <vector>

namespace z80lift {

/// Lifts runtime functions into one module as `void @name(ptr %state,
/// ptr %mem)`. A lifted function runs until control leaves it, then returns
/// with PC holding the address execution continues at.
class Lifter {
public:
  static llvm::Expected<std::unique_ptr<Lifter>>
  create(z80core::Cpu C, const z80core::Image &Img, llvm::LLVMContext &Ctx);

  /// Lifts the function at Entry and every function it reaches.
  llvm::Expected<llvm::Function *> lift(uint16_t Entry);

  /// Drops the semantics nothing calls.
  void prune();

  /// Inlines the semantics and optimizes at -O2.
  void optimize();

  llvm::Module &module() { return *M; }
  std::unique_ptr<llvm::Module> takeModule() { return std::move(M); }

private:
  Lifter(z80core::Cpu C, const z80core::Image &Img,
         std::unique_ptr<llvm::Module> M)
      : C(C), Img(Img), M(std::move(M)) {}

  llvm::Error buildBody(const CFG &F, llvm::Function *Fn);

  z80core::Cpu C;
  const z80core::Image &Img;
  std::unique_ptr<llvm::Module> M;
  llvm::Function *GetPC = nullptr, *SetPC = nullptr, *Tick = nullptr;
  std::vector<llvm::Function *> Sem; // indexed by Op
  std::map<uint16_t, llvm::Function *> Lifted;
};

} // namespace z80lift

#endif // Z80LIFT_LIFTER_H
