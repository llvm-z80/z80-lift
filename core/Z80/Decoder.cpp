// Z80 decoder for the documented instruction set, without I/O. Follows the
// x/y/z/p/q split of the opcode byte.

#include "Insns.h"

using namespace z80core;
using namespace z80core::z80;

namespace {

// Selectors; must match Z80/Semantics.cpp.
enum R8 { B, C, D, E, H, L, MHL, A, IXH, IXL, IYH, IYL, MIX, MIY };
enum R16 { BC, DE, HL, SP, AF, IX, IY };
enum Prefix { NoPrefix, PrefixIX, PrefixIY };

bool isIndexed(unsigned R) { return R == MIX || R == MIY; }

class Decoder {
public:
  Decoder(const uint8_t *M, uint16_t Addr, Inst &I) : M(M), Pos(Addr), I(I) {
    I = Inst();
    I.Addr = Addr;
  }

  bool run() {
    uint8_t Opc = fetch();
    bool Ok;
    switch (Opc) {
    case 0xCB: Ok = cb(); break;
    case 0xED: Ok = ed(); break;
    case 0xDD: Ok = base(fetch(), PrefixIX); break;
    case 0xFD: Ok = base(fetch(), PrefixIY); break;
    default: Ok = base(Opc, NoPrefix); break;
    }
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

  bool emit(Op O, unsigned A = 0, unsigned B = 0, unsigned C = 0) {
    I.Op = O;
    I.Args[0] = A;
    I.Args[1] = B;
    I.Args[2] = C;
    I.K = instKind(Cpu::Z80, O);
    return true;
  }

  bool emitTo(Op O, uint16_t Dest, unsigned A = 0, unsigned B = 0) {
    I.Dest = Dest;
    return emit(O, A, B);
  }

  // With a DD or FD prefix, H, L and (HL) become the index register halves and
  // (IX+d); H and L stay when (IX+d) is the other operand.
  static unsigned reg8(unsigned Idx, Prefix P, bool HasMem) {
    if (P == NoPrefix)
      return Idx;
    if (Idx == 6)
      return P == PrefixIX ? MIX : MIY;
    if (HasMem)
      return Idx;
    if (Idx == 4)
      return P == PrefixIX ? IXH : IYH;
    if (Idx == 5)
      return P == PrefixIX ? IXL : IYL;
    return Idx;
  }

  static unsigned hlReg(Prefix P) {
    return P == PrefixIX ? IX : P == PrefixIY ? IY : HL;
  }

  static unsigned rp(unsigned P2, Prefix P) {
    const unsigned T[] = {BC, DE, HL, SP};
    return T[P2] == HL ? hlReg(P) : T[P2];
  }

  static unsigned rp2(unsigned P2, Prefix P) {
    const unsigned T[] = {BC, DE, HL, AF};
    return T[P2] == HL ? hlReg(P) : T[P2];
  }

  unsigned disp(unsigned R) { return isIndexed(R) ? fetch() : 0; }

  bool base(uint8_t Opc, Prefix P) {
    unsigned X = Opc >> 6, Y = Opc >> 3 & 7, Z = Opc & 7;
    switch (X) {
    case 0: return block0(Y, Z, P);
    case 1: {
      if (Y == 6 && Z == 6) {
        return emit(HALT);
      }
      bool Mem = Y == 6 || Z == 6;
      unsigned Dst = reg8(Y, P, Mem), Src = reg8(Z, P, Mem);
      unsigned D = isIndexed(Dst) || isIndexed(Src) ? fetch() : 0;
      return emit(LD_R8_R8, Dst, Src, D);
    }
    case 2: {
      unsigned Src = reg8(Z, P, false), D = disp(Src);
      return emit(ALU_R8, Y, Src, D);
    }
    default: return block3(Y, Z, P);
    }
  }

  bool block0(unsigned Y, unsigned Z, Prefix P) {
    unsigned P2 = Y >> 1, Q = Y & 1;
    switch (Z) {
    case 0:
      switch (Y) {
      case 0: return emit(NOP);
      case 1: return emit(EX_AF);
      case 2: {
        uint16_t T = rel();
        return emitTo(DJNZ, T, T);
      }
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
        unsigned R = rp(P2, P);
        uint16_t NN = fetch16();
        return emit(LD_R16_NN, R, NN);
      } else {
        unsigned Dst = hlReg(P), Src = rp(P2, P);
        return emit(ADD_R16, Dst, Src);
      }
    case 2:
      switch (P2) {
      case 0:
      case 1: {
        unsigned R = P2 == 0 ? BC : DE;
        if (Q) {
          return emit(LD_A_MR16, R);
        }
        return emit(LD_MR16_A, R);
      }
      case 2: {
        unsigned R = hlReg(P);
        uint16_t NN = fetch16();
        if (Q) {
          return emit(LD_R16_MNN, R, NN);
        }
        return emit(LD_MNN_R16, R, NN);
      }
      default: {
        uint16_t NN = fetch16();
        if (Q) {
          return emit(LD_A_MNN, NN);
        }
        return emit(LD_MNN_A, NN);
      }
      }
    case 3: {
      unsigned R = rp(P2, P);
      return emit(Q ? DEC_R16 : INC_R16, R);
    }
    case 4:
    case 5: {
      unsigned R = reg8(Y, P, false), D = disp(R);
      return emit(Z == 4 ? INC_R8 : DEC_R8, R, D);
    }
    case 6: {
      unsigned R = reg8(Y, P, false), D = disp(R), N = fetch();
      return emit(LD_R8_N, R, N, D);
    }
    default: {
      const Op Ops[] = {RLCA, RRCA, RLA, RRA, DAA, CPL, SCF, CCF};
      return emit(Ops[Y]);
    }
    }
  }

  bool block3(unsigned Y, unsigned Z, Prefix P) {
    unsigned P2 = Y >> 1, Q = Y & 1;
    switch (Z) {
    case 0: return emit(RET_CC, Y);
    case 1:
      if (!Q) {
        unsigned R = rp2(P2, P);
        return emit(POP, R);
      }
      switch (P2) {
      case 0: return emit(RET);
      case 1: return emit(EXX);
      case 2: {
        unsigned R = hlReg(P);
        return emit(JP_R16, R);
      }
      default: {
        unsigned R = hlReg(P);
        return emit(LD_SP_R16, R);
      }
      }
    case 2: {
      uint16_t NN = fetch16();
      return emitTo(JP_CC, NN, Y, NN);
    }
    case 3:
      switch (Y) {
      case 0: {
        uint16_t NN = fetch16();
        return emitTo(JP, NN, NN);
      }
      case 1: return P == NoPrefix ? cb() : indexedCB(P);
      case 2:
      case 3: return false; // OUT (n),A and IN A,(n)
      case 4: {
        unsigned R = hlReg(P);
        return emit(EX_MSP_R16, R);
      }
      case 5: return emit(EX_DE_HL);
      case 6: return emit(DI);
      default: return emit(EI);
      }
    case 4: {
      uint16_t NN = fetch16();
      return emitTo(CALL_CC, NN, Y, NN);
    }
    case 5:
      if (!Q) {
        unsigned R = rp2(P2, P);
        return emit(PUSH, R);
      }
      if (P2 == 0) {
        uint16_t NN = fetch16();
        return emitTo(CALL, NN, NN);
      }
      return false; // a second DD, ED or FD prefix
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

  bool bitOp(uint8_t Opc, unsigned R, unsigned D) {
    unsigned X = Opc >> 6, Y = Opc >> 3 & 7;
    switch (X) {
    case 0: return emit(ROT, Y, R, D);
    case 1: return emit(BIT, Y, R, D);
    case 2: return emit(RES, Y, R, D);
    default: return emit(SET, Y, R, D);
    }
  }

  bool cb() {
    uint8_t Opc = fetch();
    return bitOp(Opc, Opc & 7, 0);
  }

  // DD CB d op. Forms other than BIT that also copy the result to a register
  // are undocumented and rejected.
  bool indexedCB(Prefix P) {
    unsigned D = fetch();
    uint8_t Opc = fetch();
    if (Opc >> 6 != 1 && (Opc & 7) != 6)
      return false;
    return bitOp(Opc, P == PrefixIX ? MIX : MIY, D);
  }

  bool ed() {
    uint8_t Opc = fetch();
    unsigned X = Opc >> 6, Y = Opc >> 3 & 7, Z = Opc & 7;
    unsigned P2 = Y >> 1, Q = Y & 1;
    if (X == 1) {
      switch (Z) {
      case 0:
      case 1: return false; // IN r,(C) and OUT (C),r
      case 2: {
        unsigned R = rp(P2, NoPrefix);
        return emit(Q ? ADC_HL : SBC_HL, R);
      }
      case 3: {
        unsigned R = rp(P2, NoPrefix);
        uint16_t NN = fetch16();
        if (Q) {
          return emit(LD_R16_MNN, R, NN);
        }
        return emit(LD_MNN_R16, R, NN);
      }
      case 4: return emit(NEG);
      case 5: return emit(Y == 1 ? RETI : RETN);
      case 6: {
        const unsigned Modes[] = {0, 0, 1, 2, 0, 0, 1, 2};
        return emit(IM, Modes[Y]);
      }
      default: {
        const Op Ops[] = {LD_I_A, LD_R_A, LD_A_I, LD_A_R, RRD, RLD, NOP, NOP};
        return emit(Ops[Y]);
      }
      }
    }
    if (X == 2 && Y >= 4 && Z <= 1) {
      const Op Ops[4][2] = {{LDI, CPI}, {LDD, CPD}, {LDIR, CPIR}, {LDDR, CPDR}};
      return Y >= 6 ? emitTo(Ops[Y - 4][Z], I.Addr) : emit(Ops[Y - 4][Z]);
    }
    return false; // block I/O and undefined ED opcodes
  }
};

} // namespace

bool z80core::decodeZ80(const uint8_t *Mem, uint16_t Addr, Inst &I) {
  return Decoder(Mem, Addr, I).run();
}
