// SM83 decoder. Follows the x/y/z/p/q split of the opcode byte like the Z80
// decoder; the SM83 replaces the Z80's I/O, exchange and prefix opcodes.

#include "Insns.h"

#include <cstdarg>
#include <cstdio>

using namespace z80core;
using namespace z80core::sm83;

namespace {

// Selectors; must match SM83/Semantics.cpp.
enum R16 { BC, DE, HL, SP, AF };

const char *const R8Names[] = {"b", "c", "d", "e", "h", "l", "(hl)", "a"};
const char *const R16Names[] = {"bc", "de", "hl", "sp", "af"};
const char *const CCNames[] = {"nz", "z", "nc", "c"};
const char *const AluNames[] = {"add a,", "adc a,", "sub ", "sbc a,",
                                "and ",   "xor ",   "or ",  "cp "};
const char *const RotNames[] = {"rlc", "rrc", "rl",   "rr",
                                "sla", "sra", "swap", "srl"};

std::string format(const char *Fmt, ...) {
  char Buf[64];
  va_list Ap;
  va_start(Ap, Fmt);
  vsnprintf(Buf, sizeof Buf, Fmt, Ap);
  va_end(Ap);
  return Buf;
}

std::string signedByte(unsigned E) {
  int V = int8_t(E);
  return format("%c0x%02x", V < 0 ? '-' : '+', V < 0 ? -V : V);
}

class Decoder {
public:
  Decoder(const uint8_t *M, uint16_t Addr, Inst &I, std::string *Text)
      : M(M), Pos(Addr), I(I), Text(Text) {
    I = Inst();
    I.Addr = Addr;
  }

  bool run() {
    uint8_t Opc = fetch();
    bool Ok = Opc == 0xCB ? cb() : base(Opc);
    I.Len = uint16_t(Pos - I.Addr);
    return Ok;
  }

private:
  const uint8_t *M;
  uint16_t Pos;
  Inst &I;
  std::string *Text;

  uint8_t fetch() { return M[Pos++]; }

  uint16_t fetch16() {
    uint16_t Lo = fetch();
    return Lo | fetch() << 8;
  }

  uint16_t rel() {
    int8_t D = fetch();
    return Pos + D;
  }

  template <typename F> void print(F &&Fn) {
    if (Text)
      *Text = Fn();
  }

  bool emit(Op O, unsigned A = 0, unsigned B = 0) {
    I.Op = O;
    I.Args[0] = A;
    I.Args[1] = B;
    I.K = instKind(Cpu::SM83, O);
    return true;
  }

  bool emitTo(Op O, uint16_t Dest, unsigned A = 0, unsigned B = 0) {
    I.Dest = Dest;
    return emit(O, A, B);
  }

  static unsigned rp(unsigned P2) { return P2; } // BC DE HL SP

  static unsigned rp2(unsigned P2) {
    const unsigned T[] = {BC, DE, HL, AF};
    return T[P2];
  }

  bool base(uint8_t Opc) {
    unsigned X = Opc >> 6, Y = Opc >> 3 & 7, Z = Opc & 7;
    switch (X) {
    case 0: return block0(Y, Z);
    case 1:
      if (Y == 6 && Z == 6) {
        print([] { return std::string("halt"); });
        return emit(HALT);
      }
      print([&] { return format("ld %s,%s", R8Names[Y], R8Names[Z]); });
      return emit(LD_R8_R8, Y, Z);
    case 2:
      print([&] { return format("%s%s", AluNames[Y], R8Names[Z]); });
      return emit(ALU_R8, Y, Z);
    default: return block3(Y, Z);
    }
  }

  bool block0(unsigned Y, unsigned Z) {
    unsigned P2 = Y >> 1, Q = Y & 1;
    switch (Z) {
    case 0:
      switch (Y) {
      case 0: print([] { return std::string("nop"); }); return emit(NOP);
      case 1: {
        uint16_t NN = fetch16();
        print([&] { return format("ld (0x%04x),sp", NN); });
        return emit(LD_MNN_SP, NN);
      }
      case 2:
        fetch(); // STOP is followed by a padding byte
        print([] { return std::string("stop"); });
        return emit(STOP);
      case 3: {
        uint16_t T = rel();
        print([&] { return format("jr 0x%04x", T); });
        return emitTo(JR, T, T);
      }
      default: {
        uint16_t T = rel();
        print([&] { return format("jr %s,0x%04x", CCNames[Y - 4], T); });
        return emitTo(JR_CC, T, Y - 4, T);
      }
      }
    case 1:
      if (!Q) {
        uint16_t NN = fetch16();
        print([&] { return format("ld %s,0x%04x", R16Names[rp(P2)], NN); });
        return emit(LD_R16_NN, rp(P2), NN);
      }
      print([&] { return format("add hl,%s", R16Names[rp(P2)]); });
      return emit(ADD_HL_R16, rp(P2));
    case 2: {
      if (P2 < 2) {
        unsigned R = P2 == 0 ? BC : DE;
        if (Q) {
          print([&] { return format("ld a,(%s)", R16Names[R]); });
          return emit(LD_A_MR16, R);
        }
        print([&] { return format("ld (%s),a", R16Names[R]); });
        return emit(LD_MR16_A, R);
      }
      const Op Ops[2][2] = {{LD_MHLI_A, LD_A_MHLI}, {LD_MHLD_A, LD_A_MHLD}};
      const char *const Names[2][2] = {{"ld (hl+),a", "ld a,(hl+)"},
                                       {"ld (hl-),a", "ld a,(hl-)"}};
      print([&] { return std::string(Names[P2 - 2][Q]); });
      return emit(Ops[P2 - 2][Q]);
    }
    case 3:
      print(
          [&] { return format("%s %s", Q ? "dec" : "inc", R16Names[rp(P2)]); });
      return emit(Q ? DEC_R16 : INC_R16, rp(P2));
    case 4:
    case 5:
      print(
          [&] { return format("%s %s", Z == 4 ? "inc" : "dec", R8Names[Y]); });
      return emit(Z == 4 ? INC_R8 : DEC_R8, Y);
    case 6: {
      unsigned N = fetch();
      print([&] { return format("ld %s,0x%02x", R8Names[Y], N); });
      return emit(LD_R8_N, Y, N);
    }
    default: {
      const Op Ops[] = {RLCA, RRCA, RLA, RRA, DAA, CPL, SCF, CCF};
      const char *const Names[] = {"rlca", "rrca", "rla", "rra",
                                   "daa",  "cpl",  "scf", "ccf"};
      print([&] { return std::string(Names[Y]); });
      return emit(Ops[Y]);
    }
    }
  }

