#pragma once

// Device-safe primitives shared by the CPU renderer and the CUDA kernel.
//
// Everything in src/core is header-only and includes no OFX header. That is what
// lets tools/linetest and tools/cudatest link the *identical* code Resolve runs,
// rather than a second implementation that can quietly drift from it.

#include <cmath>

// __host__ __device__ under nvcc, nothing otherwise. The same trick as
// resolve-multi-transform's MTX_HD: one body of maths, two compilers, no way for
// the CPU and GPU paths to disagree because there is only one of them.
#if defined(__CUDACC__)
  #define BL_HD __host__ __device__
#else
  #define BL_HD
#endif

namespace bl {

// Free functions rather than std::min/std::max/std::clamp: the std versions are
// not callable from device code, and <algorithm> is not includable there.
BL_HD inline float Min(float a, float b) { return a < b ? a : b; }
BL_HD inline float Max(float a, float b) { return a > b ? a : b; }
BL_HD inline float Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
BL_HD inline float Saturate(float v) { return Clamp(v, 0.0f, 1.0f); }
BL_HD inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

/** @brief Hermite smoothstep between two edges, with a degenerate-width guard.
 *
 * The guard is load-bearing, not defensive dressing. A zero feather is the
 * normal case for a hard-edged line, and it makes e1 == e0, so the unguarded
 * form divides by zero and every pixel comes out NaN -- which composites as a
 * black hole rather than a line. Falling back to a step is what "no feather"
 * should mean anyway.
 */
BL_HD inline float SmoothStep(float e0, float e1, float x)
{
    const float w = e1 - e0;
    if (w <= 1e-9f) return x < e0 ? 0.0f : 1.0f;
    const float t = Saturate((x - e0) / w);
    return t * t * (3.0f - 2.0f * t);
}

/** @brief Coverage of a GAUSSIAN-BLURRED edge at signed distance @p d.
 *
 * Not the same thing as a smoothstep hung off the edge, and the difference is
 * the whole reason a distance-field drop shadow can look like a sticker rather
 * than a shadow. A blur kernel is symmetric, so half of it lands either side of
 * the boundary: the silhouette itself comes out at HALF coverage, the inside
 * fades down to meet it, and the outside fades away from it. A ramp that starts
 * at the silhouette and only travels outward leaves the interior at full opacity
 * right up to the edge -- a solid copy with a rim around it, which is exactly
 * how it reads.
 *
 * It also gets thin shapes right for free. A hairline under a wide blur should
 * come out faint and broad, because no part of it is ever more than a fraction
 * of a sigma from an edge; the outward-only ramp instead keeps its core at full
 * strength however hard it is blurred, and that core is the "no falloff" look.
 *
 * The profile is the normal CDF via its logistic approximation, whose error
 * peaks below 0.01 -- far under a single 8-bit code value once an opacity is
 * applied. The alternative is erff(), which is not device code.
 *
 * Well-behaved at both extremes without a guard: a large positive @p d overflows
 * expf to +inf and the reciprocal is 0, a large negative one underflows to 0 and
 * the reciprocal is 1. Both are the right answer.
 */
BL_HD inline float BlurredEdge(float d, float sigma)
{
    if (sigma <= 1e-6f) return d < 0.0f ? 1.0f : 0.0f;
    return 1.0f / (1.0f + ::expf(1.702f * d / sigma));
}

/// Where a BlurredEdge is truncated, in sigmas.
///
/// The kernel has infinite tails but the tile grid does not, so the profile is
/// cut off wherever the binning stops caring -- and the two have to be the SAME
/// number or the shadow steps at a tile boundary. 3.5 sigma leaves 0.0026
/// coverage on the table, under a single 8-bit code value even at full opacity,
/// which is what makes the cut invisible rather than merely small.
constexpr float kBlurCutoffSigmas = 3.5f;

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;

    BL_HD Vec2() {}
    BL_HD Vec2(float px, float py) : x(px), y(py) {}
};

BL_HD inline Vec2  operator+(Vec2 a, Vec2 b)  { return Vec2(a.x + b.x, a.y + b.y); }
BL_HD inline Vec2  operator-(Vec2 a, Vec2 b)  { return Vec2(a.x - b.x, a.y - b.y); }
BL_HD inline Vec2  operator*(Vec2 a, float s) { return Vec2(a.x * s, a.y * s); }
BL_HD inline Vec2  operator*(float s, Vec2 a) { return Vec2(a.x * s, a.y * s); }
BL_HD inline float Dot(Vec2 a, Vec2 b)        { return a.x * b.x + a.y * b.y; }
BL_HD inline float Cross(Vec2 a, Vec2 b)      { return a.x * b.y - a.y * b.x; }
BL_HD inline float Dot2(Vec2 a)               { return Dot(a, a); }
BL_HD inline float Length(Vec2 a)             { return ::sqrtf(Dot(a, a)); }

/** @brief Unit vector, or a fallback for a zero-length input.
 *
 * Coincident control points are not a pathological case here -- they are what a
 * user produces the moment they double-click a point, and what a fully collapsed
 * trim produces every time an animation starts. Returning the fallback keeps a
 * degenerate segment drawing as a round dot instead of a NaN.
 */
BL_HD inline Vec2 Normalize(Vec2 a, Vec2 fallback = Vec2(1.0f, 0.0f))
{
    const float l = Length(a);
    return l > 1e-12f ? a * (1.0f / l) : fallback;
}

BL_HD inline Vec2 Perp(Vec2 a) { return Vec2(-a.y, a.x); }

/** @brief Axis-aligned bounds, in pixels. Also the tile-grid currency. */
struct Bounds
{
    float x0 =  1e30f;
    float y0 =  1e30f;
    float x1 = -1e30f;
    float y1 = -1e30f;

    BL_HD void Add(Vec2 p)
    {
        x0 = Min(x0, p.x); y0 = Min(y0, p.y);
        x1 = Max(x1, p.x); y1 = Max(y1, p.y);
    }
    BL_HD void Expand(float r) { x0 -= r; y0 -= r; x1 += r; y1 += r; }
    BL_HD bool Empty() const   { return x1 < x0 || y1 < y0; }
    BL_HD bool Overlaps(const Bounds& o) const
    {
        return !(o.x0 > x1 || o.x1 < x0 || o.y0 > y1 || o.y1 < y0);
    }
};

} // namespace bl
