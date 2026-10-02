# Runs lifted runtime functions against reference implementations.

z80lift_add_bitcode(z80tester-refs-bc
  NAMESPACE z80tester
  FUNCTION refsBitcode
  SOURCES refs/Int.c refs/Float.c
  FLAGS -std=c11)

add_library(z80tester STATIC
  lib/CallConv.cpp
  lib/Jit.cpp
  lib/Spec.cpp
  lib/Tester.cpp)

z80lift_llvm_libs(llvm_libs Core BitReader OrcJIT native)
target_include_directories(z80tester PUBLIC include)
target_link_libraries(z80tester PUBLIC z80lift z80tester-refs-bc ${llvm_libs})

add_subdirectory(tools/z80-tester)
