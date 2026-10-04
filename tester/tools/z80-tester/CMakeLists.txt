add_executable(z80-tester main.cpp)
target_link_libraries(z80-tester PRIVATE z80tester)
target_compile_definitions(z80-tester PRIVATE
  Z80TESTER_CLANG="${Z80LIFT_CLANG}")
install(TARGETS z80-tester)
