// z80-test: runs lifted runtime functions against the contracts in their
// assembly, or proves rewrite rules.

#include "z80tester/Tester.h"
#ifdef Z80TESTER_ALIVE2
#include "z80tester/Prover.h"
#endif
#ifdef Z80TESTER_ASSEMBLER
#include "z80tester/Assembler.h"
#endif

#include "z80lift/Lifter.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <set>
#include <tuple>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

namespace {

cl::OptionCategory Category("z80-test options");

cl::opt<std::string>
    InputPath(cl::Positional, cl::Required,
              cl::desc("<linked image, assembly file or directory, or "
                       "rules file>"),
              cl::cat(Category));

cl::list<std::string>
    Names(cl::Positional,
          cl::desc("<function or rule>... (default: every function with a "
                   "contract, or every rule)"),
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
cl::opt<unsigned> Reports("reports",
                          cl::desc("Failures to show per function or rule"),
                          cl::init(10), cl::cat(Category));
cl::opt<unsigned>
    MaxFailures("max-failures",
                cl::desc("Failing cases after which a rule stops, or 0"),
                cl::init(0), cl::cat(Category));
cl::opt<unsigned> SmtTimeout("smt-timeout",
                             cl::desc("Seconds for each query of a proof"),
                             cl::init(600), cl::cat(Category));
cl::opt<unsigned> SmtMemory("smt-memory",
                            cl::desc("Megabytes the SMT solver may use"),
                            cl::init(16384), cl::cat(Category));

} // namespace

[[noreturn]] static void fail(Error E) {
  WithColor::error(errs(), "z80-test") << toString(std::move(E)) << '\n';
  std::exit(1);
}

static void check(Error E) {
  if (E)
    fail(std::move(E));
}

template <typename T> static T check(Expected<T> V) {
  if (!V)
    fail(V.takeError());
  return std::move(*V);
}

/// The C name of an assembler symbol.
static std::string cName(StringRef Asm) {
  return Asm.starts_with("_") ? Asm.drop_front().str() : Asm.str();
}

static bool isAssembly(StringRef Path) {
  StringRef Ext = sys::path::extension(Path);
  return sys::fs::is_directory(Path) || Ext == ".asm" || Ext == ".s";
}

/// The *.asm files of a directory, sorted, by their real paths.
static Expected<std::vector<std::string>> asmFiles(StringRef Dir) {
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
static std::vector<Contract>
selectContracts(function_ref<bool(StringRef)> Defined,
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
static std::vector<std::string>
uncovered(const std::vector<std::string> &Globals,
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

static void status(bool Ok) {
  if (Ok)
    WithColor(outs(), raw_ostream::GREEN) << "ok  ";
  else
    WithColor(outs(), raw_ostream::RED, /*Bold=*/true) << "FAIL";
}

static int runCheck() {
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
    Roots.reserve(Contracts.size());
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
      status(Ok);
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
      status(false);
      outs() << '\n';
      WithColor(outs(), raw_ostream::RED)
          << "  " << toString(R.takeError()) << '\n';
      outs().flush();
      continue;
    }
    bool Ok = R->S == ProofResult::Proved;
    AllOk &= Ok;
    status(Ok);
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

#if defined(Z80TESTER_ASSEMBLER) && defined(Z80TESTER_ALIVE2)
static std::string hex(uint64_t V, unsigned Bits) {
  return (Bits == 8 ? formatv("0x{0:x-2}", V) : formatv("0x{0:x-4}", V)).str();
}

/// An argument of a counterexample, by name.
static std::optional<uint64_t> exampleValue(const ProofResult &R,
                                            StringRef Name) {
  for (const auto &[N, V] : R.Example)
    if (N == Name)
      return uint64_t(V);
  return std::nullopt;
}

/// The registers and flags a counterexample starts from.
static std::string startState(Cpu C, const ProofResult &R) {
  auto Get = [&](StringRef Name) { return exampleValue(R, Name); };
  std::vector<std::string> Out;
  auto Show = [&](StringRef Name, std::optional<uint64_t> V, unsigned Bits) {
    Out.push_back((Name + "=" + (V ? hex(*V, Bits) : "?")).str());
  };
  for (StringRef N : {"A", "B", "C", "D", "E", "H", "L"})
    Show(N, Get(N), 8);
  // F as the CPU lays it out.
  std::optional<uint64_t> F = 0;
  for (auto [Name, Z80Bit, SM83Bit] :
       {std::tuple("SF", 7, -1), std::tuple("ZF", 6, 7), std::tuple("HF", 4, 5),
        std::tuple("PVF", 2, -1), std::tuple("NF", 1, 6),
        std::tuple("CF", 0, 4)}) {
    int Bit = C == Cpu::SM83 ? SM83Bit : Z80Bit;
    if (Bit < 0)
      continue;
    std::optional<uint64_t> V = Get(Name);
    F = F && V ? std::optional(*F | *V << Bit) : std::nullopt;
  }
  Show("F", F, 8);
  if (C == Cpu::Z80)
    for (auto [Pair, Hi, Lo] :
         {std::tuple("IX", "IXH", "IXL"), std::tuple("IY", "IYH", "IYL")}) {
      std::optional<uint64_t> H = Get(Hi), L = Get(Lo);
      Show(Pair, H && L ? std::optional(*H << 8 | *L) : std::nullopt, 16);
    }
  Show("SP", Get("SP"), 16);
  return join(Out, " ");
}

/// The memory a counterexample starts with: the bytes it gives, and the value
/// of the others.
static std::string startMemory(const ProofResult &R) {
  std::vector<std::string> Out;
  std::set<uint64_t> Seen;
  for (unsigned C = 0;; ++C) {
    std::optional<uint64_t> Addr = exampleValue(R, "addr" + std::to_string(C));
    std::optional<uint64_t> Byte = exampleValue(R, "byte" + std::to_string(C));
    if (!Addr || !Byte)
      break;
    // The first cell for an address wins.
    if (Seen.insert(*Addr).second)
      Out.push_back("(" + hex(*Addr, 16) + ")=" + hex(*Byte, 8));
  }
  std::optional<uint64_t> Fill = exampleValue(R, "fill");
  Out.push_back((Out.empty() ? "every byte " : "others ") +
                (Fill ? hex(*Fill, 8) : "?"));
  return join(Out, " ");
}

/// The values of a case's numbers in a counterexample.
static std::string numberValues(const RuleInstance &I, const ProofResult &P) {
  std::vector<std::string> Out;
  for (const RuleVar &K : I.Numbers) {
    std::optional<uint64_t> V = exampleValue(P, "const." + K.Name);
    std::string Text = "?";
    if (V && K.Min < 0)
      Text = std::to_string(int32_t(*V));
    else if (V)
      Text = hex(*V, 16);
    Out.push_back(K.Name + "=" + Text);
  }
  return join(Out, " ");
}

/// What one side of a case returns, as Alive2 prints it: whether it got to
/// its end, where it went if not, the fields it compares and the byte at
/// `at`.
static std::optional<APInt> sideValue(StringRef Text, const RuleInstance &I) {
  unsigned Bits = 1 + 16 + 8;
  for (const StateField &F : stateFields(I.C))
    if (is_contained(I.Compared, F.Name))
      Bits += F.Bits;
  if (!Text.consume_front("#x"))
    return std::nullopt;
  return APInt(Bits, Text.take_while(isHexDigit), 16);
}

/// What each side leaves that the other does not: where it went, the
/// fields and the byte at At.
static std::pair<std::string, std::string>
sideResults(const RuleInstance &I, const APInt &Before, const APInt &After,
            std::optional<uint64_t> At) {
  std::vector<std::string> Out[2];
  unsigned Pos = Before.getBitWidth();
  auto Take = [&](unsigned N) {
    Pos -= N;
    return std::pair(Before.extractBitsAsZExtValue(N, Pos),
                     After.extractBitsAsZExtValue(N, Pos));
  };
  auto Differ = [&](uint64_t B, uint64_t A, auto Show) {
    if (B != A) {
      Out[0].push_back(Show(B));
      Out[1].push_back(Show(A));
    }
  };
  auto [EndedB, EndedA] = Take(1);
  auto [ExitB, ExitA] = Take(16);
  if (EndedB != EndedA || (!EndedB && ExitB != ExitA))
    for (bool Side : {false, true})
      Out[Side].push_back((Side ? EndedA : EndedB)
                              ? "ran to the end"
                              : "leaves to " + hex(Side ? ExitA : ExitB, 16));
  for (const std::string &Field : I.Compared) {
    unsigned Bits = 0;
    for (const StateField &F : stateFields(I.C))
      if (Field == F.Name)
        Bits = F.Bits;
    auto [B, A] = Take(Bits);
    Differ(B, A, [&](uint64_t V) {
      return Field + "=" + (Bits == 1 ? std::to_string(V) : hex(V, Bits));
    });
  }
  auto [B, A] = Take(8);
  Differ(B, A, [&](uint64_t V) {
    return "(" + (At ? hex(*At, 16) : std::string("?")) + ")=" + hex(V, 8);
  });
  return {join(Out[0], " "), join(Out[1], " ")};
}

/// Assembles and links a case of a rule; fails with what the assembler says
/// for a case whose code does not assemble. The case gains the assumption
/// that each number fits the fields it is written to.
static Expected<Image> assembleCase(RuleInstance &I, bool &Assembles) {
  Assembles = false;
  SmallString<128> Tmp;
  int FD;
  if (std::error_code EC =
          sys::fs::createTemporaryFile("z80-test-rule", "s", FD, Tmp))
    return createStringError(EC, "cannot create a temporary file: %s",
                             EC.message().c_str());
  FileRemover Remove(Tmp);
  {
    raw_fd_ostream OS(FD, /*shouldClose=*/true);
    OS << instanceSource(I);
  }
  Expected<AsmLibrary> Lib = AsmLibrary::assemble(I.C, {Tmp.str().str()});
  if (!Lib)
    return Lib.takeError();
  Assembles = true;
  std::vector<std::string> Numbers;
  Numbers.reserve(I.Numbers.size());
  for (const RuleVar &K : I.Numbers)
    Numbers.push_back(K.Name);
  std::vector<SymbolField> Fields;
  Expected<Image> Img =
      Lib->link({ruleLabel(false), ruleLabel(true)}, Numbers, &Fields);
  if (!Img) {
    // The object is named after the temporary file.
    std::string Msg = toString(Img.takeError());
    StringRef TmpName = sys::path::filename(Tmp);
    for (size_t At; (At = Msg.find(TmpName)) != std::string::npos;)
      Msg.replace(At, TmpName.size(), I.R->where());
    return createStringError("%s", Msg.c_str());
  }
  auto Node = [](Expr::Kind K, std::string Name, int64_t V = 0,
                 std::shared_ptr<const Expr> L = nullptr,
                 std::shared_ptr<const Expr> R = nullptr) {
    auto E = std::make_shared<Expr>();
    E->K = K;
    E->Name = std::move(Name);
    E->Val = V;
    E->L = std::move(L);
    E->R = std::move(R);
    return E;
  };
  std::set<std::tuple<std::string, int64_t, int64_t, int64_t>> Seen;
  for (const SymbolField &F : Fields) {
    if (!Seen.insert({F.Symbol, F.Addend, F.Min, F.Max}).second)
      continue;
    auto V = Node(Expr::Binary, "+", 0, Node(Expr::Id, F.Symbol),
                  Node(Expr::Num, "", F.Addend));
    I.Assumes.push_back(
        Node(Expr::Binary, "&&", 0,
             Node(Expr::Binary, "<=", 0, Node(Expr::Num, "", F.Min), V),
             Node(Expr::Binary, "<=", 0, V, Node(Expr::Num, "", F.Max))));
  }
  return Img;
}

/// Shows a counterexample of a case.
static void showCounterexample(const RuleInstance &I, const ProofResult &P) {
  if (!I.Choice.empty())
    outs() << "  case    " << I.choice() << '\n';
  WithColor(outs(), raw_ostream::RED) << "  Alive2: " << P.Verdict << '\n';
  outs() << "  from    " << startState(I.C, P) << "\n          "
         << startMemory(P) << '\n';
  if (!I.Numbers.empty())
    outs() << "  number  " << numberValues(I, P) << '\n';
  std::optional<APInt> Before = sideValue(P.SourceValue, I);
  std::optional<APInt> After = sideValue(P.TargetValue, I);
  if (Before && After) {
    auto [B, A] = sideResults(I, *Before, *After, exampleValue(P, "at"));
    outs() << "  before  " << B << "\n  after   " << A << '\n';
  }
}

static int runRules() {
  std::vector<Rule> All = check(loadRules(InputPath));
  std::vector<const Rule *> Rules;
  if (Names.empty())
    for (const Rule &R : All)
      Rules.push_back(&R);
  for (const std::string &Name : Names) {
    auto It = llvm::find_if(All, [&](const Rule &R) { return R.Name == Name; });
    if (It == All.end())
      fail(createStringError("no rule %s", Name.c_str()));
    Rules.push_back(&*It);
  }

  ProofOptions PO;
  PO.Timeout = SmtTimeout;
  PO.Memory = SmtMemory;

  size_t Width = 16;
  for (const Rule *R : Rules)
    Width = std::max(Width, R->Name.size() + 7);
  bool AllOk = true;
  for (const Rule *R : Rules)
    for (Cpu C : ruleCpus(*R, CpuFlag)) {
      std::string Title = R->Name;
      if (R->Cpus.size() > 1)
        Title += " (" + std::string(cpuName(C)) + ")";
      outs() << left_justify(Title, Width) << "  ";
      outs().flush();
      auto Fail = [&](const Twine &Msg) {
        AllOk = false;
        status(false);
        outs() << '\n';
        WithColor(outs(), raw_ostream::RED) << "  " << Msg << '\n';
        outs().flush();
      };

      std::string NotAssembled;
      Expected<std::vector<RuleInstance>> Cases = instantiate(
          *R, C,
          [&](StringRef Text, unsigned Line) -> std::optional<std::string> {
            Rule One;
            One.File = R->File;
            One.Before = {{Text.str(), Line}};
            RuleInstance I;
            I.R = &One;
            I.C = C;
            I.Before = One.Before;
            bool Assembles;
            Expected<Image> Img = assembleCase(I, Assembles);
            if (Assembles) {
              consumeError(Img.takeError());
              return std::nullopt;
            }
            return toString(Img.takeError());
          },
          &NotAssembled);
      if (!Cases) {
        Fail(toString(Cases.takeError()));
        continue;
      }
      if (Cases->empty()) {
        Fail(NotAssembled.empty() ? "no case meets the conditions"
                                  : "no case assembles: " + NotAssembled);
        continue;
      }
      size_t Proved = 0, Failed = 0, Unproven = 0;
      double Seconds = 0;
      std::string Error;
      std::vector<std::pair<const RuleInstance *, ProofResult>> Shown;
      bool Stopped = false;
      for (RuleInstance &I : *Cases) {
        bool Assembles;
        Expected<Image> Img = assembleCase(I, Assembles);
        if (!Img) {
          std::string Msg = toString(Img.takeError());
          if (Assembles) {
            Error = Msg;
            break;
          }
          if (NotAssembled.empty())
            NotAssembled = (I.choice().empty() ? "" : I.choice() + ": ") + Msg;
          continue;
        }
        // A case whose code cannot be lifted, such as one with I/O or a
        // loop, is one that is not proved.
        ProofResult R;
        if (Expected<ProofResult> P = proveRule(I, *Img, PO))
          R = std::move(*P);
        else
          R.Verdict = toString(P.takeError());
        Seconds += R.Seconds;
        if (R.S == ProofResult::Proved) {
          ++Proved;
          continue;
        }
        bool Example = R.S == ProofResult::Counterexample;
        ++(Example ? Failed : Unproven);
        Stopped = MaxFailures && Failed == MaxFailures;
        // Counterexamples first, then a case that was not proved.
        if (Example && !Shown.empty() &&
            Shown[0].second.S != ProofResult::Counterexample)
          Shown.clear();
        if ((Example || Shown.empty()) && Shown.size() < Reports)
          Shown.push_back({&I, std::move(R)});
        if (Stopped)
          break;
      }

      if (!Error.empty()) {
        Fail(Error);
        continue;
      }
      if (Failed) {
        AllOk = false;
        status(false);
        outs() << formatv("  counterexample in {0}{1} of {2} cases",
                          Stopped ? "at least " : "", Failed,
                          Stopped ? Cases->size() : Proved + Failed + Unproven);
        if (Unproven)
          outs() << formatv(", {0} not proved", Unproven);
        outs() << '\n';
        for (const auto &[I, P] : Shown)
          showCounterexample(*I, P);
      } else if (Unproven) {
        AllOk = false;
        status(false);
        outs() << formatv("  {0} of {1} cases not proved\n", Unproven,
                          Proved + Unproven);
        if (!Shown[0].first->Choice.empty())
          outs() << "  case    " << Shown[0].first->choice() << '\n';
        WithColor(outs(), raw_ostream::YELLOW)
            << "  reason  " << Shown[0].second.Verdict << '\n';
      } else if (!Proved) {
        Fail("no case assembles: " + NotAssembled);
        continue;
      } else {
        status(true);
        outs() << formatv("  proved {0} case{1} in {2:f1} s\n", Proved,
                          Proved == 1 ? "" : "s", Seconds);
      }
      outs().flush();
    }
  return AllOk ? 0 : 1;
}
#endif

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  // Only our options; those of the linked LLVM libraries still work.
  cl::HideUnrelatedOptions(Category);
  cl::ParseCommandLineOptions(
      argc, argv,
      "Tests the llvm-z80 runtime against its contracts, or proves rewrite "
      "rules\n");
  if (sys::path::extension(InputPath) == ".rules") {
#if defined(Z80TESTER_ASSEMBLER) && defined(Z80TESTER_ALIVE2)
    return runRules();
#else
    fail(createStringError("%s: proving rules needs a z80-test built with "
                           "Z80LIFT_TESTER_ASSEMBLER and Alive2",
                           InputPath.c_str()));
#endif
  }
  return runCheck();
}
