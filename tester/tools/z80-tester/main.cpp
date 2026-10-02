// z80-tester: runs lifted runtime functions against their references.

#include "z80tester/Tester.h"

#include "z80lift/Lifter.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

cl::SubCommand Check("check", "Test runtime functions against refs/");
cl::SubCommand EmitTV("emit-tv", "Write @src and @tgt for alive-tv");

cl::opt<std::string> ImagePath(cl::Positional, cl::Required,
                               cl::desc("<image>"), cl::sub(Check),
                               cl::sub(EmitTV));

cl::list<std::string> Names(cl::Positional,
                            cl::desc("<function>... (default: every spec)"),
                            cl::sub(Check), cl::sub(EmitTV));

cl::opt<Cpu> CpuFlag("cpu", cl::desc("CPU of the program"),
                     cl::values(clEnumValN(Cpu::Z80, "z80", "Z80 (default)"),
                                clEnumValN(Cpu::SM83, "sm83", "SM83")),
                     cl::init(Cpu::Z80), cl::sub(cl::SubCommand::getAll()));

cl::opt<std::string> SpecPath("spec",
                              cl::desc("Spec file (default: specs/<cpu>.yaml)"),
                              cl::sub(Check), cl::sub(EmitTV));

enum class Engine { Jit, Interp };
cl::opt<Engine> EngineFlag(
    "engine", cl::desc("How to run the runtime functions"),
    cl::values(clEnumValN(Engine::Jit, "jit", "lifted and JIT-compiled"),
               clEnumValN(Engine::Interp, "interp", "core's interpreter")),
    cl::init(Engine::Jit), cl::sub(Check));

cl::opt<unsigned> Threads("threads",
                          cl::desc("Worker threads (default: all cores)"),
                          cl::init(0), cl::sub(Check));
cl::opt<unsigned> MaxBits(
    "max-exhaustive-bits",
    cl::desc("Try every input up to this many input bits, sample above"),
    cl::init(32), cl::sub(Check));
cl::opt<uint64_t> Samples("samples", cl::desc("Inputs to sample"),
                          cl::init(1 << 24), cl::sub(Check));
cl::opt<uint64_t> Seed("seed", cl::desc("Random seed"), cl::init(1),
                       cl::sub(Check));
cl::opt<uint64_t> StepLimit("step-limit",
                            cl::desc("Instructions a call may run"),
                            cl::init(100000), cl::sub(Check));
cl::opt<unsigned> Reports("reports", cl::desc("Failures to show per function"),
                          cl::init(10), cl::sub(Check));

[[noreturn]] void fail(Error E) {
  WithColor::error(errs(), "z80-tester") << toString(std::move(E)) << '\n';
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

/// The specs named on the command line, or all of them.
std::vector<FunctionSpec> selectSpecs(Cpu C) {
  std::string Path = SpecPath.empty() ? std::string(Z80TESTER_SPEC_DIR) + "/" +
                                            cpuName(C) + ".yaml"
                                      : SpecPath.getValue();
  std::vector<FunctionSpec> All = check(loadSpecs(Path));
  if (Names.empty())
    return All;
  std::vector<FunctionSpec> Picked;
  for (const std::string &Name : Names) {
    auto It = llvm::find_if(
        All, [&](const FunctionSpec &S) { return S.Name == Name; });
    if (It == All.end())
      check(createStringError("%s has no spec for %s", Path.c_str(),
                              Name.c_str()));
    Picked.push_back(*It);
  }
  return Picked;
}

int runCheck() {
  Cpu C = CpuFlag;
  Image Img = check(Image::load(ImagePath));
  std::vector<FunctionSpec> Specs = selectSpecs(C);

  std::unique_ptr<Jit> J = check(Jit::create());
  std::set<std::string> RefNames;
  for (const FunctionSpec &S : Specs)
    RefNames.insert(S.refName());
  std::map<std::string, RefSig> Sigs = check(
      J->addRefs(std::vector<std::string>(RefNames.begin(), RefNames.end())));

  std::vector<TestPlan> Plans;
  for (const FunctionSpec &S : Specs) {
    TestPlan P = check(planTest(C, Img, S, Sigs.at(S.refName())));
    P.Ref = check(J->ref(S.refName()));
    if (Sigs.at(S.refName()).HasPre)
      P.Pre = check(J->pre(S.refName()));
    Plans.push_back(std::move(P));
  }

  if (EngineFlag == Engine::Jit) {
    auto Ctx = std::make_unique<LLVMContext>();
    auto L = check(z80lift::Lifter::create(C, Img, *Ctx));
    for (const TestPlan &P : Plans)
      check(L->lift(P.Entry));
    L->optimize();
    check(J->addLifted(L->takeModule(), std::move(Ctx)));
    for (TestPlan &P : Plans)
      P.Fn = check(J->lifted(Img.nameAt(P.Entry)));
  }

  TestOptions O;
  O.Threads = Threads;
  O.MaxExhaustiveBits = MaxBits;
  O.Samples = Samples;
  O.Seed = Seed;
  O.StepLimit = StepLimit;
  O.MaxReports = Reports;

  bool AllOk = true;
  for (const TestPlan &P : Plans) {
    TestResult R = runTest(P, Img, O);
    bool Ok = R.Mismatches == 0 && R.Faults == 0;
    AllOk &= Ok;
    outs() << formatv("{0,-16} {1,12} {2,-9} {3,12} checked  ", P.Name,
                      R.Inputs, R.Exhaustive ? "(all)" : "(sampled)",
                      R.Checked);
    if (Ok)
      WithColor(outs(), raw_ostream::GREEN) << "ok  ";
    else
      WithColor(outs(), raw_ostream::RED, /*Bold=*/true) << "FAIL";
    outs() << formatv("  max {0} steps\n", R.MaxSteps);
    if (!Ok) {
      WithColor(outs(), raw_ostream::RED)
          << formatv("  {0} mismatches, {1} bad calls", R.Mismatches, R.Faults);
      outs() << '\n';
    }
    for (const std::string &Rep : R.Reports)
      outs() << "  " << Rep << '\n';
    outs().flush();
  }
  return AllOk ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv,
                              "Tests the llvm-z80 runtime against refs/\n");
  if (Check)
    return runCheck();
  if (EmitTV) {
    WithColor::error(errs(), "z80-tester")
        << "emit-tv is not implemented yet\n";
    return 1;
  }
  cl::PrintHelpMessage();
  return 1;
}
