// z80-test: runs lifted runtime functions against the contracts in their
// assembly.

#include "z80tester/Tester.h"
#ifdef Z80TESTER_ALIVE2
#include "z80tester/Prover.h"
#endif
#ifdef Z80TESTER_ASSEMBLER
#include "z80tester/Assembler.h"
#endif

#include "z80lift/Lifter.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

cl::OptionCategory Category("z80-test options");

cl::opt<std::string>
    InputPath(cl::Positional, cl::Required,
              cl::desc("<linked image, or assembly file or directory>"),
              cl::cat(Category));

cl::list<std::string>
    Names(cl::Positional,
          cl::desc("<function>... (default: every function with a contract)"),
          cl::cat(Category));

cl::opt<Cpu> CpuFlag("cpu", cl::desc("CPU of the program"),
                     cl::values(clEnumValN(Cpu::Z80, "z80", "Z80 (default)"),
                                clEnumValN(Cpu::SM83, "sm83", "SM83")),
                     cl::init(Cpu::Z80), cl::cat(Category));

cl::list<std::string>
    ContractPaths("contracts",
                  cl::desc("A file of contracts, or a directory whose *.asm "
                           "files hold them"),
                  cl::cat(Category));

cl::opt<std::string> ClangPath("clang",
                               cl::desc("clang to compile the contracts with"),
                               cl::init(Z80TESTER_CLANG), cl::cat(Category));

enum class Engine { Jit, Interp };
cl::opt<Engine> EngineFlag(
    "engine", cl::desc("How to run the runtime functions"),
    cl::values(clEnumValN(Engine::Jit, "jit", "lifted and JIT-compiled"),
               clEnumValN(Engine::Interp, "interp", "core's interpreter")),
    cl::init(Engine::Jit), cl::cat(Category));

cl::opt<unsigned> Threads("threads",
                          cl::desc("Worker threads (default: all cores)"),
                          cl::init(0), cl::cat(Category));
// The contract's `tests` take precedence over these.
cl::opt<unsigned> MaxBits(
    "max-exhaustive-bits",
    cl::desc("Try every input up to this many input bits, sample above"),
    cl::init(32), cl::cat(Category));
cl::opt<uint64_t> Samples("samples", cl::desc("Inputs to sample"),
                          cl::init(1 << 24), cl::cat(Category));
cl::opt<uint64_t> Seed("seed", cl::desc("Random seed"), cl::init(1),
                       cl::cat(Category));
cl::opt<uint64_t> StepLimit("step-limit",
                            cl::desc("Instructions a call may run"),
                            cl::init(100000), cl::cat(Category));
cl::opt<unsigned> Reports("reports", cl::desc("Failures to show per function"),
                          cl::init(10), cl::cat(Category));
cl::opt<unsigned> SmtTimeout("smt-timeout",
                             cl::desc("Seconds for each query of a proof"),
                             cl::init(600), cl::cat(Category));
cl::opt<unsigned> SmtMemory("smt-memory",
                            cl::desc("Megabytes the SMT solver may use"),
                            cl::init(16384), cl::cat(Category));

[[noreturn]] void fail(Error E) {
  WithColor::error(errs(), "z80-test") << toString(std::move(E)) << '\n';
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

/// The C name of an assembler symbol.
std::string cName(StringRef Asm) {
  return Asm.starts_with("_") ? Asm.drop_front().str() : Asm.str();
}

bool isAssembly(StringRef Path) {
  StringRef Ext = sys::path::extension(Path);
  return sys::fs::is_directory(Path) || Ext == ".asm" || Ext == ".s";
}

/// The *.asm files of a directory, sorted, by their real paths.
Expected<std::vector<std::string>> asmFiles(StringRef Dir) {
  std::vector<std::string> Files;
  std::error_code EC;
  for (sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC)) {
    SmallString<128> Real;
    if (sys::path::extension(It->path()) == ".asm" &&
        !sys::fs::real_path(It->path(), Real))
      Files.push_back(Real.str().str());
  }
  if (EC)
    return createStringError(EC, "%s: %s", Dir.str().c_str(),
                             EC.message().c_str());
  llvm::sort(Files);
  return Files;
}

