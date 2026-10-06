// A linked image and its symbols.

#ifndef Z80CORE_IMAGE_H
#define Z80CORE_IMAGE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace z80core {

struct Image {
  /// The 64 KiB address space with every allocated section loaded.
  std::vector<uint8_t> Mem = std::vector<uint8_t>(0x10000);

  /// One past the highest allocated byte.
  uint32_t End = 0;

  /// Global symbols in executable sections; these are the function entries.
  std::set<uint16_t> Entries;

  /// One name per address, preferring global symbols, for printing.
  std::map<uint16_t, std::string> Names;

  /// The names of the global symbols in executable sections.
  std::set<std::string, std::less<>> Globals;

  /// Every symbol in an executable section by name.
  std::map<std::string, uint16_t, std::less<>> Addrs;

  /// A byte that holds bits Shift to Shift + 7 of Symbol + Addend, for a
  /// symbol the link left undefined.
  struct SymbolByte {
    std::string Symbol;
    int64_t Addend = 0;
    unsigned Shift = 0;
  };

  /// The bytes of Mem that hold part of an undefined symbol's value; Mem has
  /// 0 there.
  std::map<uint16_t, SymbolByte> SymbolBytes;

  static llvm::Expected<Image> load(llvm::StringRef Path);

  /// Records a symbol in an executable section.
  void addSymbol(llvm::StringRef Name, uint16_t Addr, bool Global);

  /// Looks a symbol up by its assembler name or its C name.
  std::optional<uint16_t> lookup(llvm::StringRef Name) const;

  /// The symbol at Addr, or sub_XXXX.
  std::string nameAt(uint16_t Addr) const;
};

} // namespace z80core

#endif // Z80CORE_IMAGE_H
