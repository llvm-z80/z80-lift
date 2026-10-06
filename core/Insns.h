// The instruction lists as enums, private to core.

#ifndef Z80CORE_INSNS_H
#define Z80CORE_INSNS_H

#include "z80core/Decoder.h"

namespace z80core {

namespace z80 {
enum Op : unsigned {
#define INST(Name, Kind) Name,
#include "Z80/Insns.def"
  NumOps
};
} // namespace z80

namespace sm83 {
enum Op : unsigned {
#define INST(Name, Kind) Name,
#include "SM83/Insns.def"
  NumOps
};
} // namespace sm83

/// A decoded argument and the bytes it was read from, if any.
struct Arg {
  unsigned V = 0;
  Field F;
  Arg(unsigned V = 0) : V(V) {}
  Arg(unsigned V, Field F) : V(V), F(F) {}
};

bool decodeZ80(const uint8_t *Mem, uint16_t Addr, Inst &I);
bool decodeSM83(const uint8_t *Mem, uint16_t Addr, Inst &I);

} // namespace z80core

#endif // Z80CORE_INSNS_H
