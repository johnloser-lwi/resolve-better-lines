// CPU vs Metal parity -- the macOS counterpart of tools/cudatest.
//
// src/core is header-only so that the CPU and the GPU compile the same
// ShadePixel, but "same source" is not "same result": the Metal compiler has
// its own maths library and its own idea of float reassociation. In Resolve
// every frame on a Mac goes through this path, so a divergence a linetest
// render cannot see is a divergence nobody sees until it ships.
//
// Also checks what CUDA never has to: the Metal path does its own pixel
// addressing (images arrive as whole buffers, not pointers to a pixel), so one
// case renders an offset window into padded, offset image bounds.
//
// Exit code 0 on agreement, 1 on divergence or Metal failure.

#include "ParityCases.h"
#include "core/TileGrid.h"
#include "core/FillGrid.h"
#include "render/MetalRender.h"

#import <Metal/Metal.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace bl;
using namespace parity;

namespace {

id<MTLDevice>       gDev;
id<MTLCommandQueue> gQueue;

/// Wait for everything enqueued so far. The queue is serial, so an empty
/// command buffer completing means the render before it has too.
void Drain()
{
    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    [cb commit];
    [cb waitUntilCompleted];
}

/// Owning scene, CPU-built exactly as render() builds it.
struct Built
{
    std::vector<Segment>  segs;
    std::vector<FillEdge> fill;
    TileGrid grid;
    BandGrid bands;
    SceneView view;
    GpuSceneDesc desc;

    void Finish(const bl::Style* style, int x0, int y0, int x1, int y1, float reach, float feather)
    {
        BuildTileGrid(segs, reach + 1.0f, x0, y0, x1, y1, 16, grid);
        BuildBandGrid(fill, feather + 1.0f, y0, y1, 16, bands);

        view.segs       = segs.empty() ? nullptr : segs.data();
        view.segCount   = int(segs.size());
        view.grid       = grid.View();
        view.fill       = fill.empty() ? nullptr : fill.data();
        view.fillCount  = int(fill.size());
        view.bands      = bands.View();
        view.styles     = style;
        view.styleCount = 1;

        desc.segs           = view.segs;
        desc.segCount       = view.segCount;
        desc.tileStart      = grid.start.empty() ? nullptr : grid.start.data();
        desc.tileStartCount = int(grid.start.size());
        desc.tileItems      = grid.items.empty() ? nullptr : grid.items.data();
        desc.tileItemCount  = int(grid.items.size());
        desc.gridMeta       = grid.View();
        desc.fill           = view.fill;
        desc.fillCount      = view.fillCount;
        desc.bandStart      = bands.start.empty() ? nullptr : bands.start.data();
        desc.bandStartCount = int(bands.start.size());
        desc.bandItems      = bands.items.empty() ? nullptr : bands.items.data();
        desc.bandItemCount  = int(bands.items.size());
        desc.bandMeta       = bands.View();
        desc.styles         = style;
        desc.styleCount     = 1;
    }
};

void BuildCase(const Case& c, Built& b, int x0, int y0, int x1, int y1)
{
    Polyline poly;
    FlattenStroke(c.stroke, 0.2f, poly);
    if (c.style.fill && c.stroke.closed) AppendFillContour(poly.pts, 0, b.fill);
    BuildStrokeSegments(poly, c.style, c.trim, c.stroke.closed,
                        c.dashLen, c.gapLen, 0.0f, b.segs);
    b.Finish(&c.style, x0, y0, x1, y1, c.style.MaxReach(), c.style.feather);
}

/// A non-trivial source, not black: compositing over black hides a whole class
/// of premultiplication mistakes, because the term that would disagree is
/// multiplied by zero.
void FillSource(std::vector<float>& img, int w, int h, int strideFloats, int ox, int oy)
{
    img.assign(size_t(strideFloats) * size_t(h), -7.0f);   // padding: never read
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float* p = &img[size_t(y) * strideFloats + size_t(x) * 4];
            p[0] = float(x + ox) / kSize;
            p[1] = float(y + oy) / kSize;
            p[2] = 0.25f;
            p[3] = 1.0f;
        }
}

