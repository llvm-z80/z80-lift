// Declarations of the semantics functions in Z80/ and SM83/, which the
// interpreter calls natively.

#ifndef Z80CORE_SEMANTICS_DECLARATIONS_H
#define Z80CORE_SEMANTICS_DECLARATIONS_H

#include "z80core/State.h"

namespace z80core {

using SemFn = void (*)(State *S, uint8_t *M, unsigned A, unsigned B,
                       unsigned C);

} // namespace z80core

extern "C" {
#define INST(Name, Kind)                                                       \
  void z80_##Name(z80core::State *, uint8_t *, unsigned, unsigned, unsigned);
#include "Z80/Insns.def"
#define INST(Name, Kind)                                                       \
  void sm83_##Name(z80core::State *, uint8_t *, unsigned, unsigned, unsigned);
#include "SM83/Insns.def"

unsigned z80core_get_pc(z80core::State *S);
void z80core_set_pc(z80core::State *S, unsigned PC);
bool z80core_tick(z80core::State *S, unsigned N);
}

#endif // Z80CORE_SEMANTICS_DECLARATIONS_H
