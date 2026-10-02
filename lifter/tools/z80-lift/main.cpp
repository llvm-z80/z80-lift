// z80-lift: disassembles runtime functions and lifts them to LLVM IR.

#include "z80lift/CFG.h"
#include "z80lift/Lifter.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace z80core;
using namespace z80lift;

namespace {

cl::SubCommand Decode("decode", "Disassemble functions of a runtime image");
cl::SubCommand Lift("lift", "Print the lifted IR of functions");

cl::opt<std::string> ImagePath(cl::Positional, cl::Required,
                               cl::desc("<image>"), cl::sub(Decode),
                               cl::sub(Lift));

cl::list<std::string> Symbols(cl::Positional, cl::desc("<function>..."),
                              cl::sub(Decode), cl::sub(Lift));

cl::opt<Cpu> CpuFlag("cpu", cl::desc("CPU of the program"),
                     cl::values(clEnumValN(Cpu::Z80, "z80", "Z80 (default)"),
                                clEnumValN(Cpu::SM83, "sm83", "SM83")),
                     cl::init(Cpu::Z80), cl::sub(cl::SubCommand::getAll()));

cl::opt<bool> Raw("raw", cl::desc("Print the IR before optimization"),
                  cl::sub(Lift));

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

void printInst(Cpu C, const Image &Img, const Inst &I) {
  std::string Text;
  Inst Again;
  decode(C, Img.Mem.data(), I.Addr, Again, &Text);
  std::string Bytes;
  for (unsigned K = 0; K < I.Len; ++K)
    Bytes += formatv("{0:x-2} ", Img.Mem[uint16_t(I.Addr + K)]).str();
  WithColor(outs(), raw_ostream::BRIGHT_BLACK)
      << "  " << format_hex_no_prefix(I.Addr, 4) << ": "
      << left_justify(Bytes, 13);
  outs() << Text << '\n';
}

int runDecode() {
  Cpu C = CpuFlag;
  Image Img = check(Image::load(ImagePath));
  std::vector<uint16_t> Entries;
  if (Symbols.empty())
    Entries.assign(Img.Entries.begin(), Img.Entries.end());
  for (const std::string &Name : Symbols)
    Entries.push_back(lookup(Img, Name));

  std::map<uint16_t, Inst> Insts;
  for (uint16_t Entry : Entries) {
    CFG F = check(recoverCFG(C, Img, Entry));
    for (const auto &[A, B] : F.Blocks)
      for (const Inst &I : B.Insts)
        Insts[I.Addr] = I;
  }
  for (const auto &[A, I] : Insts) {
    if (auto It = Img.Names.find(A); It != Img.Names.end()) {
      WithColor(outs(), raw_ostream::YELLOW, /*Bold=*/true)
          << It->second << ':';
      outs() << '\n';
    }
    printInst(C, Img, I);
  }
  return 0;
}

int runLift() {
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

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Z80/SM83 machine code lifter\n");
  if (Decode)
    return runDecode();
  if (Lift)
    return runLift();
  cl::PrintHelpMessage();
  return 1;
}
