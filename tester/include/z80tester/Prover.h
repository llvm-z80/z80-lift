// Proves with Alive2 that a lifted function meets its contract for every
// input, instead of trying inputs, and that rewrite rules keep what they say.

#ifndef Z80TESTER_PROVER_H
#define Z80TESTER_PROVER_H

#include "z80tester/Rules.h"
#include "z80tester/Tester.h"

#include "llvm/Support/MemoryBufferRef.h"

#include <string>
#include <utility>
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
  // The arguments Alive2 gives, by name.
  std::vector<std::pair<std::string, U128>> Example;
  // What each side returns, as Alive2 prints it.
  std::string SourceValue, TargetValue;
};

/// Proves that the function of P meets contract I of Contracts, which
/// compileContracts built without sanitizers, for every input that meets
/// `requires` and whatever the other registers hold.
llvm::Expected<ProofResult> prove(const TestPlan &P, const Contract &K,
                                  size_t I, const z80core::Image &Img,
                                  llvm::MemoryBufferRef Contracts,
                                  const ProofOptions &O);

/// Proves that the code after rule I keeps what the rule lists, and memory,
/// as the code before it does, for any registers, flags and memory. Img holds
/// both sides at the labels that rulesSource gives them.
llvm::Expected<ProofResult> proveRule(const Rule &R, size_t I, z80core::Cpu C,
                                      const z80core::Image &Img,
                                      const ProofOptions &O);

} // namespace z80tester

#endif // Z80TESTER_PROVER_H
