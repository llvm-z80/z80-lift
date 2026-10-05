// Proves with Alive2 that a lifted function meets its contract for every
// input, instead of trying inputs.

#ifndef Z80TESTER_PROVER_H
#define Z80TESTER_PROVER_H

#include "z80tester/Tester.h"

#include "llvm/Support/MemoryBufferRef.h"

#include <string>
#include <vector>

namespace z80tester {

struct ProofOptions {
  unsigned Timeout = 600;  // seconds for each SMT query
  unsigned Memory = 16384; // megabytes for the SMT solver
  uint64_t StepLimit = 100000;
};

struct ProofResult {
  enum Status { Proved, Counterexample, Unproven } S = Unproven;
  double Seconds = 0;
  std::string Verdict; // Alive2's reason, unless proved
  // A counterexample's arguments, when Alive2 gives all of them.
  std::vector<U128> Inputs;
};

/// Proves that the function of P meets contract I of Contracts, which
/// compileContracts built without sanitizers, for every input that meets
/// `requires` and whatever the other registers hold.
llvm::Expected<ProofResult> prove(const TestPlan &P, const Contract &K,
                                  size_t I, const z80core::Image &Img,
                                  llvm::MemoryBufferRef Contracts,
                                  const ProofOptions &O);

} // namespace z80tester

#endif // Z80TESTER_PROVER_H
