// Assembles source files in memory and links them like an archive. The
// relocations follow lld's Z80 port.

#include "z80tester/Assembler.h"

#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h" // IWYU pragma: keep
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include <mutex>
#include <set>

using namespace llvm;
using namespace z80core;
using namespace z80tester;

struct AsmLibrary::Object {
  std::string File;
  std::unique_ptr<MemoryBuffer> Buf;
  std::unique_ptr<object::ObjectFile> Obj;
};

/// Assembles a file to an ELF object, as `llvm-mc -filetype=obj` does.
static Expected<std::unique_ptr<MemoryBuffer>>
assembleFile(const Target &T, const Triple &TT, StringRef Path) {
  auto Buf = MemoryBuffer::getFile(Path);
  if (!Buf)
    return createStringError(Buf.getError(), "%s: %s", Path.str().c_str(),
                             Buf.getError().message().c_str());
  SourceMgr SrcMgr;
  SrcMgr.AddNewSourceBuffer(std::move(*Buf), SMLoc());

  MCTargetOptions Opts;
  std::unique_ptr<MCRegisterInfo> MRI(T.createMCRegInfo(TT));
  std::unique_ptr<MCAsmInfo> MAI(T.createMCAsmInfo(*MRI, TT, Opts));
  std::unique_ptr<MCSubtargetInfo> STI(T.createMCSubtargetInfo(TT, "", ""));
  std::unique_ptr<MCInstrInfo> MII(T.createMCInstrInfo());
  MCContext Ctx(TT, *MAI, *MRI, *STI, &SrcMgr);
  std::unique_ptr<MCObjectFileInfo> MOFI(
      T.createMCObjectFileInfo(Ctx, /*PIC=*/false));
  Ctx.setObjectFileInfo(MOFI.get());

  SmallVector<char, 0> Out;
  raw_svector_ostream OS(Out);
  MCAsmBackend *MAB = T.createMCAsmBackend(*STI, *MRI, Opts);
  std::unique_ptr<MCStreamer> Str(T.createMCObjectStreamer(
      TT, Ctx, std::unique_ptr<MCAsmBackend>(MAB), MAB->createObjectWriter(OS),
      std::unique_ptr<MCCodeEmitter>(T.createMCCodeEmitter(*MII, Ctx)), *STI));
  std::unique_ptr<MCAsmParser> Parser(
      createMCAsmParser(SrcMgr, Ctx, *Str, *MAI));
  std::unique_ptr<MCTargetAsmParser> TAP(
      T.createMCAsmParser(*STI, *Parser, *MII));
  Parser->setTargetParser(*TAP);
  if (Parser->Run(/*NoInitialTextSection=*/false))
    return createStringError("%s: does not assemble", Path.str().c_str());
  return MemoryBuffer::getMemBufferCopy(StringRef(Out.data(), Out.size()),
                                        Path);
}

static void write16(uint8_t *Loc, uint64_t V) {
  Loc[0] = V;
  Loc[1] = V >> 8;
}

