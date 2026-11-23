#pragma once

// Interface to the Metal render path -- the macOS counterpart of CudaRender.h.
//
// Plain C++ with opaque handles, so BetterLinePlugin.cpp stays a C++ file and
// only MetalRender.mm sees Objective-C.

#include "render/GpuScene.h"

#include <string>

namespace bl {

/** @brief Is there a Metal device at all? Answered once and cached.
 *
 * Gates setSupportsMetalRender: claiming Metal on a machine without it would
 * have Resolve hand us buffers nothing here can service.
 */
bool MetalAvailable();

/** @brief One image as Resolve hands it over on the Metal path. */
struct MetalImage
{
    void* buffer   = nullptr;  ///< id<MTLBuffer>, the image's kOfxImagePropData
    int   x1 = 0, y1 = 0;      ///< bounds, absolute frame pixels; the buffer's
    int   x2 = 0, y2 = 0;      ///  first float is pixel (x1, y1)
    int   rowBytes = 0;        ///< must be positive on this path
};

/** @brief Composite the line over @p src into @p dst, on the GPU.
 *
 * Enqueues on @p cmdQueue (the host's id<MTLCommandQueue>) and returns WITHOUT
 * waiting, as the OFX Metal contract requires: the host orders its own work
 * behind ours on that queue. The scene arrays are copied into Metal buffers
 * before this returns, so the caller's vectors may die immediately.
 *
 * One scene is an ordinary render. More than one is motion blur: the samples
 * are averaged, then composited once. A single sample deliberately takes the
 * one-scene path, so a still frame renders identically with blur on or off.
 *
 * @param src  null for an unconnected source clip.
 * @param err  receives a description on failure; may be null.
 * @return false on any failure, in which case nothing was enqueued.
 */
bool MetalRenderLine(void* cmdQueue,
                     const MetalImage* src, const MetalImage& dst,
                     int x0, int y0, int x1, int y1,
                     const GpuSceneDesc* scenes, int sceneCount,
                     std::string* err = nullptr);

} // namespace bl
