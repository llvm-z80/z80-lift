// Decodes Z80 and SM83 instructions. The instruction lists are Z80/Insns.def
// and SM83/Insns.def; outside core an instruction is an opaque Op number.

#ifndef Z80CORE_DECODER_H
#define Z80CORE_DECODER_H

#include <cstdint>

namespace z80core {

enum class Cpu { Z80, SM83 };

const char *cpuName(Cpu C);

/// How an instruction leaves; drives control-flow recovery.
enum class Kind {
  Seq,
  Jump,
  CondJump,
  Call,
  CondCall,
  Ret,
  CondRet,
  JumpInd,
  Halt,
};

/// Where an argument was read from: Size bytes, little-endian, at offset Off
/// in the instruction. Size 0 means the opcode gave it.
struct Field {
  uint8_t Off = 0;
  uint8_t Size = 0;
};

struct Inst {
  uint16_t Addr = 0;
  uint8_t Len = 0;
  unsigned Op = 0;
  unsigned Args[3] = {0, 0, 0};
  Field Fields[3];
  Kind K = Kind::Seq;
  uint16_t Dest = 0; // target of a direct jump or call

  uint16_t next() const { return Addr + Len; }
};

/// Decodes the instruction at Addr. Fails on bytes the CPU lacks and on
/// instructions core does not model, such as I/O.
bool decode(Cpu C, const uint8_t *Mem, uint16_t Addr, Inst &I);

unsigned numOps(Cpu C);
Kind instKind(Cpu C, unsigned Op);
const char *instName(Cpu C, unsigned Op);

} // namespace z80core

#endif // Z80CORE_DECODER_H