/// Applies a relocation as lld's Z80 port does. V is S + A, less P for the
/// PC-relative ones.
static bool relocate(uint8_t *Loc, uint32_t Type, int64_t V) {
  auto Fits = [&](unsigned Bits) {
    return isIntN(Bits, V) || isUIntN(Bits, V);
  };
  switch (Type) {
  case ELF::R_Z80_NONE: return true;
  case ELF::R_Z80_IMM8:
  case ELF::R_Z80_ADDR8: *Loc = V; return Fits(8);
  case ELF::R_Z80_ADDR16:
  case ELF::R_Z80_IMM16:
  case ELF::R_Z80_ADDR24_SEGMENT:
  case ELF::R_Z80_ADDR_ASCIZ: write16(Loc, V); return Fits(16);
  case ELF::R_Z80_ADDR16_LO:
  case ELF::R_Z80_ADDR24_SEGMENT_LO: *Loc = V; return true;
  case ELF::R_Z80_ADDR16_HI:
  case ELF::R_Z80_ADDR24_SEGMENT_HI: *Loc = V >> 8; return true;
  case ELF::R_Z80_PCREL_8: *Loc = V - 1; return isIntN(8, V - 1);
  case ELF::R_Z80_PCREL_16: write16(Loc, V - 1); return isIntN(16, V - 1);
  case ELF::R_Z80_ADDR24:
    write16(Loc, V);
    Loc[2] = V >> 16;
    return Fits(24);
  case ELF::R_Z80_ADDR24_BANK: *Loc = V >> 16; return true;
  case ELF::R_Z80_ADDR13: write16(Loc, V); return isUIntN(13, V);
  case ELF::R_Z80_DISP8: *Loc = V; return isIntN(8, V);
  case ELF::R_SM83_LDH8: *Loc = V - 0xFF00; return isUIntN(8, V - 0xFF00);
  case ELF::R_Z80_FK_DATA_4:
  case ELF::R_Z80_FK_DATA_8:
    for (unsigned I = 0; I < (Type == ELF::R_Z80_FK_DATA_4 ? 4 : 8); ++I)
      Loc[I] = uint64_t(V) >> (8 * I);
    return true;
  default: return false;
  }
}

namespace {

struct Symbol {
  std::string Name;
  uint32_t Flags = 0;
  object::SymbolRef::Type Type = object::SymbolRef::ST_Unknown;
  object::section_iterator Section;
  uint64_t Value = 0;
};

} // namespace

static Expected<Symbol> readSymbol(const object::SymbolRef &Sym) {
  Symbol S{"", 0, object::SymbolRef::ST_Unknown,
           Sym.getObject()->section_end()};
  Expected<StringRef> Name = Sym.getName();
  if (!Name)
    return Name.takeError();
  Expected<uint32_t> Flags = Sym.getFlags();
  if (!Flags)
    return Flags.takeError();
  Expected<object::SymbolRef::Type> Type = Sym.getType();
  if (!Type)
    return Type.takeError();
  Expected<object::section_iterator> Sec = Sym.getSection();
  if (!Sec)
    return Sec.takeError();
  Expected<uint64_t> Value = Sym.getValue();
  if (!Value)
    return Value.takeError();
  S.Name = Name->str();
  S.Flags = *Flags;
  S.Type = *Type;
  S.Section = *Sec;
  S.Value = *Value;
  return S;
}

static bool isDefinedGlobal(const Symbol &S) {
  return !S.Name.empty() && !(S.Flags & object::SymbolRef::SF_Undefined) &&
         (S.Flags & object::SymbolRef::SF_Global);
}

AsmLibrary::AsmLibrary() = default;
AsmLibrary::AsmLibrary(AsmLibrary &&) noexcept = default;
AsmLibrary &AsmLibrary::operator=(AsmLibrary &&) noexcept = default;
AsmLibrary::~AsmLibrary() = default;

Expected<AsmLibrary> AsmLibrary::assemble(Cpu C, ArrayRef<std::string> Files) {
  static std::once_flag Once;
  std::call_once(Once, [] {
    LLVMInitializeZ80TargetInfo();
    LLVMInitializeZ80TargetMC();
    LLVMInitializeZ80AsmParser();
  });
  Triple TT(C == Cpu::SM83 ? "sm83" : "z80");
  std::string Err;
  const Target *T = TargetRegistry::lookupTarget(TT, Err);
  if (!T)
    return createStringError("%s", Err.c_str());

  AsmLibrary L;
  for (const std::string &F : Files) {
    auto Buf = assembleFile(*T, TT, F);
    if (!Buf)
      return Buf.takeError();
    auto Obj = object::ObjectFile::createObjectFile((*Buf)->getMemBufferRef());
    if (!Obj)
      return Obj.takeError();
    if (!isa<object::ELFObjectFileBase>(**Obj))
      return createStringError("%s: not assembled to ELF", F.c_str());

    size_t I = L.Objects.size();
    for (const object::SymbolRef &Sym : (*Obj)->symbols()) {
      Expected<Symbol> S = readSymbol(Sym);
      if (!S)
        return S.takeError();
      if (isDefinedGlobal(*S))
        L.Defs.try_emplace(S->Name, I);
    }
    auto O = std::make_unique<Object>();
    O->File = F;
    O->Buf = std::move(*Buf);
    O->Obj = std::move(*Obj);
    L.Objects.push_back(std::move(O));
  }
  return std::move(L);
}

