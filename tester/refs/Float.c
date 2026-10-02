// Reference implementations of the f32 runtime functions, in IEEE 754 single
// precision with round-to-nearest-even. Conversions to an integer are defined
// only when the truncated value fits.

#include <stdbool.h>
#include <stdint.h>

typedef __int128 i128;
typedef unsigned __int128 u128;

// Rounding to an integral float

float ref_ceilf(float x) { return __builtin_ceilf(x); }
float ref_floorf(float x) { return __builtin_floorf(x); }
float ref_truncf(float x) { return __builtin_truncf(x); }
float ref_roundf(float x) { return __builtin_roundf(x); }
float ref_rintf(float x) { return __builtin_rintf(x); }
float ref_nearbyintf(float x) { return __builtin_nearbyintf(x); }
float ref_roundevenf(float x) { return __builtin_roundevenf(x); }

// Rounding to an integer; long is 32-bit on the target.

int32_t ref_lroundf(float x) { return (int32_t)__builtin_roundf(x); }
bool pre_lroundf(float x) {
  float r = __builtin_roundf(x);
  return r >= -0x1p31f && r < 0x1p31f;
}

// float to integer

int32_t ref_fixsfsi(float x) { return (int32_t)x; }
bool pre_fixsfsi(float x) { return x >= -0x1p31f && x < 0x1p31f; }

uint32_t ref_fixunssfsi(float x) { return (uint32_t)x; }
bool pre_fixunssfsi(float x) { return x > -1.0f && x < 0x1p32f; }

int64_t ref_fixsfdi(float x) { return (int64_t)x; }
bool pre_fixsfdi(float x) { return x >= -0x1p63f && x < 0x1p63f; }

uint64_t ref_fixunssfdi(float x) { return (uint64_t)x; }
bool pre_fixunssfdi(float x) { return x > -1.0f && x < 0x1p64f; }

i128 ref_fixsfti(float x) { return (i128)x; }
bool pre_fixsfti(float x) { return x >= -0x1p127f && x < 0x1p127f; }

u128 ref_fixunssfti(float x) { return (u128)x; }
bool pre_fixunssfti(float x) { return x > -1.0f && x < __builtin_inff(); }

// integer to float

float ref_floatsisf(int32_t a) { return (float)a; }
float ref_floatunsisf(uint32_t a) { return (float)a; }
float ref_floatdisf(int64_t a) { return (float)a; }
float ref_floatundisf(uint64_t a) { return (float)a; }
float ref_floattisf(i128 a) { return (float)a; }
float ref_floatuntisf(u128 a) { return (float)a; }

// half precision

float ref_extendhfsf2(_Float16 x) { return (float)x; }
_Float16 ref_truncsfhf2(float x) { return (_Float16)x; }

// Arithmetic

float ref_addsf3(float a, float b) { return a + b; }
float ref_subsf3(float a, float b) { return a - b; }
float ref_mulsf3(float a, float b) { return a * b; }
float ref_divsf3(float a, float b) { return a / b; }
