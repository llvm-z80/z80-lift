// Turns a function's control flow into LLVM IR that calls the semantics.

#ifndef Z80LIFT_LIFTER_H
#define Z80LIFT_LIFTER_H

#include "z80lift/CFG.h"

#include "llvm/IR/Module.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class IRBuilderBase;
} // namespace llvm

namespace z80lift {

/// Lifts runtime functions into one module as `void @name(ptr %state,
/// ptr %mem)`. A lifted function runs until control leaves it, then returns
/// with PC holding the address execution continues at. An argument read from
/// the bytes of an undefined symbol is built from a call to
/// `i32 @"z80lift.symbol.<name>"()`, which stands for the symbol's value.
class Lifter {
public:
  /// The name of the function that stands for the value of Symbol.
  static std::string symbolFunction(llvm::StringRef Symbol);

  static llvm::Expected<std::unique_ptr<Lifter>>
  create(z80core::Cpu C, const z80core::Image &Img, llvm::LLVMContext &Ctx);

  /// Lifts the function at Entry and every function it reaches.
  llvm::Expected<llvm::Function *> lift(uint16_t Entry);

  /// Makes a conditional return that is taken leave the function even when it
  /// returns to the next instruction, as for code that is not at the address
  /// it runs at. Applies to the functions lifted after it.
  void setTakenReturnsLeave(bool V) { TakenReturnsLeave = V; }

  /// Makes a jump or call to an address past the image leave the function
  /// for it, rather than fail for want of code there.
  void setJumpsOutsideLeave(bool V) { JumpsOutsideLeave = V; }

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
  llvm::Value *argValue(llvm::IRBuilderBase &B, const z80core::Inst &I,
                        unsigned K);

  z80core::Cpu C;
  const z80core::Image &Img;
  std::unique_ptr<llvm::Module> M;
  llvm::Function *GetPC = nullptr, *SetPC = nullptr, *GetSP = nullptr,
                 *Tick = nullptr;
  bool TakenReturnsLeave = false;
  bool JumpsOutsideLeave = false;
  std::vector<llvm::Function *> Sem; // indexed by Op
  std::map<uint16_t, llvm::Function *> Lifted;
};

} // namespace z80lift

#endif // Z80LIFT_LIFTER_H
