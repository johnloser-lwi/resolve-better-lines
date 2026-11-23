// CPU vs CUDA parity.
//
// src/core is header-only so that both paths compile the same ShadePixel, but
// "same source" is not "same result": nvcc reassociates float arithmetic, and
// -use_fast_math or a stray intrinsic would silently change the answer. In
// Resolve every frame goes through the GPU path, so a divergence that a
// linetest render cannot see is a divergence nobody sees until it ships.
//
// Exit code 0 on agreement, 1 on divergence or CUDA failure.

#include "core/Geometry.h"
#include "core/PathBuild.h"
#include "core/Raster.h"
#include "core/TileGrid.h"
#include "render/CudaRender.h"
#include "ParityCases.h"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bl;

using namespace parity;

int main()
{
    if (!CudaAvailable())
    {
        std::fprintf(stderr, "cudatest: no CUDA device available\n");
        return 1;
    }

    // A non-trivial source image, not black. Compositing "over" black hides a
    // whole class of premultiplication mistakes, because the term that would
    // disagree is multiplied by zero.
    std::vector<float> srcHost(static_cast<size_t>(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            float* p = &srcHost[(static_cast<size_t>(y) * kSize + x) * 4];
            p[0] = static_cast<float>(x) / kSize;
            p[1] = static_cast<float>(y) / kSize;
            p[2] = 0.25f;
            p[3] = 1.0f;
        }

    const size_t pixels = static_cast<size_t>(kSize) * kSize;
    const size_t bytes  = pixels * 4 * sizeof(float);

    float *srcDev = nullptr, *dstDev = nullptr;
    if (cudaMalloc(&srcDev, bytes) != cudaSuccess ||
        cudaMalloc(&dstDev, bytes) != cudaSuccess)
    {
        std::fprintf(stderr, "cudatest: device allocation failed\n");
        return 1;
    }
    cudaMemcpy(srcDev, srcHost.data(), bytes, cudaMemcpyHostToDevice);

    // The tolerance allows for float reassociation between two compilers, and
    // nothing more. It is deliberately far below anything an eye or an 8-bit
    // export could resolve, so a real behavioural difference cannot hide under it.
    //
    // Measured worst across these cases is 2.3e-6, so this leaves roughly a
    // factor of two. Set at 1e-5 it was loose enough to have hidden the cap-plane
    // cancellation bug that this suite was written to find -- which is the whole
    // argument for keeping it tight rather than comfortable.
    const float kTol = 5e-6f;

    // Two further allowances, and they exist for one specific, understood effect
    // rather than to make the suite quiet.
    //
    // Gradient coordinates are taken from the nearest primitive. Where an
    // arrowhead overlaps its own shaft both contain the pixel, their coordinate
    // frames genuinely differ, and which one is nearest is decided by a
    // difference of about 1e-7 -- so a thin locus of pixels lands on either side
    // depending on how each compiler rounds. Measured: 53 samples of 65536,
    // about 13 pixels, worst 1.2e-2 in one channel, which is under half a step
    // of 8-bit output.
    //
    // It cannot be designed away by removing the overlap. Two SDF shapes that
    // merely touch have d = 0 along the join, which is HALF coverage -- a
    // visible crack across the shape. The overlap is load-bearing.
    //
    // So: a handful of boundary samples with sub-perceptual error passes, and
    // anything wider or larger still fails loudly. A real difference in the
    // maths moves far more than a quarter of a percent of the frame.
    const double kMaxBadFraction = 0.0025;
    const float  kAbsCeiling     = 2.5e-2f;

    int failures = 0;
    std::vector<Case> cases = MakeCases();

    for (const Case& c : cases)
    {
        Polyline poly;
        FlattenStroke(c.stroke, 0.2f, poly);

        std::vector<FillEdge> fill;
        if (c.style.fill && c.stroke.closed) AppendFillContour(poly.pts, 0, fill);

        std::vector<Segment> segs;
        BuildStrokeSegments(poly, c.style, c.trim, c.stroke.closed,
                            c.dashLen, c.gapLen, 0.0f, segs);

        TileGrid grid;
        BuildTileGrid(segs, c.style.MaxReach() + 1.0f, 0, 0, kSize, kSize, 16, grid);

        BandGrid bands;
        BuildBandGrid(fill, c.style.feather + 1.0f, 0, kSize, 16, bands);

        SceneView scene;
        scene.segs      = segs.empty() ? nullptr : segs.data();
        scene.segCount  = static_cast<int>(segs.size());
        scene.grid      = grid.View();
        scene.fill      = fill.empty() ? nullptr : fill.data();
        scene.fillCount = static_cast<int>(fill.size());
        scene.bands      = bands.View();
        scene.styles     = &c.style;
        scene.styleCount = 1;

        // --- CPU ---
        std::vector<float> cpu(srcHost);
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
            {
                const Premul line = ShadePixel(scene, x + 0.5f, y + 0.5f);
                if (line.a > 0.0f) Over(line, &cpu[(static_cast<size_t>(y) * kSize + x) * 4]);
            }

        // --- GPU ---
        cudaMemcpy(dstDev, srcDev, bytes, cudaMemcpyDeviceToDevice);

        CudaSceneDesc up;
        up.segs           = scene.segs;
        up.segCount       = scene.segCount;
        up.tileStart      = grid.start.empty() ? nullptr : grid.start.data();
        up.tileStartCount = static_cast<int>(grid.start.size());
        up.tileItems      = grid.items.empty() ? nullptr : grid.items.data();
        up.tileItemCount  = static_cast<int>(grid.items.size());
        up.gridMeta       = grid.View();
        up.fill           = scene.fill;
        up.fillCount      = scene.fillCount;
        up.bandStart      = bands.start.empty() ? nullptr : bands.start.data();
        up.bandStartCount = static_cast<int>(bands.start.size());
        up.bandItems      = bands.items.empty() ? nullptr : bands.items.data();
        up.bandItemCount  = static_cast<int>(bands.items.size());
        up.bandMeta       = bands.View();
        up.styles         = &c.style;
        up.styleCount     = 1;

        // Null stream on purpose: it exercises the branch that has to
        // cudaDeviceSynchronize, which is the one that produces torn frames when
        // it is missing.
        if (!CudaRenderLine(nullptr, srcDev, kSize * 4, dstDev, kSize * 4,
                            0, 0, kSize, kSize, up))
        {
            std::printf("  %-30s CUDA RENDER FAILED\n", c.name);
            ++failures;
            continue;
        }

        std::vector<float> gpu(pixels * 4);
        cudaMemcpy(gpu.data(), dstDev, bytes, cudaMemcpyDeviceToHost);

        double worst = 0.0;
        size_t worstAt = 0;
        size_t bad = 0;
        for (size_t i = 0; i < gpu.size(); ++i)
        {
            const double d = std::fabs(static_cast<double>(gpu[i]) - cpu[i]);
            if (d > worst) { worst = d; worstAt = i; }
            if (d > kTol) ++bad;
        }

        // The COUNT separates the two things a divergence can mean. A handful of
        // pixels is an instability at some boundary -- a tie between primitives,
        // a value straddling a clamp -- which is inherent and localised.
        // Thousands means the two paths are computing genuinely different maths.
        const double badFraction = static_cast<double>(bad) / static_cast<double>(gpu.size());
        const bool ok = (worst <= kTol) ||
                        (badFraction <= kMaxBadFraction && worst <= kAbsCeiling);

        std::printf("  %-30s segs=%4zu  maxdiff=%.3e  bad=%zu (%.3f%%)  %s\n",
                    c.name, segs.size(), worst, bad, badFraction * 100.0,
                    ok ? (worst <= kTol ? "ok" : "ok (boundary)") : "DIVERGED");

        if (!ok)
        {
            const size_t px = worstAt / 4;
            std::printf("      worst at (%zu,%zu) ch%zu: cpu=%.9f gpu=%.9f\n",
                        px % kSize, px / kSize, worstAt % 4,
                        cpu[worstAt], gpu[worstAt]);
            ++failures;
        }
    }

    // --- motion blur -------------------------------------------------------
    //
    // The blur path is a DIFFERENT pair of kernels -- accumulate, then composite
    // once -- rather than the single LineKernel every case above exercises. None
    // of those would catch a fault in it, because they all run one sample.
    {
        const int kSamples = 4;

        // Four translated copies of one stroke, standing in for four shutter
        // times. What varies between samples does not matter to the accumulator;
        // that they genuinely differ does.
        std::vector<std::vector<Segment> >  segsN(kSamples);
        std::vector<std::vector<FillEdge> > fillN(kSamples);
        std::vector<TileGrid>  gridN(kSamples);
        std::vector<BandGrid>  bandN(kSamples);
        std::vector<SceneView> viewN(kSamples);
        std::vector<CudaSceneDesc> upN(kSamples);

        Style st;
        st.halfWidth = 9.0f; st.feather = 1.2f;
        st.color = Color(1.0f, 0.85f, 0.3f, 1.0f);

        for (int k = 0; k < kSamples; ++k)
        {
            const float dx = static_cast<float>(k) * 11.0f;
            Stroke sk;
            sk.pts = { CP(40 + dx, 70, 70, 60), CP(150 + dx, 200, 50, 0),
                       CP(215 + dx, 70, 0, -60) };

            Polyline poly;
            FlattenStroke(sk, 0.2f, poly);
            TrimState trim;
            BuildStrokeSegments(poly, st, trim, false, 0.0f, 0.0f, 0.0f, segsN[k]);

            BuildTileGrid(segsN[k], st.MaxReach() + 1.0f, 0, 0, kSize, kSize, 16, gridN[k]);
            BuildBandGrid(fillN[k], st.feather + 1.0f, 0, kSize, 16, bandN[k]);

            SceneView v;
            v.segs = segsN[k].empty() ? nullptr : segsN[k].data();
            v.segCount = static_cast<int>(segsN[k].size());
            v.grid = gridN[k].View();
            v.fill = nullptr; v.fillCount = 0;
            v.bands = bandN[k].View();
            v.styles = &st; v.styleCount = 1;
            viewN[k] = v;

            CudaSceneDesc u;
            u.segs = v.segs; u.segCount = v.segCount;
            u.tileStart = gridN[k].start.empty() ? nullptr : gridN[k].start.data();
            u.tileStartCount = static_cast<int>(gridN[k].start.size());
            u.tileItems = gridN[k].items.empty() ? nullptr : gridN[k].items.data();
            u.tileItemCount = static_cast<int>(gridN[k].items.size());
            u.gridMeta = gridN[k].View();
            u.fill = nullptr; u.fillCount = 0;
            u.bandStart = bandN[k].start.empty() ? nullptr : bandN[k].start.data();
            u.bandStartCount = static_cast<int>(bandN[k].start.size());
            u.bandItems = bandN[k].items.empty() ? nullptr : bandN[k].items.data();
            u.bandItemCount = static_cast<int>(bandN[k].items.size());
            u.bandMeta = bandN[k].View();
            u.styles = &st; u.styleCount = 1;
            upN[k] = u;
        }

        std::vector<float> cpu(srcHost);
        const float weight = 1.0f / static_cast<float>(kSamples);
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
            {
                Premul acc; acc.r = acc.g = acc.b = acc.a = 0.0f;
                for (int k = 0; k < kSamples; ++k)
                {
                    const Premul s2 = ShadePixel(viewN[k],
                                                 static_cast<float>(x) + 0.5f,
                                                 static_cast<float>(y) + 0.5f);
                    acc.r += s2.r * weight; acc.g += s2.g * weight;
                    acc.b += s2.b * weight; acc.a += s2.a * weight;
                }
                if (acc.a > 0.0f)
                    Over(acc, &cpu[(static_cast<size_t>(y) * kSize + x) * 4]);
            }

        cudaMemcpy(srcDev, srcHost.data(), bytes, cudaMemcpyHostToDevice);
        if (!CudaRenderLineBlur(nullptr, srcDev, kSize * 4, dstDev, kSize * 4,
                                0, 0, kSize, kSize, upN.data(), kSamples))
        {
            std::printf("  %-30s CUDA BLUR RENDER FAILED\n", "motion blur x4");
            ++failures;
        }
        else
        {
            std::vector<float> gpu(pixels * 4);
            cudaMemcpy(gpu.data(), dstDev, bytes, cudaMemcpyDeviceToHost);

            double worst = 0.0; size_t bad = 0;
            for (size_t i = 0; i < gpu.size(); ++i)
            {
                const double d = std::fabs(double(gpu[i]) - double(cpu[i]));
                if (d > worst) worst = d;
                if (d > kTol) ++bad;
            }
            const bool ok = (worst <= kTol) ||
                            (double(bad) / double(gpu.size()) <= kMaxBadFraction &&
                             worst <= kAbsCeiling);
            std::printf("  %-30s segs=%4zu  maxdiff=%.3e  bad=%zu  %s\n",
                        "motion blur x4", segsN[0].size(), worst, bad,
                        ok ? "ok" : "DIVERGED");
            if (!ok) ++failures;
        }
    }

    cudaFree(srcDev);
    cudaFree(dstDev);

    std::printf("cudatest: %d/%zu cases diverged\n", failures, cases.size());
    return failures == 0 ? 0 : 1;
}