std::optional<size_t> AsmLibrary::definer(StringRef Name) const {
  auto It = Defs.find(Name);
  if (It == Defs.end())
    return std::nullopt;
  return It->second;
}

bool AsmLibrary::defines(StringRef Name) const {
  return definer(Name) || definer(("_" + Name).str());
}

std::vector<std::string> AsmLibrary::globals(StringRef File) const {
  std::vector<std::string> Out;
  for (const auto &[Name, I] : Defs)
    if (Objects[I]->File == File)
      Out.push_back(Name);
  return Out;
}

bool AsmLibrary::linkable(StringRef File) const {
  for (size_t I = 0; I < Objects.size(); ++I) {
    if (Objects[I]->File != File)
      continue;
    Expected<std::set<size_t>> Picked = pick({I});
    if (Picked)
      return true;
    consumeError(Picked.takeError());
  }
  return false;
}

/// The objects Work needs, picked the way an archive's members are pulled in.
Expected<std::set<size_t>> AsmLibrary::pick(std::vector<size_t> Work) const {
  std::set<size_t> Picked(Work.begin(), Work.end());
  while (!Work.empty()) {
    size_t I = Work.back();
    Work.pop_back();
    for (const object::SymbolRef &Sym : Objects[I]->Obj->symbols()) {
      Expected<Symbol> S = readSymbol(Sym);
      if (!S)
        return S.takeError();
      if (S->Name.empty() || !(S->Flags & object::SymbolRef::SF_Undefined))
        continue;
      if (std::optional<size_t> D = definer(S->Name)) {
        if (Picked.insert(*D).second)
          Work.push_back(*D);
      } else if (!(S->Flags & object::SymbolRef::SF_Weak)) {
        return createStringError(
            "%s needs %s, which no file defines",
            sys::path::filename(Objects[I]->File).str().c_str(),
            S->Name.c_str());
      }
    }
  }
  return Picked;
}

