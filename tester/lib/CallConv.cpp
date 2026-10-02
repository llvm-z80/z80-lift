// Mirrors classifyArg and isCalleeCleanup in the backend's Z80CallLowering.cpp.

#include "z80tester/CallConv.h"

#include "llvm/IR/DerivedTypes.h"

using namespace llvm;
using namespace z80core;
using namespace z80tester;

unsigned z80tester::sizeOf(Ty T) {
  switch (T) {
  case Ty::I8: return 1;
  case Ty::I16:
  case Ty::F16:
  case Ty::Ptr: return 2;
  case Ty::I32:
  case Ty::F32: return 4;
  case Ty::I64: return 8;
  case Ty::I128: return 16;
  }
  return 0;
}

const char *z80tester::tyName(Ty T) {
  switch (T) {
  case Ty::I8: return "i8";
  case Ty::I16: return "i16";
  case Ty::I32: return "i32";
  case Ty::I64: return "i64";
  case Ty::I128: return "i128";
  case Ty::F16: return "f16";
  case Ty::F32: return "f32";
  case Ty::Ptr: return "ptr";
  }
  return "?";
}

std::optional<Ty> z80tester::tyFromIR(Type *T) {
  if (T->isHalfTy())
    return Ty::F16;
  if (T->isFloatTy())
    return Ty::F32;
  if (T->isPointerTy())
    return Ty::Ptr;
  if (T->isIntegerTy()) {
    switch (T->getIntegerBitWidth()) {
    case 8: return Ty::I8;
    case 16: return Ty::I16;
    case 32: return Ty::I32;
    case 64: return Ty::I64;
    case 128: return Ty::I128;
    }
  }
  return std::nullopt;
}

std::optional<std::vector<Reg8>> z80tester::parseRegs(StringRef S) {
  std::vector<Reg8> Regs;
  for (char C : S.upper()) {
    switch (C) {
    case 'A': Regs.push_back(RA); break;
    case 'B': Regs.push_back(RB); break;
    case 'C': Regs.push_back(RC); break;
    case 'D': Regs.push_back(RD); break;
    case 'E': Regs.push_back(RE); break;
    case 'H': Regs.push_back(RH); break;
    case 'L': Regs.push_back(RL); break;
    default: return std::nullopt;
    }
  }
  if (Regs.empty())
    return std::nullopt;
  return Regs;
}

namespace {

/// The register table of one target's __sdcccall(1).
struct Table {
  std::vector<Reg8> First16, First32, AfterI8_8, AfterI8_16, AfterI16_8,
      AfterI16_16, Ret8, Ret16, Ret32;
  std::vector<Reg8> Pairs[3]; // Z80_Builtin pool, in order
};

const Table &table(Cpu C) {
  static const Table Z80 = {{RH, RL},
                            {RH, RL, RD, RE},
                            {RL},
                            {RD, RE},
                            {},
                            {RD, RE},
                            {RA},
                            {RD, RE},
                            {RH, RL, RD, RE},
                            {{RH, RL}, {RD, RE}, {RB, RC}}};
  static const Table SM83 = {{RD, RE},
                             {RD, RE, RB, RC},
                             {RE},
                             {RD, RE},
                             {RA},
                             {RB, RC},
                             {RA},
                             {RB, RC},
                             {RD, RE, RB, RC},
                             {{RD, RE}, {RB, RC}, {RH, RL}}};
  return C == Cpu::Z80 ? Z80 : SM83;
}

std::optional<Loc> retLoc(const Table &Tab, Ty T) {
  switch (sizeOf(T)) {
  case 1: return Loc{Tab.Ret8, 0, 1};
  case 2: return Loc{Tab.Ret16, 0, 2};
  case 4: return Loc{Tab.Ret32, 0, 4};
  default: return std::nullopt;
  }
}

Expected<CallLayout> layoutBuiltin(const Table &Tab, ArrayRef<Ty> Params,
                                   std::optional<Ty> Ret) {
  CallLayout L;
  bool UsedA = false, Used[3] = {false, false, false};
  for (Ty P : Params) {
    unsigned Size = sizeOf(P);
    Loc Lc;
    Lc.Size = Size;
    if (Size == 1 && !UsedA) {
      UsedA = true;
      Lc.Regs = {RA};
    } else if (Size <= 2) {
      for (unsigned I = 0; I < 3 && Lc.Regs.empty(); ++I)
        if (!Used[I]) {
          Used[I] = true;
          Lc.Regs =
              Size == 1 ? std::vector<Reg8>{Tab.Pairs[I][1]} : Tab.Pairs[I];
        }
    } else if (Size == 4) {
      for (unsigned I = 0; I + 1 < 3 && Lc.Regs.empty(); ++I)
        if (!Used[I] && !Used[I + 1]) {
          Used[I] = Used[I + 1] = true;
          Lc.Regs = Tab.Pairs[I];
          Lc.Regs.insert(Lc.Regs.end(), Tab.Pairs[I + 1].begin(),
                         Tab.Pairs[I + 1].end());
        }
    }
    if (Lc.Regs.empty())
      return createStringError("builtin argument does not fit in registers");
    L.Params.push_back(Lc);
  }
  if (Ret) {
    L.Ret = retLoc(Tab, *Ret);
    if (!L.Ret)
      return createStringError("builtin result does not fit in registers");
  }
  return L;
}

} // namespace

Expected<CallLayout> z80tester::layoutCall(Cpu C, CallConv CC,
                                           ArrayRef<Ty> Params,
                                           std::optional<Ty> Ret) {
  const Table &Tab = table(C);
  if (CC == CallConv::Builtin)
    return layoutBuiltin(Tab, Params, Ret);

  CallLayout L;
  unsigned Offset = 0;
  if (Ret && sizeOf(*Ret) > 4) {
    L.SRet = true;
    Offset = 2;
  } else if (Ret) {
    L.Ret = retLoc(Tab, *Ret);
  }

  // The first two positions may take registers; a position is used up even
  // when its argument goes on the stack.
  enum { None, First8, First16 } First = None;
  for (unsigned Pos = 0; Pos < Params.size(); ++Pos) {
    unsigned Size = sizeOf(Params[Pos]);
    Loc Lc;
    Lc.Size = Size;
    if (Pos == 0) {
      if (Size == 1) {
        Lc.Regs = {RA};
        First = First8;
      } else if (Size == 2) {
        Lc.Regs = Tab.First16;
        First = First16;
      } else if (Size == 4) {
        Lc.Regs = Tab.First32;
      }
    } else if (Pos == 1) {
      if (First == First8)
        Lc.Regs = Size == 1   ? Tab.AfterI8_8
                  : Size == 2 ? Tab.AfterI8_16
                              : std::vector<Reg8>{};
      else if (First == First16)
        Lc.Regs = Size == 1   ? Tab.AfterI16_8
                  : Size == 2 ? Tab.AfterI16_16
                              : std::vector<Reg8>{};
    }
    if (Lc.Regs.empty()) {
      Lc.Offset = Offset;
      Offset += Size;
    }
    L.Params.push_back(Lc);
  }
  L.StackBytes = Offset;

  if (C == Cpu::SM83) {
    L.CalleeCleanup = true;
  } else {
    unsigned RetBits = Ret ? sizeOf(*Ret) * 8 : 0;
    L.CalleeCleanup = RetBits <= 16 || (Ret == Ty::F32 && !Params.empty() &&
                                        Params[0] == Ty::F32);
  }
  return L;
}
