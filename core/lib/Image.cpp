// Loads a linked ELF image and its symbol table.

#include "z80core/Image.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Format.h"

using namespace llvm;
using namespace z80core;

Expected<Image> Image::load(StringRef Path) {
  auto Bin = object::createBinary(Path);
  if (!Bin)
    return Bin.takeError();
  auto *Obj = dyn_cast<object::ELFObjectFileBase>(Bin->getBinary());
  if (!Obj)
    return createStringError("%s: not an ELF file", Path.str().c_str());

  Image Img;
  std::set<uint64_t> ExecSections;
  for (const object::SectionRef &Sec : Obj->sections()) {
    object::ELFSectionRef ESec(Sec);
    if (!(ESec.getFlags() & ELF::SHF_ALLOC))
      continue;
    uint64_t Addr = Sec.getAddress(), Size = Sec.getSize();
    if (Addr + Size > Img.Mem.size())
      return createStringError("%s: section above 64 KiB", Path.str().c_str());
    if (ESec.getFlags() & ELF::SHF_EXECINSTR)
      ExecSections.insert(Sec.getIndex());
    Img.End = std::max<uint32_t>(Img.End, Addr + Size);
    if (Sec.isBSS())
      continue;
    Expected<StringRef> Data = Sec.getContents();
    if (!Data)
      return Data.takeError();
    std::copy(Data->begin(), Data->end(), Img.Mem.begin() + Addr);
  }

  for (const object::SymbolRef &Sym : Obj->symbols()) {
    Expected<StringRef> Name = Sym.getName();
    Expected<uint64_t> Addr = Sym.getAddress();
    Expected<uint32_t> Flags = Sym.getFlags();
    Expected<object::section_iterator> Sec = Sym.getSection();
    if (!Name || !Addr || !Flags || !Sec) {
      consumeError(Name.takeError());
      consumeError(Addr.takeError());
      consumeError(Flags.takeError());
      consumeError(Sec.takeError());
      continue;
    }
    if (Name->empty() || *Sec == Obj->section_end() ||
        !ExecSections.count((*Sec)->getIndex()))
      continue;

    uint16_t A = *Addr;
    bool Global = *Flags & object::SymbolRef::SF_Global;
    Img.Addrs.emplace(Name->str(), A);
    if (Global) {
      Img.Entries.insert(A);
      Img.Names[A] = Name->str();
    } else if (!Img.Names.count(A)) {
      Img.Names[A] = Name->str();
    }
  }
  return Img;
}

std::optional<uint16_t> Image::lookup(StringRef Name) const {
  auto It = Addrs.find(Name);
  if (It == Addrs.end())
    It = Addrs.find(("_" + Name).str());
  if (It == Addrs.end())
    return std::nullopt;
  return It->second;
}

std::string Image::nameAt(uint16_t Addr) const {
  auto It = Names.find(Addr);
  if (It != Names.end())
    return It->second;
  std::string S;
  raw_string_ostream(S) << "sub_" << format_hex_no_prefix(Addr, 4);
  return S;
}
