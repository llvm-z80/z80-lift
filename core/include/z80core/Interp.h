// Runs the native semantics one instruction at a time.

#ifndef Z80CORE_INTERP_H
#define Z80CORE_INTERP_H

#include "z80core/Decoder.h"
#include "z80core/State.h"

namespace z80core {

/// Executes the instruction at S.PC. Returns false if it does not decode.
bool step(Cpu T, State &S, uint8_t *M);

/// Steps until PC reaches Stop, the CPU halts or a fault is raised. Returns
/// false if an instruction does not decode.
bool run(Cpu T, State &S, uint8_t *M, uint16_t Stop);

} // namespace z80core

#endif // Z80CORE_INTERP_H
