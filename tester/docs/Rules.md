# Rules

z80-test proves rewrite rules, such as a compiler's peepholes, with
[Alive2](https://github.com/AliveToolkit/alive2): the code after the rewrite
must leave the CPU state, memory and the way it ends as the code before it
does, for any state it starts from, every register the rule is written over
and every value of its numbers. Given a `.rules` file, it proves every rule in
it, or the rules named after it.

```sh
z80-test peepholes.rules
z80-test --cpu=sm83 peepholes.rules zero-a
```

## Syntax

```
// A register loaded with a constant and copied: load the constant into the
// copy instead, when the first register is dead.
rule load-copy
    for: x in reg8; y in reg8 | (hl); n in u8
    if: !overlaps(x, y)
    before:
        ld      x, #n
        ld      y, x
    after:
        ld      y, #n
    dead: x
```

- `rule <name>` starts a rule at the beginning of a line. Each rule needs a
  different name. The clauses after it are indented.
- `cpu: z80 sm83` gives the CPUs the rule must hold on. Without it, the rule
  is proved on the CPU of `--cpu`.
- `for:` gives the variables and their types: names of the same type are
  separated by `,`, and groups by `;`. A rule may have several `for:` lines.
- `if:` gives a condition the rule applies under. A rule may have several
  `if:` lines, which must all hold.
- `before:` and `after:` are followed by lines of assembly, in the syntax of
  the runtime's sources. A line may also follow the keyword. The code after
  can be empty.
- `dead:` lists what may differ afterwards: fields of the state, variables
  that are registers, and `below SP`.
- `//` begins a comment that ends at the end of the line.

## Variables

A variable of an operand type takes each of its operands in turn, and the
rule is proved for each, as a case. A variable of a number type is a symbol
in the code, and one proof covers every value it can have.

| Type | Values |
| --- | --- |
| `reg8` | `a b c d e h l` |
| `reg16` | `bc de hl` |
| `pair` | `bc de hl sp` |
| `idx` | `ix iy`, none on the SM83 |
| `cond` | `z nz c nc`, and `po pe p m` on the Z80 |
| `bit` | `0` to `7` |
| `u8`, `s8`, `u16`, `s16` | numbers of 8 or 16 bits, unsigned or signed |
| `<min>..<max>` | numbers from min to max |

Operand types and operands combine with `|`, as in `reg8 | (hl) | #n`; an
operand may use a number variable, as `#n` and `n (ix)` do. A number type
cannot combine with operands. A variable's name cannot be a register's, a
condition's or a field's of the state.

A number must fit each field it is written to, as a linker checks: from -128
to 255 for a byte, from -32768 to 65535 for a word, from -128 to 127 for an
index displacement, and from 0xff00 to 0xffff for the address of the SM83's
`ldh`. A case only covers the numbers that fit. An operand can add a number
to a number variable, as in `(n + 1)`, but not another variable: give the sum
a variable of its own and an `if:` that says what it equals. A number cannot
be a jump target or part of an opcode, such as the bit number of `bit`: use
an operand variable for those.

## Conditions

A condition is an expression with C's operators and precedence. It can name:

- the variables; an operand that is a number, such as a bit number, is that
  number;
- the fields of the state the code starts from, in capitals, such as `HL` or
  `SP`, as unsigned numbers;
- registers and conditions as operands, such as `a` or `nz`, and other
  operands in quotes, such as `'(hl)'`;
- `overlaps(x, y)`, whether two operands use a register in common, counting
  the registers that address memory, so that `h` overlaps `(hl)`;
- `isreg(x)`, whether an operand is a register.

Two operands are equal when their text is, with the numbers in them equal.
The parts of a condition that only depend on operand variables decide which
cases the rule has, and are checked as soon as those variables are chosen;
the rest is assumed of the numbers and the starting state.
Arithmetic is on 64-bit integers and does not wrap at 16 bits: write
`((SP - HL) & 0xffff) > 2` for an address more than two bytes below SP.
Dividing by 0 gives 0.

## What must match

Both sides must leave every field of the state the same, except those that
`dead:` lists: `A` to `L`, `IXH`, `IXL`, `IYH`, `IYL`, `I`, `R`, `SP`, the
flags `SF`, `ZF`, `HF`, `PVF`, `NF`, `CF`, the alternate registers `A2`,
`F2`, `B2` to `L2`, `IFF1`, `IFF2` and `IM`. The SM83 has `A` to `L`, `SP`,
`ZF`, `HF`, `NF`, `CF` and `IFF1`. `dead:` can also name `BC`, `DE`, `HL`,
`IX`, `IY`, `F` for all the flags and `AF`, and a variable, for the register
it is in each case. A case where a variable in `dead:` is not a register is
left out.

Both sides must also leave every byte of memory the same, unless `dead:`
lists `below SP`, which leaves out the 32 KiB below SP, where interrupts may
write; SP must then not differ. And both must end the same way: either both
run to the end of their code, or both leave it for the same address, as
`ret` or `jp (hl)` does.

## Labels

Each side has its own labels, so both can use the same names. A jump to a
label that a side does not define leaves the code, for an address of its
own, the same on both sides. A call to such a label returns at once.

## Results

z80-test assembles each case on its own. Cases whose code does not assemble,
such as `ld (hl), (hl)`, are left out, as soon as a line whose variables are
chosen does not; a rule none of whose cases assembles fails. For a file with
the rule above and this one:

```
rule ld-twice
    for: x, y in reg8 | (hl); n in u8
    if: x != y
    before:
        ld      x, #n
        ld      y, #n
    after:
        ld      y, #n
        ld      x, y
```

z80-test shows:

```
$ z80-test --reports=1 example.rules
load-copy         ok    proved 47 cases in 1.0 s
ld-twice          FAIL  counterexample in 4 of 56 cases
  case    x=h y=(hl)
  Alive2: Value mismatch
  from    A=0x03 B=0x03 C=0x03 D=0x03 E=0x03 H=0x00 L=0x00 F=0xd7 IX=0x0303 IY=0x0303 SP=0x0003
          (0x0000)=0x00 others 0x00
  number  n=0x0001
  before  (0x0000)=0x00
  after   (0x0000)=0x01
```

For a rule that fails, `case` gives the operands of a case that does, `from`
the registers and memory it starts with (the bytes listed, and the value of
all others), `number` the values of the numbers, and `before` and `after`
what the two sides leave differently. `--reports` sets how many failing
cases are shown, 10 by default, and `--max-failures` stops a rule after that
many, which makes the count of failing cases a lower bound. A case whose code
cannot be lifted, such as one with I/O or a loop, is counted as not proved,
with the reason.

## Limits

The code cannot have a loop, unless the loop always runs the same number of
times, such as `djnz` after loading B with a constant. A loop whose two sides
have the same shape can be proved one turn at a time, with the jump back
made a jump to a label outside the rule.

z80-lift does not model bits 3 and 5 of the Z80's F, which read as zero.
