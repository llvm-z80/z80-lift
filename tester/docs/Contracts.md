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

- The prototype is one line, starting at the beginning of the line (after
  `;@ ` in assembly). The indented lines below it belong to the same
  contract.
- `requires`, `modifies`, `ensures` and `tests` begin a section. Each item
  ends with `;` and may span several lines. An item may follow the keyword on
  the same line.
- An `#include` line at the beginning of a line adds a C header for every
  contract. A quoted path is relative to the file.
- `//` begins a comment that ends at the end of the line.
- Ranges are half-open everywhere: `lo .. hi` is from `lo` up to, not
  including, `hi`.

```asm
;@ uint8_t __udivqi3(uint8_t a, uint8_t b)
;@     requires b != 0;           // an item on the keyword's line
;@     ensures
;@         result == a / b;
```

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

`char`, `short`, `int`, `long`, `double`, `size_t` and `bool` are rejected. A
parameter cannot be named after a register or `result`.

```asm
;@ float __addsf3(float a, float b)
;@ unsigned __int128 __udivti3(unsigned __int128 a, unsigned __int128 b)
;@ int32_t __divmodsi4(int32_t a, int32_t b, int32_t *rem)
;@ uint16_t strlen(const uint8_t *s)
```

## Calling convention

A function outside `__sdcccall(1)` gives the place of every value in its
prototype:

- `__reg(R)` after a parameter, or after the parameter list for the result,
  names its registers, most significant first: `A` to `L`, `BC`, `DE` and
  `HL`, as in `A`, `HL` or `DEHL`. They must hold exactly the value's size.
- `__stack(N)` passes a parameter in the stack arguments, N bytes above the
  return address.
- `__pops(N)` after the parameter list means the function removes N bytes of
  stack arguments before it returns. Without it, the caller removes them.

Once one value has a place, every parameter and the result need one.

Only `__sdcccall(1)` promises to keep IX, so a function placed by hand is not
checked for it. A register it must keep goes in `ensures`, as
`IX == old(IX);`.

The backend calls `__z80_memcpy_builtin` with all three arguments in
registers:

```asm
;@ void __z80_memcpy_builtin(uint8_t *dst __reg(HL), const uint8_t *src __reg(DE), uint16_t n __reg(BC))
```

A stack argument removed by the function, written out in full. This is how
`__sdcccall(1)` passes it anyway:

```asm
;@ float __addsf3_fast(float a __reg(HLDE), float b __stack(0)) __reg(HLDE) __pops(4)
```

## Memory

Each pointer argument points to a buffer at a random address. Buffers may
overlap, so a contract that does not allow overlap says so in `requires`.

`modifies` lists the memory a function may write through its pointers:

- `p[lo .. hi]` is the bytes from `p + lo` up to `p + hi`; `*p` is the value
  `p` points to. The bounds are C expressions, read before the call.
- Apart from its own stack, a function may write only to what `modifies`
  lists. Writing anywhere else breaks the contract, even without a
  `modifies` section.
- A buffer is as large as what `modifies` lists in it, or its string.

`memmove` must handle overlapping buffers, and `memcpy` need not:

```asm
;@ uint8_t *memmove(uint8_t *dst, const uint8_t *src, uint16_t n)
;@     modifies
;@         dst[0 .. n];
;@     ensures
;@         result == dst;
;@         forall(i, 0, n, dst[i] == old(src[i]));
;@     tests
;@         n in 0 .. 600;
```

```asm
;@ uint8_t *memcpy(uint8_t *dst, const uint8_t *src, uint16_t n)
;@     requires
;@         dst + n <= src || src + n <= dst;
;@     ...
```

An argument the function writes its answer through:

```asm
;@ int32_t __divmodsi4(int32_t a, int32_t b, int32_t *rem)
;@     requires
;@         b != 0;
;@         !(a == INT32_MIN && b == -1);
;@     modifies
;@         *rem;
;@     ensures
;@         result == a / b;
;@         *rem == a % b;
```

A buffer sized by a string argument:

```asm
;@ uint8_t *strcpy(uint8_t *dst, const uint8_t *src)
;@     modifies
;@         dst[0 .. strlen((const char *)src) + 1];
;@     ensures
;@         result == dst;
;@         strcmp((const char *)dst, (const char *)src) == 0;
;@     tests
;@         src in string(0 .. 100);
```

## Conditions

Conditions are C17 expressions, compiled for the host by clang. They can use
the following names:

