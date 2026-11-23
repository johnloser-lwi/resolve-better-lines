// The Metal kernels. Compiled at runtime by MetalRender.mm, after the prelude,
// src/core and MetalShared.h have been concatenated in front of this file by
// cmake/EmbedMetal.cmake -- the includes below are for readers and editors; the
// embed step drops them.
//
// Like CudaKernel.cu, this file contains NO maths of its own. It calls the same
// ShadePixel the CPU path calls, so there is one implementation and the two
// paths cannot drift apart. tools/metaltest is what holds us to it.

#include "render/MetalPrelude.metal"
#include "core/Raster.h"
#include "render/MetalShared.h"

using namespace bl;

namespace {

/// Rebuild the SceneView the CPU built, pointing at this dispatch's buffers.
/// An empty array is bound to a placeholder buffer by the host, so the counts
/// -- not the pointers -- say what is really there, and a zero count becomes
/// the null pointer the shared code tests for.
inline SceneView MakeView(constant MetalSceneMeta& m,
                          const device Segment*  segs,
                          const device int*      tileStart,
                          const device int*      tileItems,
                          const device FillEdge* fill,
                          const device int*      bandStart,
                          const device int*      bandItems,
                          const device Style*    styles)
{
    SceneView v;
    v.segs     = m.segCount > 0 ? segs : nullptr;
    v.segCount = m.segCount;

    v.grid.tileSize = m.tileSize;
    v.grid.cols     = m.tileCols;
    v.grid.rows     = m.tileRows;
    v.grid.originX  = m.tileOriginX;
    v.grid.originY  = m.tileOriginY;
    v.grid.start    = m.tileStartCount > 0 ? tileStart : nullptr;
    v.grid.items    = m.tileItemCount  > 0 ? tileItems : nullptr;

    v.fill      = m.fillCount > 0 ? fill : nullptr;
    v.fillCount = m.fillCount;

    v.bands.bandHeight = m.bandHeight;
    v.bands.rows       = m.bandRows;
    v.bands.originY    = m.bandOriginY;
    v.bands.start      = m.bandStartCount > 0 ? bandStart : nullptr;
    v.bands.items      = m.bandItemCount  > 0 ? bandItems : nullptr;

    v.styles     = m.styleCount > 0 ? styles : nullptr;
    v.styleCount = m.styleCount;
    return v;
}

/// The source pixel at absolute (ax, ay), or transparent black outside the
/// source's bounds -- the CPU path's null-row case, made per pixel.
inline void ReadSrc(constant MetalFrameMeta& f, const device float* src,
                    int ax, int ay, thread float* px)
{
    if (f.hasSrc && ax >= f.srcX1 && ax < f.srcX2 && ay >= f.srcY1 && ay < f.srcY2)
    {
        const long i = long(ay - f.srcY1) * long(f.srcStride) + long(ax - f.srcX1) * 4;
        px[0] = src[i + 0]; px[1] = src[i + 1]; px[2] = src[i + 2]; px[3] = src[i + 3];
    }
    else
    {
        px[0] = px[1] = px[2] = px[3] = 0.0f;
    }
}

inline void WriteDst(constant MetalFrameMeta& f, device float* dst,
                     int ax, int ay, const thread float* px)
{
    const long i = long(ay - f.dstY1) * long(f.dstStride) + long(ax - f.dstX1) * 4;
    dst[i + 0] = px[0]; dst[i + 1] = px[1]; dst[i + 2] = px[2]; dst[i + 3] = px[3];
}

} // namespace

#define BL_SCENE_ARGS                                                        \
    constant MetalSceneMeta& scene     [[buffer(kSlotScene)]],               \
    const device Segment*    segs      [[buffer(kSlotSegs)]],                \
    const device int*        tileStart [[buffer(kSlotTileStart)]],           \
    const device int*        tileItems [[buffer(kSlotTileItems)]],           \
    const device FillEdge*   fill      [[buffer(kSlotFill)]],                \
    const device int*        bandStart [[buffer(kSlotBandStart)]],           \
    const device int*        bandItems [[buffer(kSlotBandItems)]],           \
    const device Style*      styles    [[buffer(kSlotStyles)]]

#define BL_SCENE_VIEW \
    MakeView(scene, segs, tileStart, tileItems, fill, bandStart, bandItems, styles)

/// One frame, one scene: shade and composite over the source in a single pass.
kernel void LineKernel(const device float*      src   [[buffer(kSlotSrc)]],
                       device float*            dst   [[buffer(kSlotDst)]],
                       constant MetalFrameMeta& frame [[buffer(kSlotFrame)]],
                       BL_SCENE_ARGS,
                       uint2 gid [[thread_position_in_grid]])
{
    if (int(gid.x) >= frame.winW || int(gid.y) >= frame.winH) return;

    const int ax = frame.winX0 + int(gid.x);
    const int ay = frame.winY0 + int(gid.y);

    float px[4];
    ReadSrc(frame, src, ax, ay, px);

    // +0.5 samples the pixel CENTRE, in the same absolute frame coordinates the
    // tile grid was binned in -- exactly as CudaKernel.cu and the CPU loop do.
    const SceneView view = BL_SCENE_VIEW;
    const Premul line = ShadePixel(view, float(ax) + 0.5f, float(ay) + 0.5f);
    if (line.a > 0.0f) Over(line, px);

    WriteDst(frame, dst, ax, ay, px);
}

/// One shutter sample, ADDED into the accumulator at weight 1/N. Adding, not
/// compositing: motion blur is the average of the samples, and over-compositing
/// N copies at 1/N opacity would leave a still, solid line at 63%. The source is
/// not read here; CompositeKernel lays the average over it once.
kernel void AccumKernel(device float*            accum [[buffer(kSlotAccum)]],
                        constant MetalFrameMeta& frame [[buffer(kSlotFrame)]],
                        BL_SCENE_ARGS,
                        uint2 gid [[thread_position_in_grid]])
{
    if (int(gid.x) >= frame.winW || int(gid.y) >= frame.winH) return;

    const int ax = frame.winX0 + int(gid.x);
    const int ay = frame.winY0 + int(gid.y);

    const SceneView view = BL_SCENE_VIEW;
    const Premul line = ShadePixel(view, float(ax) + 0.5f, float(ay) + 0.5f);

    device float* a = accum + (long(gid.y) * long(frame.winW) + long(gid.x)) * 4;
    a[0] += line.r * frame.weight;
    a[1] += line.g * frame.weight;
    a[2] += line.b * frame.weight;
    a[3] += line.a * frame.weight;
}

/// Composite the averaged line over the source, once.
kernel void CompositeKernel(const device float*      src   [[buffer(kSlotSrc)]],
                            device float*            dst   [[buffer(kSlotDst)]],
                            constant MetalFrameMeta& frame [[buffer(kSlotFrame)]],
                            const device float*      accum [[buffer(kSlotAccum)]],
                            uint2 gid [[thread_position_in_grid]])
{
    if (int(gid.x) >= frame.winW || int(gid.y) >= frame.winH) return;

    const int ax = frame.winX0 + int(gid.x);
    const int ay = frame.winY0 + int(gid.y);

    float px[4];
    ReadSrc(frame, src, ax, ay, px);

    const device float* a = accum + (long(gid.y) * long(frame.winW) + long(gid.x)) * 4;
    Premul line;
    line.r = a[0]; line.g = a[1]; line.b = a[2]; line.a = a[3];
    if (line.a > 0.0f) Over(line, px);

    WriteDst(frame, dst, ax, ay, px);
}
