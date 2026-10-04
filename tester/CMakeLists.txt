# Runs lifted runtime functions against the contracts in their assembly.

option(Z80LIFT_TESTER_ASSEMBLER
  "Let z80-tester assemble the runtime's sources; needs LLVM's Z80 target" OFF)

set(sources
  lib/CallConv.cpp
  lib/Contract.cpp
  lib/Jit.cpp
  lib/Tester.cpp)
set(components Core BitReader OrcJIT native)
if(Z80LIFT_TESTER_ASSEMBLER)
  if(NOT "Z80" IN_LIST LLVM_TARGETS_TO_BUILD)
    message(FATAL_ERROR "Z80LIFT_TESTER_ASSEMBLER needs an LLVM built with "
      "the Z80 target, such as llvm-z80's; ${LLVM_DIR} has "
      "${LLVM_TARGETS_TO_BUILD}")
  endif()
  list(APPEND sources lib/Assembler.cpp)
  list(APPEND components MC MCParser Object Z80AsmParser Z80Desc Z80Info)
endif()

add_library(z80tester STATIC ${sources})
z80lift_llvm_libs(llvm_libs ${components})
target_include_directories(z80tester PUBLIC include)
target_link_libraries(z80tester PUBLIC z80lift ${llvm_libs})
if(Z80LIFT_TESTER_ASSEMBLER)
  target_compile_definitions(z80tester PUBLIC Z80TESTER_ASSEMBLER)
endif()

add_subdirectory(tools/z80-tester)
