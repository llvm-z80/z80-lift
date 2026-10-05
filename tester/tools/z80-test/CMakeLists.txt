add_executable(z80-test main.cpp)
target_link_libraries(z80-test PRIVATE z80tester)
target_compile_definitions(z80-test PRIVATE
  Z80TESTER_CLANG="${Z80LIFT_CLANG}")
install(TARGETS z80-test)
