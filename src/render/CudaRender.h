#pragma once

// Interface to the CUDA render path.
//
// Deliberately free of CUDA headers so the plugin translation unit is compiled by
// the host compiler alone and only the kernel goes through nvcc.

#include "render/GpuScene.h"

namespace bl {

/** @brief Is there a usable CUDA device? Answered once and cached.
 *
 * Cached because it is asked on every frame and the underlying query is not
 * free, and because a machine does not grow a GPU mid-session.
 */
bool CudaAvailable();

/// The scene description is shared with the Metal path; see render/GpuScene.h.
using CudaSceneDesc = GpuSceneDesc;

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

/** @brief Motion-blurred render: average @p sceneCount shutter samples.
 *
 * Each scene is one evaluation of the whole geometry at a different fractional
 * time, so this averages N genuinely different lines rather than smearing one.
 *
 * A count of 1 forwards to CudaRenderLine rather than running a one-sample
 * average, so a frame that is not moving takes a byte-identical path to an
 * un-blurred render.
 */
bool CudaRenderLineBlur(void* streamHandle,
                        const float* srcDev, int srcRowStrideFloats,
                        float*       dstDev, int dstRowStrideFloats,
                        int x0, int y0, int x1, int y1,
                        const CudaSceneDesc* scenes, int sceneCount);

} // namespace bl
