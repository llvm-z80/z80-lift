// Assembles the runtime's sources with LLVM's Z80 assembler and links them into
// an image. Only built with Z80LIFT_TESTER_ASSEMBLER.

#ifndef Z80TESTER_ASSEMBLER_H
#define Z80TESTER_ASSEMBLER_H

#include "z80core/Decoder.h"
#include "z80core/Image.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace z80tester {

/// Objects assembled from source files, linked on demand like an archive.
class AsmLibrary {
public:
  static llvm::Expected<AsmLibrary> assemble(z80core::Cpu C,
                                             llvm::ArrayRef<std::string> Files);

  AsmLibrary(AsmLibrary &&);
  AsmLibrary &operator=(AsmLibrary &&);
  ~AsmLibrary();

  /// Whether a file defines the symbol, by its assembler name or its C name.
  bool defines(llvm::StringRef Name) const;

  /// The global symbols File defines.
  std::vector<std::string> globals(llvm::StringRef File) const;

  /// Whether every symbol File needs is defined, so that it links on its own.
  bool linkable(llvm::StringRef File) const;

  /// Links the objects that define Roots, and those they need, from address 0.
  llvm::Expected<z80core::Image> link(llvm::ArrayRef<std::string> Roots) const;

private:
  struct Object;

  AsmLibrary();
  std::optional<size_t> definer(llvm::StringRef Name) const;
  llvm::Expected<std::set<size_t>> pick(std::vector<size_t> Work) const;

  std::vector<std::unique_ptr<Object>> Objects;
  std::map<std::string, size_t, std::less<>> Defs; // the first file wins
};

} // namespace z80tester

#endif // Z80TESTER_ASSEMBLER_H