/// The contracts named on the command line, or every one whose function is
/// there to test.
std::vector<Contract> selectContracts(function_ref<bool(StringRef)> Defined,
                                      const std::vector<Contract> &All) {
  std::vector<Contract> Picked;
  if (Names.empty()) {
    for (const Contract &K : All) {
      if (Defined(K.Name))
        Picked.push_back(K);
      else
        WithColor::warning(errs(), "z80-test")
            << K.where() << ": no function " << K.Name << '\n';
    }
    return Picked;
  }
  for (const std::string &Name : Names) {
    auto It =
        llvm::find_if(All, [&](const Contract &K) { return K.Name == Name; });
    if (It == All.end())
      check(createStringError("no contract for %s", Name.c_str()));
    Picked.push_back(*It);
  }
  return Picked;
}

/// The public functions among Globals that have no contract.
std::vector<std::string> uncovered(const std::vector<std::string> &Globals,
                                   const std::vector<Contract> &All) {
  std::set<std::string> Covered;
  for (const Contract &K : All)
    Covered.insert(K.Name);
  std::vector<std::string> Out;
  for (const std::string &Asm : Globals)
    if (!Covered.count(cName(Asm)))
      Out.push_back(cName(Asm));
  return Out;
}

int runCheck() {
  Cpu C = CpuFlag;
  // Contracts come from the sources under test and from --contracts. Sources
  // are linked once the functions to test are known.
  std::vector<std::string> Sources;
  std::vector<std::string> Published; // global symbols to have contracts
  std::optional<Image> Img;
#ifdef Z80TESTER_ASSEMBLER
  std::optional<AsmLibrary> Lib;
#endif
  if (isAssembly(InputPath)) {
#ifdef Z80TESTER_ASSEMBLER
    SmallString<128> Input;
    if (std::error_code EC = sys::fs::real_path(InputPath, Input))
      fail(createStringError(EC, "%s: %s", InputPath.c_str(),
                             EC.message().c_str()));
    bool Dir = sys::fs::is_directory(Input);
    StringRef DirPath = Dir ? StringRef(Input) : sys::path::parent_path(Input);
    std::vector<std::string> Files = check(asmFiles(DirPath));
    if (!Dir && !is_contained(Files, Input))
      Files.push_back(Input.str().str());
    Lib = check(AsmLibrary::assemble(C, Files));
    Sources.push_back(Input.str().str());
    for (const std::string &F :
         Dir ? Files : std::vector<std::string>{Input.str().str()})
      if (Lib->linkable(F))
        append_range(Published, Lib->globals(F));
    llvm::sort(Published);
#else
    fail(createStringError("%s: this z80-test cannot assemble; build it "
                           "with Z80LIFT_TESTER_ASSEMBLER, or give it a "
                           "linked image",
                           InputPath.c_str()));
#endif
  } else {
    if (ContractPaths.empty())
      fail(createStringError("a linked image needs --contracts"));
    Img = check(Image::load(InputPath));
    Published.assign(Img->Globals.begin(), Img->Globals.end());
  }
  append_range(Sources, ContractPaths);

  std::vector<Contract> All = check(loadContracts(Sources, C));
  std::vector<Contract> Contracts = selectContracts(
      [&](StringRef Name) {
#ifdef Z80TESTER_ASSEMBLER
        if (Lib)
          return Lib->defines(Name);
#endif
        return Img->lookup(Name).has_value();
      },
      All);
#ifdef Z80TESTER_ASSEMBLER
  if (Lib) {
    std::vector<std::string> Roots;
    for (const Contract &K : Contracts)
      Roots.push_back(K.Name);
    Img = check(Lib->link(Roots));
  }
#endif

  std::unique_ptr<MemoryBuffer> Bitcode =
      check(compileContracts(contractSource(Contracts), ClangPath));
  std::vector<std::string> Adapt, Sigs;
  for (size_t I = 0; I < Contracts.size(); ++I) {
    Sigs.push_back(signatureFunction(I));
    if (!Contracts[I].Requires.empty())
      Adapt.push_back(requiresFunction(I));
    for (size_t E = 0; E < Contracts[I].Ensures.size(); ++E)
      Adapt.push_back(ensuresFunction(I, E));
    for (size_t E = 0; E < Contracts[I].Examples.size(); ++E)
      for (size_t V = 0; V < Contracts[I].Examples[E].Values.size(); ++V)
        Adapt.push_back(exampleFunction(I, E, V));
    for (size_t R = 0; R < Contracts[I].Ranges.size(); ++R)
      for (bool Hi : {false, true})
        Adapt.push_back(rangeFunction(I, R, Hi));
    for (size_t D = 0; D < Contracts[I].Domains.size(); ++D)
      for (bool Hi : {false, true})
        Adapt.push_back(domainFunction(I, D, Hi));
  }
  std::unique_ptr<Jit> J = check(Jit::create());
  std::map<std::string, Signature> Signatures =
      check(J->addBitcode(Bitcode->getMemBufferRef(), Adapt, Sigs));

  std::vector<TestPlan> Plans;
  for (size_t I = 0; I < Contracts.size(); ++I) {
    const Contract &K = Contracts[I];
    TestPlan P = check(planTest(C, *Img, K, Signatures.at(Sigs[I])));
    if (!K.Requires.empty())
      P.Requires = {&K.Requires.front(), K.Requires.front().where(),
                    check(J->adapter(requiresFunction(I)))};
    for (size_t E = 0; E < K.Ensures.size(); ++E)
      P.Ensures.push_back({&K.Ensures[E], K.Ensures[E].where(),
                           check(J->adapter(ensuresFunction(I, E)))});
    for (size_t E = 0; E < K.Examples.size(); ++E) {
      ExampleCheck &EC = P.Examples.emplace_back();
      EC.Where = K.Examples[E].where();
      for (size_t V = 0; V < K.Examples[E].Values.size(); ++V)
        EC.Values.push_back({K.Examples[E].Values[V].first,
                             check(J->adapter(exampleFunction(I, E, V)))});
    }
    for (size_t R = 0; R < P.Ranges.size(); ++R) {
      P.Ranges[R].Lo = check(J->adapter(rangeFunction(I, R, false)));
      P.Ranges[R].Hi = check(J->adapter(rangeFunction(I, R, true)));
    }
    for (size_t D = 0; D < P.Domains.size(); ++D) {
      P.Domains[D].Lo = check(J->adapter(domainFunction(I, D, false)));
      P.Domains[D].Hi = check(J->adapter(domainFunction(I, D, true)));
    }
    check(finishPlan(P));
    Plans.push_back(std::move(P));
  }

  if (EngineFlag == Engine::Jit) {
    auto Ctx = std::make_unique<LLVMContext>();
    auto L = check(z80lift::Lifter::create(C, *Img, *Ctx));
    for (const TestPlan &P : Plans)
      check(L->lift(P.Entry));
    L->optimize();
    check(J->addLifted(L->takeModule(), std::move(Ctx)));
    for (TestPlan &P : Plans)
      P.Fn = check(J->lifted(Img->nameAt(P.Entry)));
  }

  TestOptions O;
  O.Threads = Threads;
  O.MaxExhaustiveBits = MaxBits;
  O.Samples = Samples;
  O.Seed = Seed;
  O.StepLimit = StepLimit;
  O.MaxReports = Reports;

#ifdef Z80TESTER_ALIVE2
  // Proofs read the contracts without the sanitizers, which they cannot see
  // through.
  std::unique_ptr<MemoryBuffer> ProofBitcode;
  if (llvm::any_of(Contracts, [](const Contract &K) { return K.Prove; }))
    ProofBitcode = check(compileContracts(contractSource(Contracts), ClangPath,
                                          /*Sanitize=*/false));
  ProofOptions PO;
  PO.Timeout = SmtTimeout;
  PO.Memory = SmtMemory;
  PO.StepLimit = StepLimit;
#endif

  size_t Width = 16;
  for (const TestPlan &P : Plans)
    Width = std::max(Width, P.Name.size());
  auto Status = [](bool Ok) {
    if (Ok)
      WithColor(outs(), raw_ostream::GREEN) << "ok  ";
    else
      WithColor(outs(), raw_ostream::RED, /*Bold=*/true) << "FAIL";
  };
  bool AllOk = true;
  for (size_t I = 0; I < Plans.size(); ++I) {
    const TestPlan &P = Plans[I];
    const Contract &K = Contracts[I];
    if (!P.OnlyExamples || !P.Examples.empty()) {
      TestResult R = runTest(P, *Img, O);
      bool Ok = R.Mismatches == 0 && R.Faults == 0;
      AllOk &= Ok;
      outs() << left_justify(P.Name, Width)
             << formatv(" {0,12} {1,-9} {2,12} checked  ", R.Inputs,
                        P.OnlyExamples ? "(example)"
                        : R.Exhaustive ? "(all)"
                                       : "(sampled)",
                        R.Checked);
      Status(Ok);
      outs() << formatv("  max {0} steps\n", R.MaxSteps);
      if (!Ok) {
        WithColor(outs(), raw_ostream::RED)
            << formatv("  {0} broken, {1} bad calls", R.Mismatches, R.Faults);
        outs() << '\n';
      }
      if (R.Unplaced)
        WithColor(outs(), raw_ostream::YELLOW)
            << formatv("  {0} inputs needed more memory than the tests have; "
                       "bound their sizes with `in`\n",
                       R.Unplaced);
      for (const std::string &Rep : R.Reports)
        outs() << "  " << Rep << '\n';
      outs().flush();
    }
    if (!K.Prove)
      continue;

    outs() << left_justify(P.Name, Width)
           << formatv(" {0,12} {1,-9} {2,12}          ", "", "(proof)", "");
#ifdef Z80TESTER_ALIVE2
    Expected<ProofResult> R =
        prove(P, K, I, *Img, ProofBitcode->getMemBufferRef(), PO);
    if (!R) {
      AllOk = false;
      Status(false);
      outs() << '\n';
      WithColor(outs(), raw_ostream::RED)
          << "  " << toString(R.takeError()) << '\n';
      outs().flush();
      continue;
    }
    bool Ok = R->S == ProofResult::Proved;
    AllOk &= Ok;
    Status(Ok);
    if (Ok) {
      outs() << formatv("  proved in {0:f1} s", R->Seconds);
      if (K.Unroll)
        outs() << formatv(", loops unrolled {0} times", K.Unroll);
      outs() << '\n';
    } else if (R->S == ProofResult::Unproven) {
      outs() << formatv("  not proved after {0:f1} s\n", R->Seconds);
      WithColor(outs(), raw_ostream::YELLOW)
          << "  Alive2: " << R->Verdict << '\n';
    } else {
      outs() << "  counterexample\n";
      // Run it, to show it like a failed test.
      std::vector<std::string> Reports;
      if (R->Inputs.size() == P.Params.size()) {
        ExampleValues Ex;
        Ex.Where = "Alive2";
        for (size_t A = 0; A < R->Inputs.size(); ++A)
          Ex.Values.push_back({unsigned(A), R->Inputs[A]});
        TestPlan Q = P;
        Q.OnlyExamples = true;
        Q.ExampleInputs = {Ex};
        Reports = runTest(Q, *Img, O).Reports;
      }
      WithColor(outs(), raw_ostream::RED) << "  Alive2: " << R->Verdict << '\n';
      for (const std::string &Rep : Reports)
        outs() << "  " << Rep << '\n';
      if (Reports.empty() && R->Inputs.size() == P.Params.size())
        outs() << "  the arguments pass when run, so it depends on what the "
                  "other registers hold\n";
    }
#else
    WithColor(outs(), raw_ostream::YELLOW)
        << "skipped: this z80-test was built without Alive2\n";
#endif
    outs().flush();
  }

  if (Names.empty()) {
    std::vector<std::string> Missing = uncovered(Published, All);
    if (!Missing.empty())
      WithColor(outs(), raw_ostream::YELLOW)
          << "no contract: " << join(Missing, " ") << '\n';
  }
  return AllOk ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  // Only our options; those of the linked LLVM libraries still work.
  cl::HideUnrelatedOptions(Category);
  cl::ParseCommandLineOptions(
      argc, argv, "Tests the llvm-z80 runtime against its contracts\n");
  return runCheck();
}
