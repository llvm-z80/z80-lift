# Rules

z80-test proves rewrite rules, such as a compiler's peepholes, with
[Alive2](https://github.com/AliveToolkit/alive2): the code after the rewrite
must leave what the rule keeps, and memory, as the code before it does, for
any registers, flags and memory, and any values of the rule's constants.
Given a `.rules` file, it proves every rule in it, or the rules named after
it.

```sh
z80-test peepholes.rules
z80-test --cpu=sm83 peepholes.rules zero-a
```

## Syntax

```
// Loading zero into A, when the flags do not matter.
rule zero-a
    before:
        ld      a, #0
    after:
        xor     a
    keep: A
```

- `rule <name>` starts a rule at the beginning of a line. Each rule needs a
  different name.
- `before:` and `after:` are followed by indented lines of assembly, in the
  syntax of the runtime's sources. A line may also follow the keyword. The
  code after can be empty.
- `keep:` lists what both sides must leave the same: `A` to `L`, `BC`, `DE`,
  `HL`, `IX`, `IY`, `SP`, `F`, or one of the flags `SF`, `ZF`, `HF`, `PVF`,
  `NF` and `CF`. The SM83 has no `IX`, `IY`, `SF` or `PVF`. Registers and
  flags left out may differ.
- `memory: above SP` compares only the memory at and above SP after the code,
  and leaves out the 32 KiB below it, which interrupts may overwrite. The rule
  must keep `SP`. The default is `memory: all`.
- `//` begins a comment that ends at the end of the line.

All rules of a file are assembled together, so a label can be defined only
once in the file.

## Constants

A rule can hold for every value of a constant instead of one. `const:`
declares a constant, which the code names like a symbol, and the proof covers
every value in its range:

```
// Two additions of constants folded into one load.
rule fold-add
    const: k
    const: j
    const: m
    assume: k <= 0xff && j <= 0xff && m == ((k + j) & 0xff)
    before:
        ld      a, #k
        add     a, #j
    after:
        ld      a, #m
    keep: A
```

- `const: <name>` declares a constant from 0 to 0xffff, and
  `const: <name> <min>..<max>` one with another range, such as `-128..127`
  for an index offset.
- `assume: <expression>` limits the constants: the rule only has to hold where
  the expression is not 0. Expressions use C's operators and precedence on
  integers, and dividing by 0 gives 0. A rule may have several `assume:`
  lines.
- An instruction reads the low bits of a constant, as a linker would write
  them. An operand can add a number to a constant, as in `(k + 1)`, but not
  another constant: give the sum a constant of its own and an `assume:` that
  says what it equals.
- A constant cannot be a jump target or part of an opcode, such as the bit
  number of `bit`. Its name cannot be a register's or a label's.

## What must match

Besides what `keep` lists, both sides must leave every byte of memory the
same, unless the rule says `memory: above SP`, and must end the same way:
either both run to the end of their code, or both leave it for the same
address, as `ret` or `jp (hl)` does.

A rule that pushes and pops, for example, needs `memory: above SP`, since the
code after it does not write the bytes below SP:

```
rule push-pop
    before:
        push    hl
        pop     de
    after:
        ld      d, h
        ld      e, l
    keep: DE HL SP
    memory: above SP
```

## Counterexamples

A rule that does not hold shows a state where the two sides differ:

```
zero-a-flags      FAIL  counterexample
  Alive2: Value mismatch
  from    A=0x03 B=0x03 C=0x03 D=0x03 E=0x03 H=0x03 L=0x03 F=0x00 IX=0x0303 IY=0x0303 SP=0x0003
          every byte 0x03
  before  A=0x00 F=0x00
  after   A=0x00 F=0x44
```

`from` gives the registers and memory that the code starts with: the bytes it
lists, and the value of all other bytes. For a rule with constants, `const`
gives their values. `before` and `after` give what each
side leaves of what the rule keeps, and the byte of memory where they differ,
if they do.

## Limits

The code cannot have a loop, unless the loop always runs the same number of
times, such as `djnz` after loading B with a constant.
