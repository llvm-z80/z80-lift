// SM83 instruction semantics, built natively for the interpreter and as
// bitcode for the lifter.

#include "semantics/Declarations.h"
#include "semantics/Flags.h"

using namespace z80core;
using namespace z80core::sem;

namespace {

// Selectors; the decoder uses the same numbering.
enum R8 { B, C, D, E, H, L, MHL, A };
enum R16 { BC, DE, HL, SP, AF };
enum Alu { ADD, ADC, SUB, SBC, AND, XOR, OR, CP };
enum Rot { RLC, RRC, RL, RR, SLA, SRA, SWAP, SRL };

uint16_t hl(State *S) { return pair(S->H, S->L); }

void setHL(State *S, uint16_t V) {
  S->H = V >> 8;
  S->L = V;
}

uint8_t getF(State *S) {
  return S->ZF << 7 | S->NF << 6 | S->HF << 5 | S->CF << 4;
}

void setF(State *S, uint8_t F) {
  S->ZF = F & 0x80;
  S->NF = F & 0x40;
  S->HF = F & 0x20;
  S->CF = F & 0x10;
}

uint8_t get8(State *S, uint8_t *M, unsigned R) {
  switch (R) {
  case B: return S->B;
  case C: return S->C;
  case D: return S->D;
  case E: return S->E;
  case H: return S->H;
  case L: return S->L;
  case A: return S->A;
  default: return M[hl(S)];
  }
}

void set8(State *S, uint8_t *M, unsigned R, uint8_t V) {
  switch (R) {
  case B: S->B = V; return;
  case C: S->C = V; return;
  case D: S->D = V; return;
  case E: S->E = V; return;
  case H: S->H = V; return;
  case L: S->L = V; return;
  case A: S->A = V; return;
  default: write8(S, M, hl(S), V); return;
  }
}

uint16_t get16(State *S, unsigned R) {
  switch (R) {
  case BC: return pair(S->B, S->C);
  case DE: return pair(S->D, S->E);
  case HL: return hl(S);
  case SP: return S->SP;
  default: return pair(S->A, getF(S));
  }
}

void set16(State *S, unsigned R, uint16_t V) {
  uint8_t Hi = V >> 8, Lo = V;
  switch (R) {
  case BC:
    S->B = Hi;
    S->C = Lo;
    return;
  case DE:
    S->D = Hi;
    S->E = Lo;
    return;
  case HL:
    S->H = Hi;
    S->L = Lo;
    return;
  case SP: S->SP = V; return;
  default:
    S->A = Hi;
    setF(S, Lo);
    return;
  }
}

bool cond(State *S, unsigned CC) {
  switch (CC) {
  case 0: return !S->ZF;
  case 1: return S->ZF;
  case 2: return !S->CF;
  default: return S->CF;
  }
}

void alu(State *S, unsigned Op, uint8_t V) {
  uint8_t X = S->A;
  switch (Op) {
  case ADD:
  case ADC: {
    unsigned Cin = Op == ADC && S->CF;
    unsigned R = X + V + Cin;
    S->HF = (X & 0xF) + (V & 0xF) + Cin > 0xF;
    S->CF = R > 0xFF;
    S->NF = false;
    S->A = R;
    S->ZF = S->A == 0;
    return;
  }
  case SUB:
  case SBC:
  case CP: {
    int Cin = Op == SBC && S->CF;
    int R = X - V - Cin;
    S->HF = (X & 0xF) - (V & 0xF) - Cin < 0;
    S->CF = R < 0;
    S->NF = true;
    uint8_t Res = R;
    S->ZF = Res == 0;
    if (Op != CP)
      S->A = Res;
    return;
  }
  case AND:
    S->A = X & V;
    S->HF = true;
    break;
  case XOR:
    S->A = X ^ V;
    S->HF = false;
    break;
  default:
    S->A = X | V;
    S->HF = false;
    break;
  }
  S->ZF = S->A == 0;
  S->NF = false;
  S->CF = false;
}

uint8_t rot(State *S, unsigned Op, uint8_t V) {
  bool Out;
  uint8_t R;
  switch (Op) {
  case RLC:
    Out = V >> 7;
    R = V << 1 | Out;
    break;
  case RRC:
    Out = V & 1;
    R = V >> 1 | Out << 7;
    break;
  case RL:
    Out = V >> 7;
    R = V << 1 | S->CF;
    break;
  case RR:
    Out = V & 1;
    R = V >> 1 | S->CF << 7;
    break;
  case SLA:
    Out = V >> 7;
    R = V << 1;
    break;
  case SRA:
    Out = V & 1;
    R = V >> 1 | (V & 0x80);
    break;
  case SWAP:
    Out = false;
    R = V << 4 | V >> 4;
    break;
  default:
    Out = V & 1;
    R = V >> 1;
    break;
  }
  S->ZF = R == 0;
  S->CF = Out;
  S->HF = S->NF = false;
  return R;
}

/// Flags of ADD SP,e and LD HL,SP+e come from the low byte.
uint16_t addSP(State *S, unsigned E) {
  uint8_t U = E;
  S->HF = (S->SP & 0xF) + (U & 0xF) > 0xF;
  S->CF = (S->SP & 0xFF) + U > 0xFF;
  S->ZF = S->NF = false;
  return S->SP + int8_t(U);
}

} // namespace

#define SEM(Name)                                                              \
  extern "C" void sm83_##Name(State *S, uint8_t *M, unsigned a, unsigned b,    \
                              unsigned c)

SEM(NOP) {}
SEM(LD_R8_R8) { set8(S, M, a, get8(S, M, b)); }
SEM(LD_R8_N) { set8(S, M, a, b); }
SEM(LD_R16_NN) { set16(S, a, b); }
SEM(LD_MR16_A) { write8(S, M, get16(S, a), S->A); }
SEM(LD_A_MR16) { S->A = M[get16(S, a)]; }

