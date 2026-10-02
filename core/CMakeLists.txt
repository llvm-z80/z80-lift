# The machine model: decoding, the instruction semantics and an interpreter.

set(semantics
  semantics/Common.cpp
  Z80/Semantics.cpp
  SM83/Semantics.cpp)

# The semantics are built twice: natively for the interpreter, and as bitcode
# for code generators.
z80lift_add_bitcode(z80core-semantics-bc
  NAMESPACE z80core
  FUNCTION semanticsBitcode
  SOURCES ${semantics}
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}
  FLAGS -std=c++17 -fno-exceptions -fno-rtti)

add_library(z80core STATIC
  lib/Decoder.cpp
  lib/Image.cpp
  lib/Interp.cpp
  lib/Semantics.cpp
  Z80/Decoder.cpp
  SM83/Decoder.cpp
  ${semantics})

z80lift_llvm_libs(llvm_libs Support Object)
target_include_directories(z80core
  PUBLIC include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(z80core PUBLIC z80core-semantics-bc ${llvm_libs})
