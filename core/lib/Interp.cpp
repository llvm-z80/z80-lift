// Runs the native semantics one instruction at a time.

#include "z80core/Interp.h"
#include "semantics/Declarations.h"

using namespace z80core;

namespace {

const SemFn Z80Fns[] = {
#define INST(Name, Kind) z80_##Name,
#include "Z80/Insns.def"
};

const SemFn SM83Fns[] = {
#define INST(Name, Kind) sm83_##Name,
#include "SM83/Insns.def"
};

} // namespace

bool z80core::step(Cpu T, State &S, uint8_t *M) {
  Inst I;
  if (!decode(T, M, S.PC, I))
    return false;
  S.PC = I.next();
  const SemFn *Fns = T == Cpu::Z80 ? Z80Fns : SM83Fns;
  Fns[I.Op](&S, M, I.Args[0], I.Args[1], I.Args[2]);
  z80core_tick(&S, 1);
  return true;
}

bool z80core::run(Cpu T, State &S, uint8_t *M, uint16_t Stop) {
  while (S.PC != Stop && !S.Halted && S.Fault == FaultNone)
    if (!step(T, S, M))
      return false;
  return true;
}
