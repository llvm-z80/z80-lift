# Contracts

z80-tester tests each runtime function against a contract written as `;@`
comments in the runtime's assembly.

```asm
;@ uint16_t __udivmodhi4(uint16_t a, uint16_t b)
;@     requires
;@         b != 0;
;@     ensures
;@         result == a / b;
;@         HL == a % b;
;@     tests
;@         example a = 0x8000, b = 0xFFFF;
___udivhi3:
___udivmodhi4:
	...
```

## Files

In an assembly file (`.asm` or `.s`), a contract is a block of comment lines
starting with `;@`, usually placed above the function. The first line without
`;@` ends it.

Other files contain the same lines without `;@`:

```
// Extra contracts for the Z80 runtime.

uint16_t __udivmodhi4(uint16_t a, uint16_t b)
    requires
        b != 0;
    ensures
        result == a / b;
        HL == a % b;
```

When z80-tester is given assembly, it reads the contracts in it. `--contracts`
adds more and accepts files and directories. For a directory, every `*.asm`
file in it is read. The option can be given more than once. A function can
have only one contract.

## Syntax

- The prototype starts at the beginning of the line (after `;@ ` in assembly).
  The indented lines below it belong to the same contract.
- `requires`, `ensures` and `tests` begin a section. Each item ends with `;`
  and may span several lines. An item may follow the keyword on the same line:
  `requires b != 0;`.
- `//` begins a comment that ends at the end of the line.

## Prototype

The prototype is a C declaration with the function's C name, which is the
assembler name without its leading underscore: `___udivhi3` is `__udivhi3`,
and `_roundf` is `roundf`.

Unless the prototype places its values (see [Calling convention](#calling-convention)),
the calling convention, `__sdcccall(1)`, is derived from the parameter and
return types.

Only types with the same size on the Z80 and on the host are accepted:

- `uint8_t` to `uint64_t`, `int8_t` to `int64_t`
- `__int128`, `unsigned __int128`
- `float`, `_Float16`
- pointers to these

`char`, `short`, `int`, `long`, `double`, `size_t` and `bool` are rejected.

A pointer parameter points to a 32-byte buffer filled with random bytes. A
parameter cannot be named after a register or `result`.

## Calling convention

A function outside `__sdcccall(1)` gives the place of every value in its
prototype:

```
;@ uint32_t mul32(uint32_t a __reg(DEHL), uint32_t b __stack(0)) __reg(DEHL) __pops(4)
```

- `__reg(R)` after a parameter, or after the parameter list for the result,
  names its registers, most significant first: `A` to `L`, `BC`, `DE` and
  `HL`, as in `A`, `HL` or `DEHL`. They must hold exactly the value's size.
- `__stack(N)` passes a parameter in the stack arguments, N bytes above the
  return address.
- `__pops(N)` after the parameter list means the function removes N bytes of
  stack arguments before it returns. Without it, the caller removes them.

Once one value has a place, every parameter and the result need one.

## Conditions

Conditions are C17 expressions, compiled for the host by clang. They can use
the following names:

| Name | Value |
|---|---|
| parameters | the arguments of the call |
| `*p` for a pointer `p` | the buffer before the call in `requires`, after it in `ensures` |
| `result` | the result, in `ensures` |
| `A` `B` `C` `D` `E` `H` `L` | 8-bit registers after the call, as `uint8_t` |
| `BC` `DE` `HL` `IX` `IY` `SP` | 16-bit registers after the call, as `uint16_t` |
| `same(x, y)` | true if the floats have the same bits, or are both NaN |
| `isnan`, `isinf`, `signbit` | the standard float tests |

The macros of `<stdint.h>` and clang builtins such as `__builtin_roundf` can
also be used. `IX` and `IY` are not available on the SM83.

Register values are unsigned, so a signed result needs a cast:
`(int16_t)HL == a % b`.

Signed overflow wraps, and floating-point operations are not fused. Other
undefined behaviour, such as division by zero or an oversized shift, stops the
run and reports the line.

An input is tested only if every `requires` condition holds. Other inputs are
skipped and not counted as checked. Each `ensures` condition is checked
separately, and the first one that fails is reported with the result and the
registers it uses.

## Test settings

The `tests` section sets how the function is tested. Without it, the command
line options apply.

- `exhaustive;` tries every input. Pointer parameters are not counted, and the
  other parameters must total fewer than 64 bits.
- `samples N;` tries N random inputs. About a quarter of the values are
  boundary values of their type: 0, powers of two and their neighbours, and
  the extremes, and for floats also ±0.5, 2²³, 2³¹, infinities and NaNs.
- `example a = value, b = value;` tries these arguments before any others.
  Values are C expressions such as `INT16_MIN`, `-0.5f` or `0x1p31f`. Missing
  parameters are drawn at random, up to 1000 times, until `requires` holds.
  If it never holds, the example is reported as a failure. Pointer parameters
  cannot be given.

A contract can have either `exhaustive` or `samples`, and any number of
examples. If neither is given, a function is tried on every input when its
parameters total `--max-exhaustive-bits` (32) bits or fewer, and on `--samples`
(2²⁴) inputs otherwise.

## Call checks

Every call is also checked independently of the contract. A call is counted as
a bad call if it:

- runs more than `--step-limit` instructions;
- writes outside `0x8000`–`0xC1FF`, which holds the stack and the buffers;
- halts, or returns somewhere other than its caller;
- leaves SP different from what the calling convention requires;
- changes IX on the Z80.

Registers that carry no argument start with random values, so a function that
reads one of them fails its contract.
