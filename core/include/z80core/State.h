// CPU state shared by the semantics, the interpreter and the lifted IR.

#ifndef Z80CORE_STATE_H
#define Z80CORE_STATE_H

#include <stdint.h>

namespace z80core {

enum Fault : uint8_t {
  FaultNone = 0,
  FaultStepLimit, // ran more instructions than StepLimit
  FaultBadWrite,  // wrote outside [WriteLo, WriteHi); the write is dropped
};

/// Flags are separate fields so that unused ones fold away in the lifted IR.
/// Bits 3 and 5 of the Z80 F register are not modelled and read as zero.
struct State {
  uint8_t A, B, C, D, E, H, L;
  uint8_t IXH, IXL, IYH, IYL;
  uint8_t I, R;
  uint16_t SP, PC;

  // SM83 has only Z, N, H and C.
  bool SF, ZF, HF, PVF, NF, CF;

  // Z80 alternate registers, with the flags packed into F2.
  uint8_t A2, F2, B2, C2, D2, E2, H2, L2;

  bool IFF1, IFF2; // Z80 interrupt flip-flops; IFF1 is IME on SM83
  uint8_t IM;
  bool Halted;

  // Bookkeeping for the tester.
  uint64_t Steps, StepLimit;
  uint32_t WriteLo, WriteHi;
  uint8_t Fault;
  uint16_t FaultAddr;
};

} // namespace z80core

#endif // Z80CORE_STATE_H
