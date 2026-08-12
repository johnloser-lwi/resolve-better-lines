#include "render/CudaRender.h"

#include <cuda_runtime.h>

// This file contains NO maths of its own. It includes core/Raster.h and calls
// the same ShadePixel the CPU path calls, compiled here as device code via the
// BL_HD macro. That is the entire argument for keeping src/core header-only:
// there is one implementation, so the two paths cannot drift apart. tools/cudatest
// is what holds us to it.

namespace bl {

namespace {

/** RAII for a stream-ordered device allocation.
 *
 * cudaMallocAsync rather than cudaMalloc: the plain version synchronises the
 * device, which costs tens of microseconds on a path that runs every frame.
 * The async pool is stream-ordered, so the allocation, the copy into it, the
 * kernel that reads it and the free all queue behind one another with no host
 * involvement.
 */
struct DeviceBuffer
{
    void*        p      = nullptr;
    cudaStream_t stream = nullptr;

    bool Alloc(size_t bytes, cudaStream_t s)
    {
        stream = s;
        if (bytes == 0) { p = nullptr; return true; }
        return cudaMallocAsync(&p, bytes, s) == cudaSuccess;
    }
    ~DeviceBuffer() { if (p) cudaFreeAsync(p, stream); }

    DeviceBuffer() {}
    DeviceBuffer(const DeviceBuffer&)            = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
};

__global__ void LineKernel(const float* src, int srcStride,
                           float* dst, int dstStride,
                           int w, int h, int x0, int y0,
                           SceneView scene)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    float px[4];
    if (src)
    {
        const float* s = src + static_cast<ptrdiff_t>(y) * srcStride + x * 4;
        px[0] = s[0]; px[1] = s[1]; px[2] = s[2]; px[3] = s[3];
    }
    else
    {
        px[0] = px[1] = px[2] = px[3] = 0.0f;
    }

    // +0.5 samples the pixel CENTRE, and the offsets put the sample in the same
    // absolute frame coordinates the tile grid was binned in. Getting either
    // wrong shifts the GPU result against the CPU one by half a pixel or a whole
    // tile, which is exactly what the parity test is looking for.
    const Premul line = ShadePixel(scene,
                                   static_cast<float>(x0 + x) + 0.5f,
                                   static_cast<float>(y0 + y) + 0.5f);
    if (line.a > 0.0f) Over(line, px);

    float* d = dst + static_cast<ptrdiff_t>(y) * dstStride + x * 4;
    d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
}

/** @brief One shutter sample, added into the accumulator.
 *
 * Deliberately ADDS premultiplied colour rather than compositing over. Motion
 * blur is the AVERAGE of the shutter samples, and compositing N copies at 1/N
 * opacity is a different and wrong operation: over-compositing gives
 * 1-(1-a/N)^N, so a solid line standing still would come out at 63% opacity
 * rather than 100%.
 *
 * The source frame is not read here at all -- it is composited once at the end
 * by CompositeKernel. Averaging N copies of the background along with the line
 * would be pointless work, and wrong the moment the samples disagree about it.
 */
__global__ void AccumKernel(float* accum, int w, int h, int x0, int y0,
                            SceneView scene, float weight)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const Premul line = ShadePixel(scene,
                                   static_cast<float>(x0 + x) + 0.5f,
                                   static_cast<float>(y0 + y) + 0.5f);

    float* a = accum + (static_cast<ptrdiff_t>(y) * w + x) * 4;
    a[0] += line.r * weight;
    a[1] += line.g * weight;
    a[2] += line.b * weight;
    a[3] += line.a * weight;
}

/** @brief Composite the averaged line over the source, once. */
__global__ void CompositeKernel(const float* src, int srcStride,
                                float* dst, int dstStride,
                                const float* accum, int w, int h)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    float px[4];
    if (src)
    {
        const float* s = src + static_cast<ptrdiff_t>(y) * srcStride + x * 4;
        px[0] = s[0]; px[1] = s[1]; px[2] = s[2]; px[3] = s[3];
    }
    else
    {
        px[0] = px[1] = px[2] = px[3] = 0.0f;
    }

    const float* a = accum + (static_cast<ptrdiff_t>(y) * w + x) * 4;
    Premul line;
    line.r = a[0]; line.g = a[1]; line.b = a[2]; line.a = a[3];
    if (line.a > 0.0f) Over(line, px);

    float* d = dst + static_cast<ptrdiff_t>(y) * dstStride + x * 4;
    d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; d[3] = px[3];
}

