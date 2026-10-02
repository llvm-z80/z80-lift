// Per-function contract of the runtime, read from specs/.

#ifndef Z80TESTER_SPEC_H
#define Z80TESTER_SPEC_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <string>
#include <vector>

namespace z80tester {

struct FunctionSpec {
  /// The runtime function, by its C name.
  std::string Name;

  /// The function in refs/ without its ref_ prefix. Defaults to Name without
  /// leading underscores.
  std::string Ref;

  /// Registers of each result, when the function returns more than one. The
  /// reference returns the first and stores the rest through trailing
  /// pointer parameters.
  std::vector<std::string> Results;

  /// Bytes behind each pointer parameter, compared after the call.
  std::vector<unsigned> PtrBytes;

  /// How results are compared: exact, sign (negative, zero or positive) or
  /// zero (zero or not).
  std::string Compare = "exact";

  /// Takes every argument in a register (Z80_Builtin) instead of
  /// __sdcccall(1).
  bool Builtin = false;

  std::string refName() const;
};

llvm::Expected<std::vector<FunctionSpec>> loadSpecs(llvm::StringRef Path);

} // namespace z80tester

#endif // Z80TESTER_SPEC_H
