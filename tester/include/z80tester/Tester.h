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

using U128 = unsigned __int128;

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

/// The compiled bounds of a `modifies` range.
struct RangeCheck {
  unsigned Param = 0;
  bool Sizes =
      false; // whether it sizes the buffer; its bounds read only strings
  std::string Where;
  AdapterFn Lo = nullptr, Hi = nullptr;
};

/// The compiled bounds of a `tests` domain.
struct DomainCheck {
  unsigned Param = 0;
  bool String = false;
  std::string Where;
  AdapterFn Lo = nullptr, Hi = nullptr;
};

/// How the values of a parameter are drawn. Keys order the values of a type:
/// integers by value, floats by value with -0 just below +0.
struct Draw {
  enum Kind { Bits, Keys, String, Pointer } K = Bits;
  U128 Lo = 0, Size = 0;         // Keys: Size keys from Lo
  uint64_t LenLo = 0, LenHi = 0; // String: lengths from LenLo below LenHi
  std::vector<U128> Specials;    // values worth trying often
};

/// An example with its values worked out; pointers get the bytes of a string.
struct ExampleValues {
  std::string Where;
  std::vector<std::pair<unsigned, U128>> Values;
  std::vector<std::pair<unsigned, std::string>> Strings;
};

/// Everything needed to call one runtime function and check its contract.
struct TestPlan {
  z80core::Cpu C;
  std::string Name;
  std::string Where; // the contract's
  uint16_t Entry;
  std::vector<Ty> Params;
  std::optional<Ty> Ret;
  CallLayout Layout;
  bool Placed = false; // outside the C convention, which keeps IX
  std::vector<ParamInfo> Info;
  Check Requires; // all of them in one function, or none if Fn is null
  std::vector<Check> Ensures;
  std::vector<RangeCheck> Ranges;
  std::vector<DomainCheck> Domains;
  bool Exhaustive = false;
  std::optional<uint64_t> Samples;
  std::vector<ExampleCheck> Examples;

  // Worked out by finishPlan.
  std::vector<Draw> Draws;
  std::vector<ExampleValues> ExampleInputs;
  LiftedFn Fn = nullptr; // null runs the interpreter
};

struct TestResult {
  bool Exhaustive = false;
  uint64_t Inputs = 0;   // inputs tried, examples included
  uint64_t Checked = 0;  // inputs that met `requires` and were called
  uint64_t Unplaced = 0; // inputs whose buffers did not fit in memory
  uint64_t Mismatches = 0;
  uint64_t Faults = 0; // bad return, stack, IX, write or step limit
  uint64_t MaxSteps = 0;
  std::vector<std::string> Reports;
};

/// Works out the parameters and calling layout from the contract's signature.
/// The caller then fills in the compiled functions and calls finishPlan.
llvm::Expected<TestPlan> planTest(z80core::Cpu C, const z80core::Image &Img,
                                  const Contract &K, const Signature &Sig);

/// Evaluates the domains and examples, which need the compiled contract.
llvm::Error finishPlan(TestPlan &P);

TestResult runTest(const TestPlan &P, const z80core::Image &Img,
                   const TestOptions &O);

} // namespace z80tester

#endif // Z80TESTER_TESTER_H
