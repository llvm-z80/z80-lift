# z80-lift

Lifts Z80 and SM83 machine code to LLVM IR.

This makes it possible to recompile Z80 binaries for machines such as x86,
where they run 10 to 50 times faster than under z88dk-ticks. It is mainly
used to validate the [llvm-z80](https://github.com/llvm-z80/llvm-z80) runtime
library.

z80-lift takes the linked runtime image and turns each function into LLVM IR
with an explicit model of the CPU. That IR is then

- **tested**: JIT-compiled and checked against the function's contract.
- **proved**: checked against the contract for every input with
  [Alive2](https://github.com/AliveToolkit/alive2).

## Building

Alive2 needs Z3 4.8.5 or later and re2c.

```sh
git submodule update --init
cmake -G Ninja -B build
ninja -C build
```

The first build also builds LLVM and clang from the `llvm-z80` submodule. To
use an LLVM that is already built, with its clang, add
`-DLLVM_DIR=<llvm>/lib/cmake/llvm`.

## Usage

Every tool assumes a Z80; pass `--cpu=sm83` for SM83 programs.

### z80-lift

It works on a linked ELF; functions are named as in C.

```sh
# Print the lifted IR, optimized or as lifted
z80-lift prog.elf main
z80-lift --raw prog.elf main
```

### z80-test

It tests or proves runtime functions against contracts written as `;@`
comments in the runtime's assembly; see
[tester/docs/Contracts.md](tester/docs/Contracts.md). Given a file, it checks
the contracts in that file; given a directory, every contract in it.

```sh
z80-test <llvm-z80>/compiler-rt/lib/builtins/z80
z80-test --cpu=sm83 <llvm-z80>/compiler-rt/lib/builtins/sm83/divhi3.asm
```

It also takes the runtime linked into one image per CPU.

```sh
# Link the runtime of an llvm-z80 build into build/images/
scripts/import-runtime.sh <llvm-z80-build-dir>

# Test every function that has a contract, or some of them
z80-test build/images/z80-runtime.elf \
    --contracts <llvm-z80>/compiler-rt/lib/builtins/z80
z80-test --cpu=sm83 build/images/sm83-runtime.elf \
    --contracts <llvm-z80>/compiler-rt/lib/builtins/sm83 __divhi3 roundf

# Add contracts from a separate file
z80-test build/images/z80-runtime.elf \
    --contracts <llvm-z80>/compiler-rt/lib/builtins/z80 --contracts extra.contracts
```

## License

Licensed under either Apache-2.0 ([LICENSE-APACHE](LICENSE-APACHE)) or MIT
([LICENSE-MIT](LICENSE-MIT)), at your option.