SEM(LD_MHLI_A) {
  write8(S, M, hl(S), S->A);
  setHL(S, hl(S) + 1);
}

SEM(LD_MHLD_A) {
  write8(S, M, hl(S), S->A);
  setHL(S, hl(S) - 1);
}

SEM(LD_A_MHLI) {
  S->A = M[hl(S)];
  setHL(S, hl(S) + 1);
}

SEM(LD_A_MHLD) {
  S->A = M[hl(S)];
  setHL(S, hl(S) - 1);
}

SEM(LD_MNN_SP) { write16(S, M, a, S->SP); }
SEM(LD_MNN_A) { write8(S, M, a, S->A); }
SEM(LD_A_MNN) { S->A = M[uint16_t(a)]; }
SEM(LDH_MN_A) { write8(S, M, 0xFF00 | a, S->A); }
SEM(LDH_A_MN) { S->A = M[0xFF00 | a]; }
SEM(LDH_MC_A) { write8(S, M, 0xFF00 | S->C, S->A); }
SEM(LDH_A_MC) { S->A = M[0xFF00 | S->C]; }
SEM(LD_HL_SPE) { setHL(S, addSP(S, a)); }
SEM(LD_SP_HL) { S->SP = hl(S); }
SEM(ADD_SP_E) { S->SP = addSP(S, a); }
SEM(PUSH) { push16(S, M, get16(S, a)); }
SEM(POP) { set16(S, a, pop16(S, M)); }
SEM(ALU_R8) { alu(S, a, get8(S, M, b)); }
SEM(ALU_N) { alu(S, a, b); }

SEM(INC_R8) {
  uint8_t V = get8(S, M, a), R = V + 1;
  S->HF = (V & 0xF) == 0xF;
  S->NF = false;
  S->ZF = R == 0;
  set8(S, M, a, R);
}

SEM(DEC_R8) {
  uint8_t V = get8(S, M, a), R = V - 1;
  S->HF = (V & 0xF) == 0;
  S->NF = true;
  S->ZF = R == 0;
  set8(S, M, a, R);
}

SEM(INC_R16) { set16(S, a, get16(S, a) + 1); }
SEM(DEC_R16) { set16(S, a, get16(S, a) - 1); }

SEM(ADD_HL_R16) {
  unsigned X = hl(S), Y = get16(S, a), R = X + Y;
  S->HF = (X & 0xFFF) + (Y & 0xFFF) > 0xFFF;
  S->CF = R > 0xFFFF;
  S->NF = false;
  setHL(S, R);
}

SEM(RLCA) {
  bool Out = S->A >> 7;
  S->A = S->A << 1 | Out;
  S->CF = Out;
  S->ZF = S->HF = S->NF = false;
}

SEM(RRCA) {
  bool Out = S->A & 1;
  S->A = S->A >> 1 | Out << 7;
  S->CF = Out;
  S->ZF = S->HF = S->NF = false;
}

SEM(RLA) {
  bool Out = S->A >> 7;
  S->A = S->A << 1 | S->CF;
  S->CF = Out;
  S->ZF = S->HF = S->NF = false;
}

SEM(RRA) {
  bool Out = S->A & 1;
  S->A = S->A >> 1 | S->CF << 7;
  S->CF = Out;
  S->ZF = S->HF = S->NF = false;
}

SEM(ROT) { set8(S, M, b, rot(S, a, get8(S, M, b))); }

SEM(BIT) {
  S->ZF = !(get8(S, M, b) >> a & 1);
  S->HF = true;
  S->NF = false;
}

SEM(RES) { set8(S, M, b, get8(S, M, b) & ~(1u << a)); }
SEM(SET) { set8(S, M, b, get8(S, M, b) | 1u << a); }

SEM(DAA) {
  uint8_t X = S->A;
  if (!S->NF) {
    if (S->CF || X > 0x99) {
      X += 0x60;
      S->CF = true;
    }
    if (S->HF || (X & 0xF) > 9)
      X += 0x06;
  } else {
    if (S->CF)
      X -= 0x60;
    if (S->HF)
      X -= 0x06;
  }
  S->A = X;
  S->ZF = X == 0;
  S->HF = false;
}

SEM(CPL) {
  S->A = ~S->A;
  S->HF = S->NF = true;
}

SEM(SCF) {
  S->CF = true;
  S->HF = S->NF = false;
}

SEM(CCF) {
  S->CF = !S->CF;
  S->HF = S->NF = false;
}

SEM(DI) { S->IFF1 = false; }
SEM(EI) { S->IFF1 = true; }

SEM(JP) { S->PC = a; }
SEM(JP_CC) {
  if (cond(S, a))
    S->PC = b;
}
SEM(JR) { S->PC = a; }
SEM(JR_CC) {
  if (cond(S, a))
    S->PC = b;
}
SEM(JP_HL) { S->PC = hl(S); }

SEM(CALL) {
  push16(S, M, S->PC);
  S->PC = a;
}

SEM(CALL_CC) {
  if (cond(S, a)) {
    push16(S, M, S->PC);
    S->PC = b;
  }
}

SEM(RST) {
  push16(S, M, S->PC);
  S->PC = a;
}

SEM(RET) { S->PC = pop16(S, M); }
SEM(RET_CC) {
  if (cond(S, a))
    S->PC = pop16(S, M);
}

SEM(RETI) {
  S->IFF1 = true;
  S->PC = pop16(S, M);
}

SEM(HALT) { S->Halted = true; }
SEM(STOP) { S->Halted = true; }
