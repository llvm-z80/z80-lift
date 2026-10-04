add_executable(z80-lift main.cpp)
target_link_libraries(z80-lift PRIVATE z80lift)
install(TARGETS z80-lift)
