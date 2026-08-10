#pragma once

// Procedural wiggle: a seeded, deterministic wobble along the path.
//
// Deterministic is the whole requirement. Noise drawn from anything that varies
// per render -- a call counter, an address, an uninitialised seed -- shimmers
// between frames, and a line that shimmers reads as broken rather than
// hand-drawn. Everything here is a pure function of arc length, time and seed.

#include "Common.h"
#include "Geometry.h"

#include <cstdint>
#include <vector>

namespace bl {

namespace detail {

/** Integer avalanche hash. Cheap, and far better distributed than the
 *  sin(x)*43758.5 trick, which is famously platform-dependent -- exactly the
 *  property this must not have when the CPU and GPU paths are compared. */
inline uint32_t Hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

inline float Hash01(int i, unsigned seed)
{
    const uint32_t h = Hash32(static_cast<uint32_t>(i) * 374761393u + seed * 668265263u);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

/** Smooth 1D value noise in -1..1. */
inline float ValueNoise(float x, unsigned seed)
{
    const float xf = ::floorf(x);
    const float t  = x - xf;
    const int   i  = static_cast<int>(xf);

    const float a = Hash01(i,     seed);
    const float b = Hash01(i + 1, seed);

    return Lerp(a, b, t * t * (3.0f - 2.0f * t)) * 2.0f - 1.0f;
}

/** Two octaves. One is recognisably a sine wave; two reads as a wobble. */
inline float Fbm(float x, unsigned seed)
{
    return ValueNoise(x, seed) + 0.5f * ValueNoise(x * 2.17f + 11.3f, seed ^ 0x9e3779b9u);
}

} // namespace detail

/** @brief Displace a flattened path perpendicular to itself.
 *
 * @param amp        peak displacement in pixels
 * @param wavelength pixels per wobble; smaller is scribblier
 * @param phase      advances the noise, so animating it makes the line crawl
 * @param seed       two strokes with the same seed wobble identically
 *
 * The path is RESAMPLED first, at a fraction of the wavelength. Without that the
 * wobble can only appear where the flattener happened to put vertices, and a
 * straight span has exactly two of them -- so a straight line would merely have
 * its endpoints nudged, which is the same failure the width profile hit.
 *
 * Displacement is perpendicular to the local direction rather than in x and y
 * independently. A free 2D offset stretches and compresses the path as well as
 * bending it, so the line visibly changes length as it wobbles; perpendicular
 * displacement bends without pumping.
 */
inline void WigglePolyline(const Polyline& in, float amp, float wavelength,
                           float phase, unsigned seed, Polyline& out)
{
    out.pts.clear();
    out.cum.clear();

    const float total = in.Total();
    if (in.pts.size() < 2 || total <= 1e-6f || amp <= 1e-6f) { out = in; return; }

    const float wl   = Max(wavelength, 1.0f);
    const float step = Max(wl * 0.125f, 0.75f);

    // Bounded independently of the loop condition: a wavelength driven tiny by
    // an animated parameter would otherwise allocate without limit inside the
    // render action.
    int n = static_cast<int>(::ceilf(total / step));
    if (n < 2)     n = 2;
    if (n > 20000) n = 20000;

    out.pts.reserve(static_cast<size_t>(n) + 1);
    out.cum.reserve(static_cast<size_t>(n) + 1);

    size_t hint = 0;
    for (int i = 0; i <= n; ++i)
    {
        const float d = total * static_cast<float>(i) / static_cast<float>(n);

        size_t th = 0;
        const Vec2 p    = PointAtDistance(in, d, hint);
        const Vec2 dir  = TangentAtDistance(in, d, th);
        const Vec2 perp = Perp(dir);

        // Noise indexed by ARC LENGTH, so the wobble is pinned to the path and
        // stays put when the path is trimmed or dashed. Indexing by vertex
        // number instead would make the pattern slide whenever the flattening
        // tolerance changed.
        const float k = detail::Fbm(d / wl + phase, seed);
        const Vec2  q = p + perp * (amp * k);

        if (!out.pts.empty())
        {
            const float seg = Length(q - out.pts.back());
            if (seg < 1e-6f) continue;
            out.cum.push_back(out.cum.back() + seg);
        }
        else
        {
            out.cum.push_back(0.0f);
        }
        out.pts.push_back(q);
    }

    if (out.pts.size() < 2) out = in;
}

} // namespace bl
