// Runs lifted functions against their references.

#ifndef Z80TESTER_TESTER_H
#define Z80TESTER_TESTER_H

#include "z80core/Image.h"
#include "z80tester/CallConv.h"
#include "z80tester/Jit.h"
#include "z80tester/Spec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace z80tester {

struct TestOptions {
  unsigned Threads = 0;            // 0: one per hardware thread
  unsigned MaxExhaustiveBits = 32; // wider inputs are sampled instead
  uint64_t Samples = 1 << 24;
  uint64_t StepLimit = 100000;
  uint64_t Seed = 1;
  unsigned MaxReports = 10;
};

/// Everything needed to call one runtime function and its reference.
struct TestPlan {
  z80core::Cpu C;
  std::string Name;
  uint16_t Entry;
  std::vector<Ty> Params; // the runtime function's own parameters
  std::optional<Ty> Ret;
  CallLayout Layout;
  std::vector<std::vector<Reg8>> Results; // overrides Layout.Ret if set
  std::vector<unsigned> PtrBytes;
  std::string Compare;
  AdapterFn Ref = nullptr, Pre = nullptr;
  LiftedFn Fn = nullptr; // null runs the interpreter
};

struct TestResult {
  bool Exhaustive = false;
  uint64_t Inputs = 0;  // calls made
  uint64_t Checked = 0; // calls whose result was compared
  uint64_t Mismatches = 0;
  uint64_t Faults = 0; // bad return, stack, IX, write or step limit
  uint64_t MaxSteps = 0;
  std::vector<std::string> Reports;
};

/// Works out the target parameters and calling layout from the spec and the
/// reference signature.
llvm::Expected<TestPlan> planTest(z80core::Cpu C, const z80core::Image &Img,
                                  const FunctionSpec &Spec, const RefSig &Sig);

TestResult runTest(const TestPlan &P, const z80core::Image &Img,
                   const TestOptions &O);

} // namespace z80tester

#endif // Z80TESTER_TESTER_H
