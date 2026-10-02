# Control-flow recovery and lifting to LLVM IR.

add_library(z80lift STATIC
  lib/CFG.cpp
  lib/Lifter.cpp)

z80lift_llvm_libs(llvm_libs Core BitReader Passes)
target_include_directories(z80lift PUBLIC include)
target_link_libraries(z80lift PUBLIC z80core ${llvm_libs})

add_subdirectory(tools/z80-lift)
