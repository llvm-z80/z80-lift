// Where arguments and results live under the calling conventions the llvm-z80
// backend uses for runtime calls (see Z80CallLowering.cpp there).

#ifndef Z80TESTER_CALLCONV_H
#define Z80TESTER_CALLCONV_H

#include "z80core/Decoder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <optional>
#include <vector>

namespace llvm {
class Type;
}

namespace z80tester {

enum class Ty { I8, I16, I32, I64, I128, F16, F32, Ptr };

/// Bytes the type occupies on the target; pointers are 16-bit.
unsigned sizeOf(Ty T);
const char *tyName(Ty T);
std::optional<Ty> tyFromIR(llvm::Type *T);

enum Reg8 : uint8_t { RA, RB, RC, RD, RE, RH, RL };

/// A value in registers, most significant byte first, or in the stack
/// arguments at Offset bytes above the return address.
struct Loc {
  std::vector<Reg8> Regs;
  unsigned Offset = 0;
  unsigned Size = 0;
};

struct CallLayout {
  std::vector<Loc> Params;
  std::optional<Loc> Ret; // in registers, unless SRet
  bool SRet = false;      // result written through the first stack argument
  unsigned StackBytes = 0;
  unsigned Popped = 0; // stack argument bytes the callee removes
};

enum class CallConv {
  SDCCCall1, // __sdcccall(1), what C runtime calls use
  Builtin,   // Z80_Builtin: register-only helpers like __z80_memcpy_builtin
};

llvm::Expected<CallLayout> layoutCall(z80core::Cpu C, CallConv CC,
                                      llvm::ArrayRef<Ty> Params,
                                      std::optional<Ty> Ret);

} // namespace z80tester

#endif // Z80TESTER_CALLCONV_H
