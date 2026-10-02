// The parts of the decoder shared by both CPUs.

#include "Insns.h"

using namespace z80core;

namespace {

const Kind Z80Kinds[] = {
#define INST(Name, K) Kind::K,
#include "Z80/Insns.def"
};

const Kind SM83Kinds[] = {
#define INST(Name, K) Kind::K,
#include "SM83/Insns.def"
};

const char *const Z80Names[] = {
#define INST(Name, K) #Name,
#include "Z80/Insns.def"
};

const char *const SM83Names[] = {
#define INST(Name, K) #Name,
#include "SM83/Insns.def"
};

} // namespace

const char *z80core::cpuName(Cpu C) { return C == Cpu::Z80 ? "z80" : "sm83"; }

bool z80core::decode(Cpu C, const uint8_t *Mem, uint16_t Addr, Inst &I,
                     std::string *Text) {
  return C == Cpu::Z80 ? decodeZ80(Mem, Addr, I, Text)
                       : decodeSM83(Mem, Addr, I, Text);
}

unsigned z80core::numOps(Cpu C) {
  return C == Cpu::Z80 ? unsigned(z80::NumOps) : unsigned(sm83::NumOps);
}

Kind z80core::instKind(Cpu C, unsigned Op) {
  return C == Cpu::Z80 ? Z80Kinds[Op] : SM83Kinds[Op];
}

const char *z80core::instName(Cpu C, unsigned Op) {
  return C == Cpu::Z80 ? Z80Names[Op] : SM83Names[Op];
}
