// Helpers generated code calls so that it does not depend on State's layout.

#include "semantics/Declarations.h"

using namespace z80core;

extern "C" unsigned z80core_get_pc(State *S) { return S->PC; }

extern "C" void z80core_set_pc(State *S, unsigned PC) { S->PC = PC; }

extern "C" bool z80core_tick(State *S, unsigned N) {
  S->Steps += N;
  if (S->Steps > S->StepLimit && S->Fault == FaultNone)
    S->Fault = FaultStepLimit;
  return S->Fault != FaultNone;
}
