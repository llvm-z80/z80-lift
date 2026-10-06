// Z80 instruction semantics, built natively for the interpreter and as bitcode
// for the lifter. Undocumented flag bits 3 and 5 are not modelled.

#include "semantics/Declarations.h"
#include "semantics/Flags.h"

using namespace z80core;
using namespace z80core::sem;

namespace {

// Selectors; the decoder uses the same numbering.
enum R8 { B, C, D, E, H, L, MHL, A, IXH, IXL, IYH, IYL, MIX, MIY };
enum R16 { BC, DE, HL, SP, AF, IX, IY };
enum Alu { ADD, ADC, SUB, SBC, AND, XOR, OR, CP };
enum Rot { RLC, RRC, RL, RR, SLA, SRA, SLL, SRL };

} // namespace

static uint16_t hl(State *S) { return pair(S->H, S->L); }
static uint16_t ix(State *S) { return pair(S->IXH, S->IXL); }
static uint16_t iy(State *S) { return pair(S->IYH, S->IYL); }

static uint8_t getF(State *S) {
  return S->SF << 7 | S->ZF << 6 | S->HF << 4 | S->PVF << 2 | S->NF << 1 |
         S->CF;
}

static void setF(State *S, uint8_t F) {
  S->SF = F & 0x80;
  S->ZF = F & 0x40;
  S->HF = F & 0x10;
  S->PVF = F & 0x04;
  S->NF = F & 0x02;
  S->CF = F & 0x01;
}

static uint16_t addr(State *S, unsigned R, unsigned Disp) {
  switch (R) {
  case MIX: return ix(S) + int8_t(Disp);
  case MIY: return iy(S) + int8_t(Disp);
  default: return hl(S);
  }
}

static uint8_t get8(State *S, uint8_t *M, unsigned R, unsigned Disp) {
  switch (R) {
  case B: return S->B;
  case C: return S->C;
  case D: return S->D;
  case E: return S->E;
  case H: return S->H;
  case L: return S->L;
  case A: return S->A;
  case IXH: return S->IXH;
  case IXL: return S->IXL;
  case IYH: return S->IYH;
  case IYL: return S->IYL;
  default: return M[addr(S, R, Disp)];
  }
}

static void set8(State *S, uint8_t *M, unsigned R, unsigned Disp, uint8_t V) {
  switch (R) {
  case B: S->B = V; return;
  case C: S->C = V; return;
  case D: S->D = V; return;
  case E: S->E = V; return;
  case H: S->H = V; return;
  case L: S->L = V; return;
  case A: S->A = V; return;
  case IXH: S->IXH = V; return;
  case IXL: S->IXL = V; return;
  case IYH: S->IYH = V; return;
  case IYL: S->IYL = V; return;
  default: write8(S, M, addr(S, R, Disp), V); return;
  }
}

static uint16_t get16(State *S, unsigned R) {
  switch (R) {
  case BC: return pair(S->B, S->C);
  case DE: return pair(S->D, S->E);
  case HL: return hl(S);
  case SP: return S->SP;
  case AF: return pair(S->A, getF(S));
  case IX: return ix(S);
  default: return iy(S);
  }
}

static void set16(State *S, unsigned R, uint16_t V) {
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
  case AF:
    S->A = Hi;
    setF(S, Lo);
    return;
  case IX:
    S->IXH = Hi;
    S->IXL = Lo;
    return;
  default:
    S->IYH = Hi;
    S->IYL = Lo;
    return;
  }
}

static bool cond(State *S, unsigned CC) {
  switch (CC) {
  case 0: return !S->ZF;
  case 1: return S->ZF;
  case 2: return !S->CF;
  case 3: return S->CF;
  case 4: return !S->PVF;
  case 5: return S->PVF;
  case 6: return !S->SF;
  default: return S->SF;
  }
}

static void szp(State *S, uint8_t V) {
  S->SF = V & 0x80;
  S->ZF = V == 0;
  S->PVF = parity(V);
}

