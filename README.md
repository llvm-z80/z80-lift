# z80-lift

Lifts Z80 and SM83 machine code to LLVM IR.

This makes it possible to recompile Z80 binaries for machines such as x86,
where they run 10 to 50 times faster than under z88dk-ticks. It is mainly
used to validate the [llvm-z80](https://github.com/llvm-z80/llvm-z80) runtime
library.

z80-lift takes the linked runtime image and turns each function into LLVM IR
with an explicit model of the CPU. That IR is then

- **tested**: JIT-compiled and checked against the function's contract.
- **proved** (planned): checked against the reference with
  [alive-tv](https://github.com/AliveToolkit/alive2).

## Building

It needs an upstream LLVM and a clang of the same version.

```sh
cmake -G Ninja -B build -DLLVM_DIR=<llvm>/lib/cmake/llvm
ninja -C build
```

For z80-tester to take the runtime's assembly directly, build against
llvm-z80, whose LLVM has the Z80 assembler:

```sh
cmake -G Ninja -B build -DLLVM_DIR=<llvm-z80-build>/lib/cmake/llvm \
    -DZ80LIFT_TESTER_ASSEMBLER=ON
```

## Usage

Every tool assumes a Z80; pass `--cpu=sm83` for SM83 programs.

### z80-lift

It works on a linked ELF; functions are named as in C.

```sh
# Disassemble the code reachable from a function, or from every function
z80-lift decode prog.elf main
z80-lift decode prog.elf

# Print the lifted IR, optimized or as lifted
z80-lift lift prog.elf main
z80-lift lift --raw prog.elf main
```

### z80-tester

It tests runtime functions against contracts written as `;@` comments in the
runtime's assembly.
[tester/docs/Contracts.md](tester/docs/Contracts.md) describes how to write
them.

Built with `Z80LIFT_TESTER_ASSEMBLER`, it takes the assembly itself. Given a
file, it tests the contracts in that file and links the other files of its
directory as needed.

```sh
z80-tester check <llvm-z80>/compiler-rt/lib/builtins/z80
z80-tester check --cpu=sm83 <llvm-z80>/compiler-rt/lib/builtins/sm83/divhi3.asm
```

Otherwise it takes the runtime linked into one image per CPU.

```sh
# Link the runtime of an llvm-z80 build into build/images/
scripts/import-runtime.sh <llvm-z80-build-dir>

# Test every function that has a contract, or some of them
z80-tester check build/images/z80-runtime.elf \
    --contracts <llvm-z80>/compiler-rt/lib/builtins/z80
z80-tester check --cpu=sm83 build/images/sm83-runtime.elf \
    --contracts <llvm-z80>/compiler-rt/lib/builtins/sm83 __divhi3 roundf

# Add contracts from a separate file
z80-tester check build/images/z80-runtime.elf \
    --contracts <llvm-z80>/compiler-rt/lib/builtins/z80 --contracts extra.contracts
```

## License

Licensed under either Apache-2.0 ([LICENSE-APACHE](LICENSE-APACHE)) or MIT
([LICENSE-MIT](LICENSE-MIT)), at your option.
