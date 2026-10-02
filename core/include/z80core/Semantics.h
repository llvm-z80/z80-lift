// The instruction semantics as LLVM bitcode, for code generators such as the
// lifter. Each instruction is a function
//
//   void <cpu>_<Name>(State *S, uint8_t *Mem, unsigned A, unsigned B,
//                     unsigned C)
//
// that takes the decoded Inst::Args. The caller sets PC to the next
// instruction first; transfers of control overwrite it.

#ifndef Z80CORE_SEMANTICS_H
#define Z80CORE_SEMANTICS_H

#include "z80core/Decoder.h"

#include "llvm/ADT/StringRef.h"

#include <string>

namespace z80core {

llvm::StringRef semanticsBitcode();

/// The bitcode function implementing Op, such as z80_LD_R8_R8.
std::string semanticsFunction(Cpu C, unsigned Op);

/// Helpers in the bitcode, so that generated code need not know State's
/// layout: `unsigned get_pc(State *)`, `void set_pc(State *, unsigned)` and
/// `bool tick(State *, unsigned N)`, which counts N instructions and returns
/// whether execution has to stop.
inline constexpr const char *GetPCFunction = "z80core_get_pc";
inline constexpr const char *SetPCFunction = "z80core_set_pc";
inline constexpr const char *TickFunction = "z80core_tick";

} // namespace z80core

#endif // Z80CORE_SEMANTICS_H