Expected<Image> AsmLibrary::link(ArrayRef<std::string> Roots) const {
  std::vector<size_t> Work;
  for (const std::string &R : Roots) {
    std::optional<size_t> I = definer(R);
    if (!I)
      I = definer("_" + R);
    if (!I)
      return createStringError("no file defines %s", R.c_str());
    Work.push_back(*I);
  }
  Expected<std::set<size_t>> PickedOr = pick(std::move(Work));
  if (!PickedOr)
    return PickedOr.takeError();
  const std::set<size_t> &Picked = *PickedOr;

  // Code first, then data, in file order from address 0.
  Image Img;
  std::map<std::pair<size_t, uint64_t>, uint32_t> Base;
  std::set<std::pair<size_t, uint64_t>> Exec;
  uint32_t Addr = 0;
  for (bool Code : {true, false}) {
    for (size_t I : Picked) {
      for (const object::SectionRef &Sec : Objects[I]->Obj->sections()) {
        uint64_t Flags = object::ELFSectionRef(Sec).getFlags();
        if (!(Flags & ELF::SHF_ALLOC) ||
            bool(Flags & ELF::SHF_EXECINSTR) != Code)
          continue;
        Addr = alignTo(Addr, Sec.getAlignment());
        if (Addr + Sec.getSize() > Img.Mem.size())
          return createStringError("the runtime does not fit in 64 KiB");
        Base[{I, Sec.getIndex()}] = Addr;
        if (Code)
          Exec.insert({I, Sec.getIndex()});
        if (!Sec.isBSS()) {
          Expected<StringRef> Data = Sec.getContents();
          if (!Data)
            return Data.takeError();
          std::copy(Data->begin(), Data->end(), Img.Mem.begin() + Addr);
        }
        Addr += Sec.getSize();
      }
    }
  }
  Img.End = Addr;

  // The address of a defined symbol, or nothing if its section is not loaded.
  auto Place = [&](size_t I, const Symbol &S) -> std::optional<uint32_t> {
    if (S.Section == Objects[I]->Obj->section_end())
      return uint32_t(S.Value);
    auto It = Base.find({I, S.Section->getIndex()});
    if (It == Base.end())
      return std::nullopt;
    return It->second + uint32_t(S.Value);
  };

  std::map<std::string, std::pair<uint32_t, size_t>> Globals;
  for (size_t I : Picked) {
    for (const object::SymbolRef &Sym : Objects[I]->Obj->symbols()) {
      Expected<Symbol> S = readSymbol(Sym);
      if (!S)
        return S.takeError();
      if (S->Name.empty() || (S->Flags & object::SymbolRef::SF_Undefined))
        continue;
      std::optional<uint32_t> A = Place(I, *S);
      if (!A)
        continue;
      bool Global = S->Flags & object::SymbolRef::SF_Global;
      if (Global) {
        auto [It, New] = Globals.try_emplace(S->Name, *A, I);
        if (!New)
          return createStringError(
              "%s is defined in both %s and %s", S->Name.c_str(),
              sys::path::filename(Objects[It->second.second]->File)
                  .str()
                  .c_str(),
              sys::path::filename(Objects[I]->File).str().c_str());
      }
      if (S->Type != object::SymbolRef::ST_Debug &&
          S->Type != object::SymbolRef::ST_File &&
          S->Section != Objects[I]->Obj->section_end() &&
          Exec.count({I, S->Section->getIndex()}))
        Img.addSymbol(S->Name, *A, Global);
    }
  }

  for (size_t I : Picked) {
    const object::ObjectFile &Obj = *Objects[I]->Obj;
    StringRef File = sys::path::filename(Objects[I]->File);
    for (const object::SectionRef &Sec : Obj.sections()) {
      Expected<object::section_iterator> Target = Sec.getRelocatedSection();
      if (!Target)
        return Target.takeError();
      if (*Target == Obj.section_end())
        continue;
      auto B = Base.find({I, (*Target)->getIndex()});
      if (B == Base.end())
        continue;
      for (const object::RelocationRef &Rel : Sec.relocations()) {
        uint32_t P = B->second + Rel.getOffset();
        int64_t V = 0;
        if (object::symbol_iterator It = Rel.getSymbol();
            It != Obj.symbol_end()) {
          Expected<Symbol> S = readSymbol(*It);
          if (!S)
            return S.takeError();
          if (S->Flags & object::SymbolRef::SF_Undefined) {
            auto G = Globals.find(S->Name);
            V = G == Globals.end() ? 0 : G->second.first;
          } else if (std::optional<uint32_t> A = Place(I, *S)) {
            V = *A;
          } else {
            return createStringError("%s: relocation against %s, which is "
                                     "not loaded",
                                     File.str().c_str(), S->Name.c_str());
          }
        }
        Expected<int64_t> Addend = object::ELFRelocationRef(Rel).getAddend();
        if (!Addend)
          return Addend.takeError();
        V += *Addend;
        uint32_t Type = Rel.getType();
        if (Type == ELF::R_Z80_PCREL_8 || Type == ELF::R_Z80_PCREL_16)
          V -= P;
        if (P + 8 > Img.Mem.size())
          return createStringError("%s: relocation at the end of memory",
                                   File.str().c_str());
        if (!relocate(&Img.Mem[P], Type, V))
          return createStringError("%s: relocation of type %u at 0x%04x is "
                                   "unknown or out of range",
                                   File.str().c_str(), Type, P);
      }
    }
  }
  return Img;
}
