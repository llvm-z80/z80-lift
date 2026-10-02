// Reference implementations of the integer runtime functions. Avoid UB such
// as promoted signed overflow: alive-tv takes it as license to accept anything.
//
// ref_<name> computes what the runtime function promises. pre_<name>, where
// present, says for which inputs the result is defined; elsewhere only the
// call itself is checked.

#include <stdbool.h>
#include <stdint.h>

typedef __int128 i128;
typedef unsigned __int128 u128;

// 8-bit

uint8_t ref_udivqi3(uint8_t a, uint8_t b) { return a / b; }
bool pre_udivqi3(uint8_t a, uint8_t b) { return b != 0; }

uint8_t ref_umodqi3(uint8_t a, uint8_t b) { return a % b; }
bool pre_umodqi3(uint8_t a, uint8_t b) { return b != 0; }

// 16-bit

uint16_t ref_mulhi3(uint16_t a, uint16_t b) {
  return (uint16_t)((uint32_t)a * b);
}

uint16_t ref_umulhi3(uint16_t a, uint16_t b) {
  return (uint16_t)(((uint32_t)a * b) >> 16);
}

static bool sdiv16_defined(int16_t a, int16_t b) {
  return b != 0 && !(a == INT16_MIN && b == -1);
}

int16_t ref_divhi3(int16_t a, int16_t b) { return (int16_t)(a / b); }
bool pre_divhi3(int16_t a, int16_t b) { return sdiv16_defined(a, b); }

int16_t ref_modhi3(int16_t a, int16_t b) { return (int16_t)(a % b); }
bool pre_modhi3(int16_t a, int16_t b) { return sdiv16_defined(a, b); }

uint16_t ref_udivhi3(uint16_t a, uint16_t b) { return a / b; }
bool pre_udivhi3(uint16_t a, uint16_t b) { return b != 0; }

uint16_t ref_umodhi3(uint16_t a, uint16_t b) { return a % b; }
bool pre_umodhi3(uint16_t a, uint16_t b) { return b != 0; }

uint16_t ref_udivmodhi4(uint16_t a, uint16_t b, uint16_t *rem) {
  *rem = a % b;
  return a / b;
}
bool pre_udivmodhi4(uint16_t a, uint16_t b) { return b != 0; }

int16_t ref_divmodhi4(int16_t a, int16_t b, int16_t *rem) {
  *rem = (int16_t)(a % b);
  return (int16_t)(a / b);
}
bool pre_divmodhi4(int16_t a, int16_t b) { return sdiv16_defined(a, b); }

// 32-bit

uint32_t ref_mulsi3(uint32_t a, uint32_t b) { return a * b; }

static bool sdiv32_defined(int32_t a, int32_t b) {
  return b != 0 && !(a == INT32_MIN && b == -1);
}

int32_t ref_divsi3(int32_t a, int32_t b) { return a / b; }
bool pre_divsi3(int32_t a, int32_t b) { return sdiv32_defined(a, b); }

int32_t ref_modsi3(int32_t a, int32_t b) { return a % b; }
bool pre_modsi3(int32_t a, int32_t b) { return sdiv32_defined(a, b); }

uint32_t ref_udivsi3(uint32_t a, uint32_t b) { return a / b; }
bool pre_udivsi3(uint32_t a, uint32_t b) { return b != 0; }

uint32_t ref_umodsi3(uint32_t a, uint32_t b) { return a % b; }
bool pre_umodsi3(uint32_t a, uint32_t b) { return b != 0; }

int32_t ref_divmodsi4(int32_t a, int32_t b, int32_t *rem) {
  *rem = a % b;
  return a / b;
}
bool pre_divmodsi4(int32_t a, int32_t b) { return sdiv32_defined(a, b); }

uint32_t ref_udivmodsi4(uint32_t a, uint32_t b, uint32_t *rem) {
  *rem = a % b;
  return a / b;
}
bool pre_udivmodsi4(uint32_t a, uint32_t b) { return b != 0; }

// 64-bit

uint64_t ref_muldi3(uint64_t a, uint64_t b) { return a * b; }

static bool sdiv64_defined(int64_t a, int64_t b) {
  return b != 0 && !(a == INT64_MIN && b == -1);
}

int64_t ref_divdi3(int64_t a, int64_t b) { return a / b; }
bool pre_divdi3(int64_t a, int64_t b) { return sdiv64_defined(a, b); }

int64_t ref_moddi3(int64_t a, int64_t b) { return a % b; }
bool pre_moddi3(int64_t a, int64_t b) { return sdiv64_defined(a, b); }

uint64_t ref_udivdi3(uint64_t a, uint64_t b) { return a / b; }
bool pre_udivdi3(uint64_t a, uint64_t b) { return b != 0; }

uint64_t ref_umoddi3(uint64_t a, uint64_t b) { return a % b; }
bool pre_umoddi3(uint64_t a, uint64_t b) { return b != 0; }

// 128-bit

static const i128 I128_MIN = (i128)((u128)1 << 127);

static bool sdiv128_defined(i128 a, i128 b) {
  return b != 0 && !(a == I128_MIN && b == -1);
}

i128 ref_divti3(i128 a, i128 b) { return a / b; }
bool pre_divti3(i128 a, i128 b) { return sdiv128_defined(a, b); }

i128 ref_modti3(i128 a, i128 b) { return a % b; }
bool pre_modti3(i128 a, i128 b) { return sdiv128_defined(a, b); }

u128 ref_udivti3(u128 a, u128 b) { return a / b; }
bool pre_udivti3(u128 a, u128 b) { return b != 0; }

u128 ref_umodti3(u128 a, u128 b) { return a % b; }
bool pre_umodti3(u128 a, u128 b) { return b != 0; }