| Name | Value |
|---|---|
| parameters | the arguments of the call; a pointer points to its buffer |
| `result` | the result, in `ensures`; a pointer result points into the same memory |
| `A` `B` `C` `D` `E` `H` `L` | 8-bit registers after the call, as `uint8_t` |
| `BC` `DE` `HL` `IX` `IY` `SP` | 16-bit registers after the call, as `uint16_t` |
| `old(e)` | `e` before the call, with memory and registers as they were |
| `forall(i, lo, hi, c)` | true if `c` holds for every `i` in `lo .. hi` |
| `exists(i, lo, hi, c)` | true if `c` holds for some `i` in `lo .. hi` |
| `same(x, y)` | true if the floats have the same bits, or are both NaN |

`requires` sees memory before the call and `ensures` after it. `IX` and `IY`
are not available on the SM83.

A value the function leaves in a register besides its result:

```asm
;@ int16_t __divmodhi4(int16_t a, int16_t b)
;@     requires
;@         b != 0;
;@         !(a == INT16_MIN && b == -1);
;@     ensures
;@         result == a / b;
;@         (int16_t)HL == a % b;
```

Register values are unsigned, so a signed one needs a cast, as above.

Arithmetic follows the host's C, where `int` is 32 bits, so a product of two
`uint16_t` values keeps its high bits unless it is cast:

```asm
;@ uint16_t __mulhi3(uint16_t a, uint16_t b)
;@     ensures
;@         result == (uint16_t)(a * b);
```

The headers `<math.h>`, `<string.h>`, `<stdint.h>` and `<stdbool.h>` are
included, so their functions serve as references:

```asm
;@ float floorf(float x)
;@     ensures
;@         same(result, floorf(x));
```

```asm
;@ uint8_t *strchr(const uint8_t *s, int16_t c)
;@     ensures
;@         result == (uint8_t *)strchr((const char *)s, (uint8_t)c);
;@     tests
;@         s in string;
```

Other helpers come from a header of your own:

```asm
;@ #include "helpers.h"    // static inline int sign(int x) { return (x > 0) - (x < 0); }
;@
;@ int16_t strcmp(const uint8_t *a, const uint8_t *b)
;@     ensures
;@         sign(result) == sign(strcmp((const char *)a, (const char *)b));
;@     tests
;@         a, b in string(0 .. 40);
```

A pointer that `old()` returns points into memory as it was, so compare
pointers outside `old()`.

Signed overflow wraps. Other undefined behaviour, such as division by zero,
an oversized shift or a bad pointer, stops the run and reports the line.

Inputs that do not meet `requires` are skipped and not counted as checked.

## Test settings

The `tests` section sets how the function is tested. Without it, the command
line options apply.

- `exhaustive;` tries every input. Pointer and string parameters are not
  counted.
- `samples N;` tries N random inputs.
- `x in lo .. hi;` draws a number from a range. `x, y in lo .. hi;` gives
  several parameters the same range.
- `s in string(lo .. hi);` makes a pointer a NUL-terminated string whose
  length is in the range; `s in string;` allows lengths up to 64.
- `example a = value, b = value;` tries these arguments before any others.
  Values are C expressions such as `INT16_MIN`, `-0.5f` or `0x1p31f`; a
  pointer takes a string literal, whose bytes start its buffer. Parameters
  it leaves out are drawn at random until `requires` holds.

A contract can have either `exhaustive` or `samples`, and any number of
examples. If neither is given, a function is tried on every input when there
are at most 2 to the power of `--max-exhaustive-bits` (32) of them, and on
`--samples` (2²⁴) inputs otherwise.

Every float input, whatever `--max-exhaustive-bits` says:

```asm
;@ float roundf(float x)
;@     ensures
;@         same(result, roundf(x));
;@     tests
;@         exhaustive;
```

Far fewer samples for a slow function, and the largest dividend:

```asm
;@ unsigned __int128 __udivti3(unsigned __int128 a, unsigned __int128 b)
;@     requires
;@         b != 0;
;@     ensures
;@         result == a / b;
;@     tests
;@         samples 20000;
;@         example a = ~(unsigned __int128)0, b = 3;
```

Floats below 2²⁴, where the fraction bits matter:

```asm
;@     tests
;@         x in -0x1p24f .. 0x1p24f;
```

Strings, with cases that matter for comparisons:

```asm
;@     tests
;@         a, b in string(0 .. 40);
;@         example a = "abc", b = "abd";
;@         example a = "\x80", b = "\x01";
```

An example that fixes one argument and draws the other:

```asm
;@     tests
;@         example a = INT16_MIN;
```

## Call checks

Every call is also checked independently of the contract. A call is counted as
a bad call if it:

- runs more than `--step-limit` instructions;
- writes to memory that is neither its stack nor its arguments' buffers;
- halts, or returns somewhere other than its caller;
- leaves SP different from what the calling convention requires;
- changes IX on the Z80, under `__sdcccall(1)`.

Registers that carry no argument start with random values, so a function that
reads one of them fails its contract.