static void alu(State *S, unsigned Op, uint8_t V) {
  uint8_t X = S->A;
  switch (Op) {
  case ADD:
  case ADC: {
    unsigned Cin = Op == ADC && S->CF;
    unsigned R = X + V + Cin;
    S->HF = (X & 0xF) + (V & 0xF) + Cin > 0xF;
    S->PVF = (~(X ^ V) & (X ^ R) & 0x80) != 0;
    S->CF = R > 0xFF;
    S->NF = false;
    S->A = R;
    S->SF = S->A & 0x80;
    S->ZF = S->A == 0;
    return;
  }
  case SUB:
  case SBC:
  case CP: {
    int Cin = Op == SBC && S->CF;
    int R = X - V - Cin;
    S->HF = (X & 0xF) - (V & 0xF) - Cin < 0;
    S->PVF = ((X ^ V) & (X ^ R) & 0x80) != 0;
    S->CF = R < 0;
    S->NF = true;
    uint8_t Res = R;
    S->SF = Res & 0x80;
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
  szp(S, S->A);
  S->NF = false;
  S->CF = false;
}

static uint8_t rot(State *S, unsigned Op, uint8_t V) {
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
  case SLL:
    Out = V >> 7;
    R = V << 1 | 1;
    break;
  default:
    Out = V & 1;
    R = V >> 1;
    break;
  }
  S->CF = Out;
  S->HF = false;
  S->NF = false;
  szp(S, R);
  return R;
}

static uint8_t inc8(State *S, uint8_t V) {
  uint8_t R = V + 1;
  S->HF = (V & 0xF) == 0xF;
  S->PVF = V == 0x7F;
  S->NF = false;
  S->SF = R & 0x80;
  S->ZF = R == 0;
  return R;
}

static uint8_t dec8(State *S, uint8_t V) {
  uint8_t R = V - 1;
  S->HF = (V & 0xF) == 0;
  S->PVF = V == 0x80;
  S->NF = true;
  S->SF = R & 0x80;
  S->ZF = R == 0;
  return R;
}

static void ldx(State *S, uint8_t *M, int Dir) {
  uint16_t Src = hl(S), Dst = get16(S, DE), Count = get16(S, BC) - 1;
  write8(S, M, Dst, M[Src]);
  set16(S, HL, Src + Dir);
  set16(S, DE, Dst + Dir);
  set16(S, BC, Count);
  S->PVF = Count != 0;
  S->HF = false;
  S->NF = false;
}

static void cpx(State *S, uint8_t *M, int Dir) {
  uint16_t Src = hl(S), Count = get16(S, BC) - 1;
  uint8_t V = M[Src], R = S->A - V;
  S->HF = (S->A & 0xF) < (V & 0xF);
  S->SF = R & 0x80;
  S->ZF = R == 0;
  S->NF = true;
  set16(S, HL, Src + Dir);
  set16(S, BC, Count);
  S->PVF = Count != 0;
}

#define SEM(Name)                                                              \
  extern "C" void z80_##Name(State *S, uint8_t *M, unsigned a, unsigned b,     \
                             unsigned c)

SEM(NOP) {}
SEM(LD_R8_R8) { set8(S, M, a, c, get8(S, M, b, c)); }
SEM(LD_R8_N) { set8(S, M, a, c, b); }
SEM(LD_R16_NN) { set16(S, a, b); }
SEM(LD_R16_MNN) { set16(S, a, read16(M, b)); }
SEM(LD_MNN_R16) { write16(S, M, b, get16(S, a)); }
SEM(LD_A_MNN) { S->A = M[uint16_t(a)]; }
SEM(LD_MNN_A) { write8(S, M, a, S->A); }
SEM(LD_A_MR16) { S->A = M[get16(S, a)]; }
SEM(LD_MR16_A) { write8(S, M, get16(S, a), S->A); }
SEM(LD_SP_R16) { S->SP = get16(S, a); }

