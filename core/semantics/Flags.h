// Memory and flag helpers shared by the Z80 and SM83 semantics.

#ifndef Z80CORE_SEMANTICS_FLAGS_H
#define Z80CORE_SEMANTICS_FLAGS_H

#include "z80core/State.h"

namespace z80core::sem {

inline bool parity(uint8_t V) { return !(__builtin_popcount(V) & 1); }

inline uint16_t pair(uint8_t Hi, uint8_t Lo) { return Hi << 8 | Lo; }

inline void write8(State *S, uint8_t *M, uint16_t Addr, uint8_t V) {
  if (Addr < S->WriteLo || Addr >= S->WriteHi) {
    if (S->Fault == FaultNone) {
      S->Fault = FaultBadWrite;
      S->FaultAddr = Addr;
    }
    return;
  }
  M[Addr] = V;
}

inline uint16_t read16(const uint8_t *M, uint16_t Addr) {
  return pair(M[uint16_t(Addr + 1)], M[Addr]);
}

inline void write16(State *S, uint8_t *M, uint16_t Addr, uint16_t V) {
  write8(S, M, Addr, V);
  write8(S, M, Addr + 1, V >> 8);
}

inline void push16(State *S, uint8_t *M, uint16_t V) {
  S->SP -= 2;
  write16(S, M, S->SP, V);
}

inline uint16_t pop16(State *S, const uint8_t *M) {
  uint16_t V = read16(M, S->SP);
  S->SP += 2;
  return V;
}

} // namespace z80core::sem

#endif // Z80CORE_SEMANTICS_FLAGS_H
