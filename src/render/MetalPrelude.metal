// Prepended to the Metal source before src/core. Not compiled on its own.
//
// src/core calls the C maths library by its global names -- ::sqrtf, ::expf --
// which MSL does not have. These forward them to metal::precise, NOT to the
// default metal:: overloads: those are allowed to be approximations when fast
// maths is on, and the whole point of compiling the same source is that the
// GPU computes what the CPU computes. MetalRender.mm also compiles with fast
// maths off, so this holds whichever way the compiler defaults move.

#include <metal_stdlib>

// The CPU build turns off fused multiply-add contraction (see CMakeLists.txt),
// so the GPU does too. Contracted, a*b+c rounds once instead of twice and the
// two paths drift apart by exactly the kind of error the parity test exists to
// catch -- as with the xform round-trip check on Apple Silicon.
#pragma METAL fp contract(off)

inline float sqrtf(float x)          { return metal::precise::sqrt(x); }
inline float expf(float x)           { return metal::precise::exp(x); }
inline float logf(float x)           { return metal::precise::log(x); }
inline float powf(float x, float y)  { return metal::precise::pow(x, y); }
inline float floorf(float x)         { return metal::floor(x); }
inline float fabsf(float x)          { return metal::fabs(x); }
