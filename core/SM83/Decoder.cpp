// SM83 decoder. Follows the x/y/z/p/q split of the opcode byte like the Z80
// decoder; the SM83 replaces the Z80's I/O, exchange and prefix opcodes.

#include "Insns.h"

using namespace z80core;
using namespace z80core::sm83;

namespace {

// Selectors; must match SM83/Semantics.cpp.
enum R16 { BC, DE, HL, SP, AF };

class Decoder {
public:
  Decoder(const uint8_t *M, uint16_t Addr, Inst &I) : M(M), Pos(Addr), I(I) {
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

  uint8_t fetch() { return M[Pos++]; }

  uint16_t fetch16() {
    uint16_t Lo = fetch();
    return Lo | fetch() << 8;
  }

  uint16_t rel() {
    int8_t D = fetch();
    return Pos + D;
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
        return emit(HALT);
      }
      return emit(LD_R8_R8, Y, Z);
    case 2: return emit(ALU_R8, Y, Z);
    default: return block3(Y, Z);
    }
  }

  bool block0(unsigned Y, unsigned Z) {
    unsigned P2 = Y >> 1, Q = Y & 1;
    switch (Z) {
    case 0:
      switch (Y) {
      case 0: return emit(NOP);
      case 1: {
        uint16_t NN = fetch16();
        return emit(LD_MNN_SP, NN);
      }
      case 2:
        fetch(); // STOP is followed by a padding byte
        return emit(STOP);
      case 3: {
        uint16_t T = rel();
        return emitTo(JR, T, T);
      }
      default: {
        uint16_t T = rel();
        return emitTo(JR_CC, T, Y - 4, T);
      }
      }
    case 1:
      if (!Q) {
        uint16_t NN = fetch16();
        return emit(LD_R16_NN, rp(P2), NN);
      }
      return emit(ADD_HL_R16, rp(P2));
    case 2: {
      if (P2 < 2) {
        unsigned R = P2 == 0 ? BC : DE;
        if (Q) {
          return emit(LD_A_MR16, R);
        }
        return emit(LD_MR16_A, R);
      }
      const Op Ops[2][2] = {{LD_MHLI_A, LD_A_MHLI}, {LD_MHLD_A, LD_A_MHLD}};
      return emit(Ops[P2 - 2][Q]);
    }
    case 3: return emit(Q ? DEC_R16 : INC_R16, rp(P2));
    case 4:
    case 5: return emit(Z == 4 ? INC_R8 : DEC_R8, Y);
    case 6: {
      unsigned N = fetch();
      return emit(LD_R8_N, Y, N);
    }
    default: {
      const Op Ops[] = {RLCA, RRCA, RLA, RRA, DAA, CPL, SCF, CCF};
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
        return emit(LDH_MN_A, N);
      }
      case 5: {
        unsigned E = fetch();
        return emit(ADD_SP_E, E);
      }
      case 6: {
        unsigned N = fetch();
        return emit(LDH_A_MN, N);
      }
      case 7: {
        unsigned E = fetch();
        return emit(LD_HL_SPE, E);
      }
      default: return emit(RET_CC, Y);
      }
    case 1:
      if (!Q) {
        return emit(POP, rp2(P2));
      }
      switch (P2) {
      case 0: return emit(RET);
      case 1: return emit(RETI);
      case 2: return emit(JP_HL);
      default: return emit(LD_SP_HL);
      }
    case 2:
      switch (Y) {
      case 4: return emit(LDH_MC_A);
      case 5: {
        uint16_t NN = fetch16();
        return emit(LD_MNN_A, NN);
      }
      case 6: return emit(LDH_A_MC);
      case 7: {
        uint16_t NN = fetch16();
        return emit(LD_A_MNN, NN);
      }
      default: {
        uint16_t NN = fetch16();
        return emitTo(JP_CC, NN, Y, NN);
      }
      }
    case 3:
      switch (Y) {
      case 0: {
        uint16_t NN = fetch16();
        return emitTo(JP, NN, NN);
      }
      case 6: return emit(DI);
      case 7: return emit(EI);
      default: return false; // CB is handled in run(); the rest are undefined
      }
    case 4: {
      if (Y >= 4)
        return false;
      uint16_t NN = fetch16();
      return emitTo(CALL_CC, NN, Y, NN);
    }
    case 5:
      if (!Q) {
        return emit(PUSH, rp2(P2));
      }
      if (P2 == 0) {
        uint16_t NN = fetch16();
        return emitTo(CALL, NN, NN);
      }
      return false;
    case 6: {
      unsigned N = fetch();
      return emit(ALU_N, Y, N);
    }
    default: {
      uint16_t T = Y * 8;
      return emitTo(RST, T, T);
    }
    }
  }

  bool cb() {
    uint8_t Opc = fetch();
    unsigned X = Opc >> 6, Y = Opc >> 3 & 7, R = Opc & 7;
    switch (X) {
    case 0: return emit(ROT, Y, R);
    case 1: return emit(BIT, Y, R);
    case 2: return emit(RES, Y, R);
    default: return emit(SET, Y, R);
    }
  }
};

} // namespace

bool z80core::decodeSM83(const uint8_t *Mem, uint16_t Addr, Inst &I) {
  return Decoder(Mem, Addr, I).run();
}
