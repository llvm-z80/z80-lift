// Rewrite rules, such as a compiler's peepholes: code before and after the
// rewrite, and what must stay the same.

#ifndef Z80TESTER_RULES_H
#define Z80TESTER_RULES_H

#include "z80core/Decoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>
#include <utility>
#include <vector>

namespace z80tester {

/// What a rule keeps: a register, a register pair, SP, all of F or one flag.
enum class Kept {
  A,
  B,
  C,
  D,
  E,
  H,
  L,
  BC,
  DE,
  HL,
  IX,
  IY,
  SP,
  F,
  SF,
  ZF,
  HF,
  PVF,
  NF,
  CF
};

const char *keptName(Kept K);
unsigned keptBits(Kept K);

struct Rule {
  std::string Name;
  // The assembly of each side, a line each, with the line it came from.
  std::vector<std::pair<std::string, unsigned>> Before, After;
  std::vector<Kept> Keep;
  // Whether only memory at and above SP must match, not the 32 KiB below it.
  bool AboveSP = false;
  std::string File;
  unsigned Line = 0;
  std::string where() const;
};

llvm::Expected<std::vector<Rule>> loadRules(llvm::StringRef Path,
                                            z80core::Cpu C);

/// The C names of the labels that start each side of rule I.
std::string ruleLabel(size_t I, bool After);

/// Assembly that puts each side of each rule at its label, ending in HALT.
std::string rulesSource(llvm::ArrayRef<Rule> Rules);

} // namespace z80tester

#endif // Z80TESTER_RULES_H