SEM(LD_A_I) {
  S->A = S->I;
  S->SF = S->A & 0x80;
  S->ZF = S->A == 0;
  S->HF = S->NF = false;
  S->PVF = S->IFF2;
}

SEM(LD_A_R) {
  S->A = S->R;
  S->SF = S->A & 0x80;
  S->ZF = S->A == 0;
  S->HF = S->NF = false;
  S->PVF = S->IFF2;
}

SEM(LD_I_A) { S->I = S->A; }
SEM(LD_R_A) { S->R = S->A; }
SEM(PUSH) { push16(S, M, get16(S, a)); }
SEM(POP) { set16(S, a, pop16(S, M)); }

SEM(EX_DE_HL) {
  uint16_t T = get16(S, DE);
  set16(S, DE, hl(S));
  set16(S, HL, T);
}

SEM(EX_AF) {
  uint8_t TA = S->A, TF = getF(S);
  S->A = S->A2;
  setF(S, S->F2);
  S->A2 = TA;
  S->F2 = TF;
}

SEM(EXX) {
  uint8_t T;
  T = S->B;
  S->B = S->B2;
  S->B2 = T;
  T = S->C;
  S->C = S->C2;
  S->C2 = T;
  T = S->D;
  S->D = S->D2;
  S->D2 = T;
  T = S->E;
  S->E = S->E2;
  S->E2 = T;
  T = S->H;
  S->H = S->H2;
  S->H2 = T;
  T = S->L;
  S->L = S->L2;
  S->L2 = T;
}

SEM(EX_MSP_R16) {
  uint16_t T = read16(M, S->SP);
  write16(S, M, S->SP, get16(S, a));
  set16(S, a, T);
}

SEM(ALU_R8) { alu(S, a, get8(S, M, b, c)); }
SEM(ALU_N) { alu(S, a, b); }
SEM(INC_R8) { set8(S, M, a, b, inc8(S, get8(S, M, a, b))); }
SEM(DEC_R8) { set8(S, M, a, b, dec8(S, get8(S, M, a, b))); }
SEM(INC_R16) { set16(S, a, get16(S, a) + 1); }
SEM(DEC_R16) { set16(S, a, get16(S, a) - 1); }

SEM(ADD_R16) {
  unsigned X = get16(S, a), Y = get16(S, b), R = X + Y;
  S->HF = (X & 0xFFF) + (Y & 0xFFF) > 0xFFF;
  S->CF = R > 0xFFFF;
  S->NF = false;
  set16(S, a, R);
}

SEM(ADC_HL) {
  unsigned X = hl(S), Y = get16(S, a), Cin = S->CF, R = X + Y + Cin;
  S->HF = (X & 0xFFF) + (Y & 0xFFF) + Cin > 0xFFF;
  S->PVF = (~(X ^ Y) & (X ^ R) & 0x8000) != 0;
  S->CF = R > 0xFFFF;
  S->NF = false;
  uint16_t Res = R;
  S->SF = Res & 0x8000;
  S->ZF = Res == 0;
  set16(S, HL, Res);
}

SEM(SBC_HL) {
  int X = hl(S), Y = get16(S, a), Cin = S->CF, R = X - Y - Cin;
  S->HF = (X & 0xFFF) - (Y & 0xFFF) - Cin < 0;
  S->PVF = ((X ^ Y) & (X ^ R) & 0x8000) != 0;
  S->CF = R < 0;
  S->NF = true;
  uint16_t Res = R;
  S->SF = Res & 0x8000;
  S->ZF = Res == 0;
  set16(S, HL, Res);
}

SEM(RLCA) {
  bool Out = S->A >> 7;
  S->A = S->A << 1 | Out;
  S->CF = Out;
  S->HF = S->NF = false;
}

SEM(RRCA) {
  bool Out = S->A & 1;
  S->A = S->A >> 1 | Out << 7;
  S->CF = Out;
  S->HF = S->NF = false;
}

