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

    DeviceBuffer dSegs, dStart, dItems, dFill, dBandStart, dBandItems, dStyles;
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

    if (!dSegs.Alloc(segBytes, stream))       return false;
    if (!dStart.Alloc(startBytes, stream))    return false;
    if (!dItems.Alloc(itemBytes, stream))     return false;
    if (!dFill.Alloc(fillBytes, stream))      return false;
    if (!dBandStart.Alloc(bsBytes, stream))   return false;
    if (!dBandItems.Alloc(biBytes, stream))   return false;
    if (!dStyles.Alloc(styBytes, stream))     return false;

    if (segBytes   && cudaMemcpyAsync(dSegs.p,  scene.segs,      segBytes,
                                      cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
    if (startBytes && cudaMemcpyAsync(dStart.p, scene.tileStart, startBytes,
                                      cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
    if (itemBytes  && cudaMemcpyAsync(dItems.p, scene.tileItems, itemBytes,
                                      cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
    if (fillBytes  && cudaMemcpyAsync(dFill.p,  scene.fill,      fillBytes,
                                      cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
    if (bsBytes    && cudaMemcpyAsync(dBandStart.p, scene.bandStart, bsBytes,
                                      cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
    if (biBytes    && cudaMemcpyAsync(dBandItems.p, scene.bandItems, biBytes,
                                      cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;
    if (cudaMemcpyAsync(dStyles.p, scene.styles, styBytes,
                        cudaMemcpyHostToDevice, stream) != cudaSuccess) return false;

    // The sources of those copies are ordinary pageable std::vector storage owned
    // by the caller's stack frame, which dies as soon as render() returns. An
    // async copy still reading it at that point is a use-after-free that shows up
    // as intermittently corrupted geometry rather than a crash, so the host waits
    // here until the uploads have actually happened.
    //
    // Cheap in practice: this is a few tens of kilobytes, not the frame.
    if (cudaStreamSynchronize(stream) != cudaSuccess) return false;

    SceneView view;
    view.segs      = static_cast<const Segment*>(dSegs.p);
    view.segCount  = scene.segCount;
    view.grid      = scene.gridMeta;
    view.grid.start = static_cast<const int*>(dStart.p);
    view.grid.items = static_cast<const int*>(dItems.p);

    view.fill      = static_cast<const FillEdge*>(dFill.p);
    view.fillCount = scene.fillCount;
    view.bands       = scene.bandMeta;
    view.bands.start = static_cast<const int*>(dBandStart.p);
    view.bands.items = static_cast<const int*>(dBandItems.p);

    view.styles     = static_cast<const Style*>(dStyles.p);
    view.styleCount = scene.styleCount;

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

} // namespace bl
