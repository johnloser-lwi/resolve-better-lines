#pragma once

// Interface to the CUDA render path.
//
// Deliberately free of CUDA headers so the plugin translation unit is compiled by
// the host compiler alone and only the kernel goes through nvcc.

#include "core/Raster.h"

namespace bl {

/** @brief Is there a usable CUDA device? Answered once and cached.
 *
 * Cached because it is asked on every frame and the underlying query is not
 * free, and because a machine does not grow a GPU mid-session.
 */
bool CudaAvailable();

/** @brief Host-side description of one frame's scene, ready to upload. */
struct CudaSceneDesc
{
    const Segment* segs      = nullptr;
    int            segCount  = 0;

    const int*     tileStart = nullptr;   ///< CSR offsets, cols*rows + 1 of them
    int            tileStartCount = 0;
    const int*     tileItems = nullptr;
    int            tileItemCount  = 0;

    TileGridView   gridMeta;              ///< pointers ignored; geometry used

    /// Closed contours for fill, with their own band-binned index.
    const FillEdge* fill      = nullptr;
    int             fillCount = 0;
    const int*      bandStart = nullptr;
    int             bandStartCount = 0;
    const int*      bandItems = nullptr;
    int             bandItemCount  = 0;
    BandGridView    bandMeta;

    /// One style per stroke; index 0 is the effective global style.
    const Style*   styles     = nullptr;
    int            styleCount = 0;
};

/** @brief Composite the line over @p srcDev into @p dstDev, on the GPU.
 *
 * @param stream  the host's CUDA stream, or null. Both pointers are DEVICE
 *                memory: when Resolve renders on the GPU the pixel pointers it
 *                hands a plugin are device allocations, so nothing here may
 *                dereference them on the host.
 * @param srcDev  may be null, meaning an unconnected source clip.
 * @param strides in FLOATS per row, not bytes, and legitimately negative.
 *
 * @return false if anything failed, in which case the destination is untouched.
 */
bool CudaRenderLine(void* stream,
                    const float* srcDev, int srcRowStrideFloats,
                    float*       dstDev, int dstRowStrideFloats,
                    int x0, int y0, int x1, int y1,
                    const CudaSceneDesc& scene);

} // namespace bl
