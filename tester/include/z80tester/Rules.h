// Rewrite rules, such as a compiler's peepholes: code before and after the
// rewrite, the variables it is written over, and what may differ.

#ifndef Z80TESTER_RULES_H
#define Z80TESTER_RULES_H

#include "z80core/Decoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace z80tester {

/// A field of the CPU state, which both sides of a rule must leave the same
/// unless the rule says it may differ.
struct StateField {
  const char *Name;
  unsigned Bits;
};

/// The fields of the state that rules on C compare.
llvm::ArrayRef<StateField> stateFields(z80core::Cpu C);

/// An expression over a rule's variables and the state it starts from, with
/// C's operators. An Id is a variable, a field of the starting state in
/// capitals, or a register; Text is an operand in quotes.
struct Expr {
  enum Kind { Num, Id, Text, Unary, Binary, Call };
  Kind K = Num;
  int64_t Val = 0;
  std::string Name; // the name, the operand, the operator or the function
  std::shared_ptr<const Expr> L, R;
};

/// A variable of a rule, of the type written in it: an operand that takes
/// each of Choices in turn, or a number from Min to Max, which one proof
/// covers.
struct RuleVar {
  std::string Name, Type;
  bool Number = false;
  std::vector<std::string> Choices;
  int64_t Min = 0, Max = 0;
};

struct Rule {
  std::string Name;
  std::vector<z80core::Cpu> Cpus; // none: the CPU of the command line
  std::vector<RuleVar> Vars;
  std::vector<std::shared_ptr<const Expr>> Conds;
  // The assembly of each side, a line each, with the line it came from.
  std::vector<std::pair<std::string, unsigned>> Before, After;
  std::vector<std::string> Dead;
  // Whether the 32 KiB below SP may differ.
  bool DeadBelowSP = false;
  std::string File;
  unsigned Line = 0;
  std::string where() const;
};

/// One case of a rule: its operand variables chosen and put in its code.
struct RuleInstance {
  const Rule *R = nullptr;
  z80core::Cpu C = z80core::Cpu::Z80;
  std::vector<std::pair<std::string, std::string>> Choice;
  std::vector<std::pair<std::string, unsigned>> Before, After;
  // The fields of stateFields(C) that both sides must leave the same.
  std::vector<std::string> Compared;
  bool AboveSP = false;
  std::vector<RuleVar> Numbers;
  // What the case assumes of the numbers and the starting state.
  std::vector<std::shared_ptr<const Expr>> Assumes;
  std::string choice() const;
};

llvm::Expected<std::vector<Rule>> loadRules(llvm::StringRef Path);

/// The CPUs to prove R on.
std::vector<z80core::Cpu> ruleCpus(const Rule &R, z80core::Cpu Default);

/// Whether a line of a case assembles on its own: the assembler's error if
/// it does not. Its line in the rule file comes with it.
using LineCheck = llvm::function_ref<std::optional<std::string>(
    llvm::StringRef Text, unsigned Line)>;

/// The cases of R on C whose conditions can hold, and whose lines assemble
/// on their own if Assembles is given; AsmError gets the first error.
llvm::Expected<std::vector<RuleInstance>>
instantiate(const Rule &R, z80core::Cpu C, LineCheck Assembles = nullptr,
            std::string *AsmError = nullptr);

/// The C name of the label that starts a side of a case.
std::string ruleLabel(bool After);

/// Assembly that puts each side of I at its label, ending in HALT. Labels a
/// side defines are its own; others are addresses past the code, the same on
/// both sides.
std::string instanceSource(const RuleInstance &I);

} // namespace z80tester

#endif // Z80TESTER_RULES_H