  bool block3(unsigned Y, unsigned Z) {
    unsigned P2 = Y >> 1, Q = Y & 1;
    switch (Z) {
    case 0:
      switch (Y) {
      case 4: {
        unsigned N = fetch();
        print([&] { return format("ldh (0x%02x),a", N); });
        return emit(LDH_MN_A, N);
      }
      case 5: {
        unsigned E = fetch();
        print([&] { return format("add sp,%s", signedByte(E).c_str()); });
        return emit(ADD_SP_E, E);
      }
      case 6: {
        unsigned N = fetch();
        print([&] { return format("ldh a,(0x%02x)", N); });
        return emit(LDH_A_MN, N);
      }
      case 7: {
        unsigned E = fetch();
        print([&] { return format("ld hl,sp%s", signedByte(E).c_str()); });
        return emit(LD_HL_SPE, E);
      }
      default:
        print([&] { return format("ret %s", CCNames[Y]); });
        return emit(RET_CC, Y);
      }
    case 1:
      if (!Q) {
        print([&] { return format("pop %s", R16Names[rp2(P2)]); });
        return emit(POP, rp2(P2));
      }
      switch (P2) {
      case 0: print([] { return std::string("ret"); }); return emit(RET);
      case 1: print([] { return std::string("reti"); }); return emit(RETI);
      case 2: print([] { return std::string("jp (hl)"); }); return emit(JP_HL);
      default:
        print([] { return std::string("ld sp,hl"); });
        return emit(LD_SP_HL);
      }
    case 2:
      switch (Y) {
      case 4:
        print([] { return std::string("ldh (c),a"); });
        return emit(LDH_MC_A);
      case 5: {
        uint16_t NN = fetch16();
        print([&] { return format("ld (0x%04x),a", NN); });
        return emit(LD_MNN_A, NN);
      }
      case 6:
        print([] { return std::string("ldh a,(c)"); });
        return emit(LDH_A_MC);
      case 7: {
        uint16_t NN = fetch16();
        print([&] { return format("ld a,(0x%04x)", NN); });
        return emit(LD_A_MNN, NN);
      }
      default: {
        uint16_t NN = fetch16();
        print([&] { return format("jp %s,0x%04x", CCNames[Y], NN); });
        return emitTo(JP_CC, NN, Y, NN);
      }
      }
    case 3:
      switch (Y) {
      case 0: {
        uint16_t NN = fetch16();
        print([&] { return format("jp 0x%04x", NN); });
        return emitTo(JP, NN, NN);
      }
      case 6: print([] { return std::string("di"); }); return emit(DI);
      case 7: print([] { return std::string("ei"); }); return emit(EI);
      default: return false; // CB is handled in run(); the rest are undefined
      }
    case 4: {
      if (Y >= 4)
        return false;
      uint16_t NN = fetch16();
      print([&] { return format("call %s,0x%04x", CCNames[Y], NN); });
      return emitTo(CALL_CC, NN, Y, NN);
    }
    case 5:
      if (!Q) {
        print([&] { return format("push %s", R16Names[rp2(P2)]); });
        return emit(PUSH, rp2(P2));
      }
      if (P2 == 0) {
        uint16_t NN = fetch16();
        print([&] { return format("call 0x%04x", NN); });
        return emitTo(CALL, NN, NN);
      }
      return false;
    case 6: {
      unsigned N = fetch();
      print([&] { return format("%s0x%02x", AluNames[Y], N); });
      return emit(ALU_N, Y, N);
    }
    default: {
      uint16_t T = Y * 8;
      print([&] { return format("rst 0x%02x", T); });
      return emitTo(RST, T, T);
    }
    }
  }

  bool cb() {
    uint8_t Opc = fetch();
    unsigned X = Opc >> 6, Y = Opc >> 3 & 7, R = Opc & 7;
    switch (X) {
    case 0:
      print([&] { return format("%s %s", RotNames[Y], R8Names[R]); });
      return emit(ROT, Y, R);
    case 1:
      print([&] { return format("bit %u,%s", Y, R8Names[R]); });
      return emit(BIT, Y, R);
    case 2:
      print([&] { return format("res %u,%s", Y, R8Names[R]); });
      return emit(RES, Y, R);
    default:
      print([&] { return format("set %u,%s", Y, R8Names[R]); });
      return emit(SET, Y, R);
    }
  }
};

} // namespace

bool z80core::decodeSM83(const uint8_t *Mem, uint16_t Addr, Inst &I,
                         std::string *Text) {
  return Decoder(Mem, Addr, I, Text).run();
}
