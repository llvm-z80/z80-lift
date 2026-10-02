// Names of the semantics functions in the bitcode.

#include "z80core/Semantics.h"

std::string z80core::semanticsFunction(Cpu C, unsigned Op) {
  return std::string(cpuName(C)) + "_" + instName(C, Op);
}
