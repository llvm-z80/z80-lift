// z80-lift: lifts runtime functions to LLVM IR.

#include "z80lift/Lifter.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace z80core;
using namespace z80lift;

namespace {

cl::OptionCategory Category("z80-lift options");

cl::opt<std::string> ImagePath(cl::Positional, cl::Required,
                               cl::desc("<image>"), cl::cat(Category));

cl::list<std::string> Symbols(cl::Positional, cl::OneOrMore,
                              cl::desc("<function>..."), cl::cat(Category));

cl::opt<Cpu> CpuFlag("cpu", cl::desc("CPU of the program"),
                     cl::values(clEnumValN(Cpu::Z80, "z80", "Z80 (default)"),
                                clEnumValN(Cpu::SM83, "sm83", "SM83")),
                     cl::init(Cpu::Z80), cl::cat(Category));

cl::opt<bool> Raw("raw", cl::desc("Print the IR before optimization"),
                  cl::cat(Category));

[[noreturn]] void fail(Error E) {
  WithColor::error(errs(), "z80-lift") << toString(std::move(E)) << '\n';
  std::exit(1);
}

void check(Error E) {
  if (E)
    fail(std::move(E));
}

template <typename T> T check(Expected<T> V) {
  if (!V)
    fail(V.takeError());
  return std::move(*V);
}

uint16_t lookup(const Image &Img, StringRef Name) {
  if (std::optional<uint16_t> A = Img.lookup(Name))
    return *A;
  check(createStringError("no function %s", Name.str().c_str()));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  // Only our options; those of the linked LLVM libraries still work.
  cl::HideUnrelatedOptions(Category);
  cl::ParseCommandLineOptions(argc, argv, "Z80/SM83 machine code lifter\n");

  Cpu C = CpuFlag;
  Image Img = check(Image::load(ImagePath));
  LLVMContext Ctx;
  std::unique_ptr<Lifter> L = check(Lifter::create(C, Img, Ctx));
  for (const std::string &Name : Symbols)
    check(L->lift(lookup(Img, Name)));
  if (Raw)
    L->prune();
  else
    L->optimize();
  if (verifyModule(L->module(), &errs()))
    return 1;
  L->module().print(outs(), nullptr);
  return 0;
}
