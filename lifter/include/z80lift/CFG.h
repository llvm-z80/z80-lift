// Control flow of one runtime function.

#ifndef Z80LIFT_CFG_H
#define Z80LIFT_CFG_H

#include "z80core/Decoder.h"
#include "z80core/Image.h"

#include "llvm/Support/Error.h"

#include <map>
#include <set>
#include <vector>

namespace z80lift {

struct Block {
  uint16_t Start = 0;
  std::vector<z80core::Inst> Insts;
};

struct CFG {
  uint16_t Entry = 0;
  std::map<uint16_t, Block> Blocks;

  /// Functions this one calls or continues into: call targets, and jumps or
  /// fall-through to another entry, which are tail calls.
  std::set<uint16_t> Callees;
};

/// Follows every path from Entry. Control reaching another address in
/// Img.Entries leaves the function as a tail call. With OutsideLeaves, so
/// does control reaching an address past the image.
llvm::Expected<CFG> recoverCFG(z80core::Cpu C, const z80core::Image &Img,
                               uint16_t Entry, bool OutsideLeaves = false);

} // namespace z80lift

#endif // Z80LIFT_CFG_H