SEM(RLA) {
  bool Out = S->A >> 7;
  S->A = S->A << 1 | S->CF;
  S->CF = Out;
  S->HF = S->NF = false;
}

SEM(RRA) {
  bool Out = S->A & 1;
  S->A = S->A >> 1 | S->CF << 7;
  S->CF = Out;
  S->HF = S->NF = false;
}

SEM(ROT) { set8(S, M, b, c, rot(S, a, get8(S, M, b, c))); }

SEM(BIT) {
  bool Zero = !(get8(S, M, b, c) >> a & 1);
  S->ZF = Zero;
  S->PVF = Zero;
  S->SF = a == 7 && !Zero;
  S->HF = true;
  S->NF = false;
}

SEM(RES) { set8(S, M, b, c, get8(S, M, b, c) & ~(1u << a)); }
SEM(SET) { set8(S, M, b, c, get8(S, M, b, c) | 1u << a); }

SEM(DAA) {
  uint8_t X = S->A, Fix = 0;
  bool Carry = S->CF;
  if (S->HF || (X & 0xF) > 9)
    Fix |= 0x06;
  if (S->CF || X > 0x99) {
    Fix |= 0x60;
    Carry = true;
  }
  if (S->NF) {
    S->HF = S->HF && (X & 0xF) < 6;
    S->A = X - Fix;
  } else {
    S->HF = (X & 0xF) > 9;
    S->A = X + Fix;
  }
  S->CF = Carry;
  szp(S, S->A);
}

SEM(CPL) {
  S->A = ~S->A;
  S->HF = S->NF = true;
}

SEM(NEG) {
  uint8_t V = S->A;
  S->A = 0;
  alu(S, SUB, V);
}

SEM(CCF) {
  S->HF = S->CF;
  S->CF = !S->CF;
  S->NF = false;
}

SEM(SCF) {
  S->CF = true;
  S->HF = S->NF = false;
}

SEM(RLD) {
  uint16_t Ad = hl(S);
  uint8_t T = M[Ad];
  write8(S, M, Ad, T << 4 | (S->A & 0xF));
  S->A = (S->A & 0xF0) | T >> 4;
  szp(S, S->A);
  S->HF = S->NF = false;
}

SEM(RRD) {
  uint16_t Ad = hl(S);
  uint8_t T = M[Ad];
  write8(S, M, Ad, S->A << 4 | T >> 4);
  S->A = (S->A & 0xF0) | (T & 0xF);
  szp(S, S->A);
  S->HF = S->NF = false;
}

SEM(LDI) { ldx(S, M, 1); }
SEM(LDD) { ldx(S, M, -1); }

// The repeating forms do one step and, to go on, move PC back onto
// themselves, as the CPU does.
SEM(LDIR) {
  ldx(S, M, 1);
  if (S->PVF)
    S->PC -= 2;
}

SEM(LDDR) {
  ldx(S, M, -1);
  if (S->PVF)
    S->PC -= 2;
}

SEM(CPI) { cpx(S, M, 1); }
SEM(CPD) { cpx(S, M, -1); }

SEM(CPIR) {
  cpx(S, M, 1);
  if (S->PVF && !S->ZF)
    S->PC -= 2;
}

SEM(CPDR) {
  cpx(S, M, -1);
  if (S->PVF && !S->ZF)
    S->PC -= 2;
}

SEM(DI) { S->IFF1 = S->IFF2 = false; }
SEM(EI) { S->IFF1 = S->IFF2 = true; }
SEM(IM) { S->IM = a; }

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
SEM(DJNZ) {
  if (--S->B)
    S->PC = a;
}
SEM(JP_R16) { S->PC = get16(S, a); }

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
  S->IFF1 = S->IFF2;
  S->PC = pop16(S, M);
}

SEM(RETN) {
  S->IFF1 = S->IFF2;
  S->PC = pop16(S, M);
}

SEM(HALT) { S->Halted = true; }
