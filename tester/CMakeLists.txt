# Runs lifted runtime functions against the contracts in their assembly.

if("Z80" IN_LIST LLVM_TARGETS_TO_BUILD)
  set(assembler ON)
else()
  set(assembler OFF)
endif()
option(Z80LIFT_TESTER_ASSEMBLER
  "Let z80-test assemble the runtime's sources; needs LLVM's Z80 target"
  ${assembler})

if(LLVM_ENABLE_RTTI AND LLVM_ENABLE_EH
   AND EXISTS ${PROJECT_SOURCE_DIR}/alive2/CMakeLists.txt)
  set(alive2 ON)
else()
  set(alive2 OFF)
endif()
option(Z80LIFT_TESTER_ALIVE2
  "Let z80-test prove contracts with Alive2; needs LLVM with RTTI and \
exceptions" ${alive2})

set(sources
  lib/CallConv.cpp
  lib/Contract.cpp
  lib/Jit.cpp
  lib/Rules.cpp
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

if(Z80LIFT_TESTER_ALIVE2)
  if(NOT (LLVM_ENABLE_RTTI AND LLVM_ENABLE_EH))
    message(FATAL_ERROR "Z80LIFT_TESTER_ALIVE2 needs an LLVM built with RTTI "
      "and exceptions; ${LLVM_DIR} is not")
  endif()
  # Only Alive2's libraries, not its tools.
  set(BUILD_TV OFF)
  set(BUILD_LLVM_UTILS ON)
  add_subdirectory(${PROJECT_SOURCE_DIR}/alive2 ${CMAKE_BINARY_DIR}/alive2
                   EXCLUDE_FROM_ALL)
  find_library(Z80LIFT_Z3 z3 REQUIRED)
  target_sources(z80tester PRIVATE lib/Prover.cpp)
  # Alive2's headers are C++20.
  set_target_properties(z80tester PROPERTIES CXX_STANDARD 20)
  target_include_directories(z80tester PRIVATE ${PROJECT_SOURCE_DIR}/alive2
                             ${CMAKE_BINARY_DIR}/alive2)
  z80lift_llvm_libs(alive2_llvm_libs Analysis BitReader Linker Passes
                    TransformUtils)
  target_link_libraries(z80tester PUBLIC llvm_util tools ir smt util
                        ${Z80LIFT_Z3} ${alive2_llvm_libs})
  target_compile_definitions(z80tester PUBLIC Z80TESTER_ALIVE2)
endif()

add_subdirectory(tools/z80-test)