/** @brief The seven device buffers one scene needs, and their upload.
 *
 * Factored out because the blur path uploads a scene per shutter sample. RAII on
 * the caller's stack, so a scene stays resident exactly as long as the launches
 * that read it.
 */
struct SceneUpload
{
    DeviceBuffer segs, start, items, fill, bandStart, bandItems, styles;

    bool Build(const CudaSceneDesc& scene, cudaStream_t stream, SceneView& view)
    {
        const size_t segBytes   = sizeof(Segment)  * static_cast<size_t>(scene.segCount);
        const size_t startBytes = sizeof(int)      * static_cast<size_t>(scene.tileStartCount);
        const size_t itemBytes  = sizeof(int)      * static_cast<size_t>(scene.tileItemCount);
        const size_t fillBytes  = sizeof(FillEdge) * static_cast<size_t>(scene.fillCount);
        const size_t bsBytes    = sizeof(int)      * static_cast<size_t>(scene.bandStartCount);
        const size_t biBytes    = sizeof(int)      * static_cast<size_t>(scene.bandItemCount);
        const size_t styBytes   = sizeof(Style)    * static_cast<size_t>(scene.styleCount);

        // A scene with no styles has nothing to shade with; the kernel would read
        // index 0 of a null array on every pixel.
        if (styBytes == 0 || !scene.styles) return false;

        if (!segs.Alloc(segBytes, stream))      return false;
        if (!start.Alloc(startBytes, stream))   return false;
        if (!items.Alloc(itemBytes, stream))    return false;
        if (!fill.Alloc(fillBytes, stream))     return false;
        if (!bandStart.Alloc(bsBytes, stream))  return false;
        if (!bandItems.Alloc(biBytes, stream))  return false;
        if (!styles.Alloc(styBytes, stream))    return false;

        if (segBytes   && cudaMemcpyAsync(segs.p, scene.segs, segBytes,
                                          cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
        if (startBytes && cudaMemcpyAsync(start.p, scene.tileStart, startBytes,
                                          cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
        if (itemBytes  && cudaMemcpyAsync(items.p, scene.tileItems, itemBytes,
                                          cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
        if (fillBytes  && cudaMemcpyAsync(fill.p, scene.fill, fillBytes,
                                          cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
        if (bsBytes    && cudaMemcpyAsync(bandStart.p, scene.bandStart, bsBytes,
                                          cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
        if (biBytes    && cudaMemcpyAsync(bandItems.p, scene.bandItems, biBytes,
                                          cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
        if (cudaMemcpyAsync(styles.p, scene.styles, styBytes,
                            cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;

        // The sources of those copies are ordinary pageable std::vector storage
        // owned by the caller's stack frame, which dies as soon as render()
        // returns. An async copy still reading it then is a use-after-free that
        // shows up as intermittently corrupted geometry rather than a crash, so
        // the host waits until the uploads have actually happened. Cheap in
        // practice: tens of kilobytes, not the frame.
        if (cudaStreamSynchronize(stream) != cudaSuccess) return false;

        view.segs       = static_cast<const Segment*>(segs.p);
        view.segCount   = scene.segCount;
        view.grid       = scene.gridMeta;
        view.grid.start = static_cast<const int*>(start.p);
        view.grid.items = static_cast<const int*>(items.p);

        view.fill        = static_cast<const FillEdge*>(fill.p);
        view.fillCount   = scene.fillCount;
        view.bands       = scene.bandMeta;
        view.bands.start = static_cast<const int*>(bandStart.p);
        view.bands.items = static_cast<const int*>(bandItems.p);

        view.styles     = static_cast<const Style*>(styles.p);
        view.styleCount = scene.styleCount;
        return true;
    }
};

} // namespace

bool CudaAvailable()
{
    static const bool ok = []() {
        int n = 0;
        return cudaGetDeviceCount(&n) == cudaSuccess && n > 0;
    }();
    return ok;
}

bool CudaRenderLine(void* streamHandle,
                    const float* srcDev, int srcRowStrideFloats,
                    float*       dstDev, int dstRowStrideFloats,
                    int x0, int y0, int x1, int y1,
                    const CudaSceneDesc& scene)
{
    if (!dstDev) return false;

    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w <= 0 || h <= 0) return true;      // nothing asked for is not a failure

    cudaStream_t stream = static_cast<cudaStream_t>(streamHandle);

    SceneUpload up;
    SceneView view;
    if (!up.Build(scene, stream, view)) return false;

    const dim3 block(16, 16);
    const dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);

    LineKernel<<<grid, block, 0, stream>>>(srcDev, srcRowStrideFloats,
                                           dstDev, dstRowStrideFloats,
                                           w, h, x0, y0, view);

    if (cudaGetLastError() != cudaSuccess) return false;

    // Declaring setSupportsCudaStream(true) is a promise that we render on the
    // host's stream and that the host will synchronise it. That promise cannot
    // hold if there is no stream: the launch goes to the default stream and
    // returns immediately, so the host is free to read the output buffer while
    // the kernel is still writing it. The result is intermittently torn or stale
    // frames -- the kind of bug that looks like a caching problem for a week.
    if (stream == nullptr && cudaDeviceSynchronize() != cudaSuccess) return false;

    return true;
}

bool CudaRenderLineBlur(void* streamHandle,
                        const float* srcDev, int srcRowStrideFloats,
                        float*       dstDev, int dstRowStrideFloats,
                        int x0, int y0, int x1, int y1,
                        const CudaSceneDesc* scenes, int sceneCount)
{
    if (!dstDev || !scenes || sceneCount < 1) return false;

    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w <= 0 || h <= 0) return true;

    cudaStream_t stream = static_cast<cudaStream_t>(streamHandle);

    // One shutter sample is not a blur, and must take the ordinary path rather
    // than a mathematically-equivalent-but-different one. Averaging a single
    // sample through the accumulator would round-trip the pixel through an extra
    // buffer for nothing, and any float difference it introduced would show up
    // as a parity failure on every frame that is not moving.
    if (sceneCount == 1)
    {
        return CudaRenderLine(streamHandle, srcDev, srcRowStrideFloats,
                              dstDev, dstRowStrideFloats, x0, y0, x1, y1, scenes[0]);
    }

    const size_t accumBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4 * sizeof(float);
    DeviceBuffer accum;
    if (!accum.Alloc(accumBytes, stream)) return false;
    if (cudaMemsetAsync(accum.p, 0, accumBytes, stream) != cudaSuccess) return false;

    const dim3 block(16, 16);
    const dim3 grid((w + block.x - 1) / block.x, (h + block.y - 1) / block.y);
    const float weight = 1.0f / static_cast<float>(sceneCount);

    for (int k = 0; k < sceneCount; ++k)
    {
        // Scoped so each sample's buffers are freed as its launch completes,
        // rather than holding all N scenes resident at once. At 64 samples of a
        // detailed import that difference is the whole GPU memory budget.
        SceneUpload up;
        SceneView   view;
        if (!up.Build(scenes[k], stream, view)) return false;

        AccumKernel<<<grid, block, 0, stream>>>(static_cast<float*>(accum.p),
                                                w, h, x0, y0, view, weight);
        if (cudaGetLastError() != cudaSuccess) return false;

        // The upload's buffers are freed by ~DeviceBuffer at the end of this
        // iteration. cudaFreeAsync is stream-ordered, so the free queues behind
        // the launch above rather than racing it -- which is the only reason
        // scoping them per sample is safe.
        if (cudaStreamSynchronize(stream) != cudaSuccess) return false;
    }

    CompositeKernel<<<grid, block, 0, stream>>>(srcDev, srcRowStrideFloats,
                                                dstDev, dstRowStrideFloats,
                                                static_cast<const float*>(accum.p), w, h);
    if (cudaGetLastError() != cudaSuccess) return false;

    if (stream == nullptr && cudaDeviceSynchronize() != cudaSuccess) return false;

    return true;
}

} // namespace bl
