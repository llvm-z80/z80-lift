// Runs lifted functions against their contracts.

#ifndef Z80TESTER_TESTER_H
#define Z80TESTER_TESTER_H

#include "z80core/Image.h"
#include "z80tester/CallConv.h"
#include "z80tester/Contract.h"
#include "z80tester/Jit.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace z80tester {

/// Defaults for the functions whose contracts do not say how to test them.
struct TestOptions {
  unsigned Threads = 0;            // 0: one per hardware thread
  unsigned MaxExhaustiveBits = 32; // wider inputs are sampled instead
  uint64_t Samples = 1 << 24;
  uint64_t StepLimit = 100000;
  uint64_t Seed = 1;
  unsigned MaxReports = 10;
};

/// A compiled condition of a contract.
struct Check {
  const Condition *Cond = nullptr;
  std::string Where; // kept for the trap handler
  AdapterFn Fn = nullptr;
};

/// The compiled values of an example, by parameter.
struct ExampleCheck {
  std::string Where;
  std::vector<std::pair<unsigned, AdapterFn>> Values;
};

/// Everything needed to call one runtime function and check its contract.
struct TestPlan {
  z80core::Cpu C;
  std::string Name;
  uint16_t Entry;
  std::vector<Ty> Params;
  std::optional<Ty> Ret;
  CallLayout Layout;
  Check Requires; // all of them in one function, or none if Fn is null
  std::vector<Check> Ensures;
  bool Exhaustive = false;
  std::optional<uint64_t> Samples;
  std::vector<ExampleCheck> Examples;
  LiftedFn Fn = nullptr; // null runs the interpreter
};

struct TestResult {
  bool Exhaustive = false;
  uint64_t Inputs = 0;  // calls made, examples included
  uint64_t Checked = 0; // calls that met `requires`
  uint64_t Mismatches = 0;
  uint64_t Faults = 0; // bad return, stack, IX, write or step limit
  uint64_t MaxSteps = 0;
  std::vector<std::string> Reports;
};

/// Works out the parameters and calling layout from the contract's signature.
llvm::Expected<TestPlan> planTest(z80core::Cpu C, const z80core::Image &Img,
                                  const Contract &K, const Signature &Sig);

TestResult runTest(const TestPlan &P, const z80core::Image &Img,
                   const TestOptions &O);

} // namespace z80tester

#endif // Z80TESTER_TESTER_H