id<MTLBuffer> Buf(const std::vector<float>& v)
{
    return [gDev newBufferWithBytes:v.data() length:v.size() * sizeof(float)
                            options:MTLResourceStorageModeShared];
}

// Tolerances, with the same rationale as cudatest: float reassociation between
// two compilers and nothing more, plus a tiny allowance for the arrowhead/shaft
// attribute tie (see cudatest for the full argument). Tight on purpose -- a
// comfortable tolerance is how a real divergence hides.
const float  kTol            = 5e-6f;
const double kMaxBadFraction = 0.0025;
const float  kAbsCeiling     = 2.5e-2f;

struct Diff { double worst = 0.0; size_t worstAt = 0; size_t bad = 0; size_t n = 0; };

Diff Compare(const float* gpu, const float* cpu, size_t n)
{
    Diff d; d.n = n;
    for (size_t i = 0; i < n; ++i)
    {
        const double e = std::fabs(double(gpu[i]) - double(cpu[i]));
        if (e > d.worst) { d.worst = e; d.worstAt = i; }
        if (e > kTol) ++d.bad;
    }
    return d;
}

bool Report(const char* name, size_t segs, const Diff& d)
{
    const double frac = double(d.bad) / double(d.n);
    const bool ok = (d.worst <= kTol) || (frac <= kMaxBadFraction && d.worst <= kAbsCeiling);
    std::printf("  %-30s segs=%4zu  maxdiff=%.3e  bad=%zu (%.3f%%)  %s\n",
                name, segs, d.worst, d.bad, frac * 100.0,
                ok ? (d.worst <= kTol ? "ok" : "ok (boundary)") : "DIVERGED");
    return ok;
}

} // namespace

int main()
{
    @autoreleasepool
    {
        gDev = MTLCreateSystemDefaultDevice();
        if (!MetalAvailable() || !gDev)
        {
            std::fprintf(stderr, "metaltest: no Metal device available\n");
            return 1;
        }
        gQueue = [gDev newCommandQueue];
        std::printf("metaltest on %s\n", gDev.name.UTF8String);

        int failures = 0;
        std::string err;

        const size_t pixels = size_t(kSize) * kSize;
        std::vector<float> srcHost;
        FillSource(srcHost, kSize, kSize, kSize * 4, 0, 0);
        id<MTLBuffer> srcBuf = Buf(srcHost);
        id<MTLBuffer> dstBuf = [gDev newBufferWithLength:pixels * 16
                                                 options:MTLResourceStorageModeShared];

        MetalImage srcIm; srcIm.buffer = (__bridge void*)srcBuf;
        srcIm.x2 = kSize; srcIm.y2 = kSize; srcIm.rowBytes = kSize * 16;
        MetalImage dstIm = srcIm; dstIm.buffer = (__bridge void*)dstBuf;

        // --- the shared cases ----------------------------------------------
        std::vector<Case> cases = MakeCases();
        bool firstCall = true;
        for (const Case& c : cases)
        {
            Built b;
            BuildCase(c, b, 0, 0, kSize, kSize);

            std::vector<float> cpu(srcHost);
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x)
                {
                    const Premul line = ShadePixel(b.view, x + 0.5f, y + 0.5f);
                    if (line.a > 0.0f) Over(line, &cpu[(size_t(y) * kSize + x) * 4]);
                }

            std::memset(dstBuf.contents, 0xFF, pixels * 16);   // NaN: unwritten shows
            const auto t0 = std::chrono::steady_clock::now();
            if (!MetalRenderLine((__bridge void*)gQueue, &srcIm, dstIm,
                                 0, 0, kSize, kSize, &b.desc, 1, &err))
            {
                std::printf("  %-30s METAL RENDER FAILED: %s\n", c.name, err.c_str());
                ++failures;
                continue;
            }
            Drain();
            if (firstCall)
            {
                firstCall = false;
                const double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::printf("  (shader compile + first frame: %.0f ms)\n", ms);
            }

            if (!Report(c.name, b.segs.size(),
                        Compare(static_cast<const float*>(dstBuf.contents), cpu.data(), pixels * 4)))
                ++failures;
        }

        // --- motion blur -----------------------------------------------------
        // A different pair of kernels -- accumulate, then composite once -- that
        // none of the cases above reach, because they all run one sample.
        {
            const int kSamples = 4;
            bl::Style st;
            st.halfWidth = 9.0f; st.feather = 1.2f;
            st.color = Color(1.0f, 0.85f, 0.3f, 1.0f);

            std::vector<Built> bs(kSamples);
            std::vector<GpuSceneDesc> descs(kSamples);
            for (int k = 0; k < kSamples; ++k)
            {
                const float dx = float(k) * 11.0f;
                Stroke sk;
                sk.pts = { CP(40 + dx, 70, 70, 60), CP(150 + dx, 200, 50, 0),
                           CP(215 + dx, 70, 0, -60) };
                Polyline poly;
                FlattenStroke(sk, 0.2f, poly);
                TrimState trim;
                BuildStrokeSegments(poly, st, trim, false, 0.0f, 0.0f, 0.0f, bs[k].segs);
                bs[k].Finish(&st, 0, 0, kSize, kSize, st.MaxReach(), st.feather);
                descs[k] = bs[k].desc;
            }

            std::vector<float> cpu(srcHost);
            const float weight = 1.0f / float(kSamples);
            for (int y = 0; y < kSize; ++y)
                for (int x = 0; x < kSize; ++x)
                {
                    Premul acc; acc.r = acc.g = acc.b = acc.a = 0.0f;
                    for (int k = 0; k < kSamples; ++k)
                    {
                        const Premul s2 = ShadePixel(bs[k].view, x + 0.5f, y + 0.5f);
                        acc.r += s2.r * weight; acc.g += s2.g * weight;
                        acc.b += s2.b * weight; acc.a += s2.a * weight;
                    }
                    if (acc.a > 0.0f) Over(acc, &cpu[(size_t(y) * kSize + x) * 4]);
                }

            std::memset(dstBuf.contents, 0xFF, pixels * 16);
            if (!MetalRenderLine((__bridge void*)gQueue, &srcIm, dstIm, 0, 0, kSize, kSize,
                                 descs.data(), kSamples, &err))
            {
                std::printf("  %-30s METAL BLUR RENDER FAILED: %s\n", "motion blur x4", err.c_str());
                ++failures;
            }
            else
            {
                Drain();
                if (!Report("motion blur x4", bs[0].segs.size(),
                            Compare(static_cast<const float*>(dstBuf.contents), cpu.data(), pixels * 4)))
                    ++failures;
            }
        }

        // --- addressing --------------------------------------------------------
        // Destination bounds start at (-24,-10) with 40 floats of row padding;
        // the source covers only part of the window; the window is offset inside
        // both. Pixels the window does not cover must be left untouched, and
        // pixels outside the source must composite over transparent black --
        // the CPU path's null-row behaviour.
        {
            const Case& c = cases[3];   // outline + glow + gradient: wide reach
            const int dX1 = -24, dY1 = -10, dW = 300, dH = 280, dStride = dW * 4 + 40;
            const int sX1 = 20,  sY1 = 30,  sW = 180, sH = 150, sStride = sW * 4 + 12;
            const int wx0 = 5, wy0 = 7, wx1 = 241, wy1 = 230;

            Built b;
            BuildCase(c, b, wx0, wy0, wx1, wy1);

            std::vector<float> src;
            FillSource(src, sW, sH, sStride, sX1, sY1);
            std::vector<float> dst(size_t(dStride) * dH, 0.5f);   // sentinel outside window

            std::vector<float> cpu(dst);
            for (int y = wy0; y < wy1; ++y)
                for (int x = wx0; x < wx1; ++x)
                {
                    float px[4] = {0, 0, 0, 0};
                    if (x >= sX1 && x < sX1 + sW && y >= sY1 && y < sY1 + sH)
                        std::memcpy(px, &src[size_t(y - sY1) * sStride + size_t(x - sX1) * 4], 16);
                    const Premul line = ShadePixel(b.view, x + 0.5f, y + 0.5f);
                    if (line.a > 0.0f) Over(line, px);
                    std::memcpy(&cpu[size_t(y - dY1) * dStride + size_t(x - dX1) * 4], px, 16);
                }

            id<MTLBuffer> sb = Buf(src);
            id<MTLBuffer> db = Buf(dst);
            MetalImage si; si.buffer = (__bridge void*)sb;
            si.x1 = sX1; si.y1 = sY1; si.x2 = sX1 + sW; si.y2 = sY1 + sH; si.rowBytes = sStride * 4;
            MetalImage di; di.buffer = (__bridge void*)db;
            di.x1 = dX1; di.y1 = dY1; di.x2 = dX1 + dW; di.y2 = dY1 + dH; di.rowBytes = dStride * 4;

            if (!MetalRenderLine((__bridge void*)gQueue, &si, di, wx0, wy0, wx1, wy1,
                                 &b.desc, 1, &err))
            {
                std::printf("  %-30s METAL RENDER FAILED: %s\n", "offset window, padded rows", err.c_str());
                ++failures;
            }
            else
            {
                Drain();
                // Compared over the WHOLE buffer, padding included: a write
                // outside the window is as much a failure as a wrong pixel.
                if (!Report("offset window, padded rows", b.segs.size(),
                            Compare(static_cast<const float*>(db.contents), cpu.data(), cpu.size())))
                    ++failures;
            }

            // And the refusals: a window outside the destination must fail
            // cleanly rather than write past the buffer.
            if (MetalRenderLine((__bridge void*)gQueue, &si, di, dX1 - 1, wy0, wx1, wy1,
                                &b.desc, 1, &err))
            {
                std::printf("  %-30s accepted a window outside dst  FAILED\n", "bounds guard");
                ++failures;
            }
            else
            {
                std::printf("  %-30s refused (%s)  ok\n", "bounds guard", err.c_str());
            }
        }

        // --- timing --------------------------------------------------------------
        // Not a pass/fail check, just a number worth seeing on every run: one
        // UHD frame of a stroke with every effect on.
        {
            Case c = cases[8];   // shadow + arrows + trim
            for (ControlPoint& p : c.stroke.pts) { p.pos = p.pos * 15.0f; p.tan = p.tan * 15.0f; }
            c.style.halfWidth *= 6.0f; c.style.shadowDx *= 6.0f; c.style.shadowDy *= 6.0f;
            c.style.glow = true; c.style.glowRadius = 60.0f;
            c.style.outline = true; c.style.outlineWidth = 10.0f;

            const int W = 3840, H = 2160;
            Built b;
            BuildCase(c, b, 0, 0, W, H);
            id<MTLBuffer> big = [gDev newBufferWithLength:size_t(W) * H * 16
                                                  options:MTLResourceStorageModePrivate];
            MetalImage im; im.buffer = (__bridge void*)big;
            im.x2 = W; im.y2 = H; im.rowBytes = W * 16;

            MetalRenderLine((__bridge void*)gQueue, &im, im, 0, 0, W, H, &b.desc, 1, &err);
            Drain();   // warm
            const int kRuns = 10;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < kRuns; ++i)
                MetalRenderLine((__bridge void*)gQueue, &im, im, 0, 0, W, H, &b.desc, 1, &err);
            Drain();
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count() / kRuns;
            std::printf("  UHD frame, all effects: %.2f ms on the GPU (segs=%zu)\n", ms, b.segs.size());
        }

        std::printf("metaltest: %d/%zu cases diverged\n", failures, cases.size() + 2);
        return failures == 0 ? 0 : 1;
    }
}
