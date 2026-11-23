#include "render/MetalRender.h"
#include "render/MetalShared.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// The shader source: MetalPrelude.metal + src/core + MetalShared.h +
// MetalKernels.metal, concatenated at build time by cmake/EmbedMetal.cmake.
#include "MetalSource.inc"

// Like CudaKernel.cu, this file contains no maths. It moves data and launches
// kernels; ShadePixel itself lives in src/core, compiled here by the Metal
// compiler from the very same headers the CPU build uses.

namespace bl {

namespace {

/** Compiled pipelines for one GPU. Built once per device and kept for the life
 *  of the process: compiling the shader takes a noticeable fraction of a second,
 *  and Resolve renders thousands of frames on the same device. */
struct Pipelines
{
    id<MTLComputePipelineState> line;
    id<MTLComputePipelineState> accum;
    id<MTLComputePipelineState> composite;

    /// Bound in place of an empty array. Metal wants every slot a kernel
    /// declares to hold a buffer; the kernel reads the counts, never this.
    id<MTLBuffer> placeholder;

    bool        failed = false;
    std::string error;
};

std::mutex                                 gPipeMutex;
std::unordered_map<uint64_t, Pipelines>    gPipes;   // keyed by registryID

id<MTLComputePipelineState> MakePipeline(id<MTLDevice> dev, id<MTLLibrary> lib,
                                         NSString* name, std::string& err)
{
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn)
    {
        err = std::string("Metal kernel not found: ") + name.UTF8String;
        return nil;
    }
    NSError* e = nil;
    id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:fn error:&e];
    if (!p) err = std::string("Metal pipeline failed: ") + e.localizedDescription.UTF8String;
    return p;
}

/** The pipelines for @p dev, compiling them on first use. Thread-safe: Resolve
 *  may render several frames at once. A failed compile is remembered, so a
 *  broken shader costs one attempt rather than one per frame. */
const Pipelines& PipelinesFor(id<MTLDevice> dev)
{
    std::lock_guard<std::mutex> lock(gPipeMutex);

    Pipelines& p = gPipes[dev.registryID];
    if (p.line || p.failed) return p;

    MTLCompileOptions* opts = [MTLCompileOptions new];
    // Fast maths OFF. With it on, the compiler may substitute approximate
    // transcendentals and reassociate freely -- the CPU and GPU would then be
    // computing different things from the same source. (The prelude also
    // routes the C maths names to metal::precise, so this is belt and braces.)
    if (@available(macOS 15.0, *))
    {
        opts.mathMode = MTLMathModeSafe;
    }
    else
    {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }

    NSError* e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kBetterLineMetalSource)
                                           options:opts
                                             error:&e];
    if (!lib)
    {
        p.failed = true;
        p.error  = std::string("Metal shader compile failed: ") +
                   (e ? e.localizedDescription.UTF8String : "unknown error");
        return p;
    }

    std::string err;
    p.line      = MakePipeline(dev, lib, @"LineKernel", err);
    p.accum     = p.line  ? MakePipeline(dev, lib, @"AccumKernel", err)     : nil;
    p.composite = p.accum ? MakePipeline(dev, lib, @"CompositeKernel", err) : nil;
    p.placeholder = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];

    if (!p.line || !p.accum || !p.composite || !p.placeholder)
    {
        p.line = p.accum = p.composite = nil;
        p.failed = true;
        p.error  = err.empty() ? "Metal setup failed" : err;
    }
    return p;
}

/** Copy one host array into a fresh shared buffer, or bind the placeholder.
 *
 *  newBufferWithBytes copies synchronously, which is the point: the arrays are
 *  std::vectors on render()'s stack and die the moment it returns, long before
 *  the GPU runs. The command buffer retains the Metal buffer itself until the
 *  work that reads it has completed, so nothing here has to outlive the call. */
id<MTLBuffer> Upload(id<MTLDevice> dev, const void* data, size_t bytes, id<MTLBuffer> placeholder)
{
    if (!data || bytes == 0) return placeholder;
    return [dev newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
}

/** Upload one scene and bind it, plus its parameter block, to @p enc. */
bool BindScene(id<MTLComputeCommandEncoder> enc, id<MTLDevice> dev,
               const GpuSceneDesc& s, id<MTLBuffer> placeholder)
{
    // A scene with no styles has nothing to shade with; the shared code would
    // index a null array on every pixel. Same rule as the CUDA path.
    if (!s.styles || s.styleCount <= 0) return false;

    MetalSceneMeta m;
    m.segCount       = s.segCount;
    m.fillCount      = s.fillCount;
    m.styleCount     = s.styleCount;
    m.tileStartCount = s.tileStartCount;
    m.tileItemCount  = s.tileItemCount;
    m.bandStartCount = s.bandStartCount;
    m.bandItemCount  = s.bandItemCount;
    m.tileSize       = s.gridMeta.tileSize;
    m.tileCols       = s.gridMeta.cols;
    m.tileRows       = s.gridMeta.rows;
    m.tileOriginX    = s.gridMeta.originX;
    m.tileOriginY    = s.gridMeta.originY;
    m.bandHeight     = s.bandMeta.bandHeight;
    m.bandRows       = s.bandMeta.rows;
    m.bandOriginY    = s.bandMeta.originY;

    id<MTLBuffer> b[7] = {
        Upload(dev, s.segs,      sizeof(Segment)  * size_t(s.segCount),       placeholder),
        Upload(dev, s.tileStart, sizeof(int)      * size_t(s.tileStartCount), placeholder),
        Upload(dev, s.tileItems, sizeof(int)      * size_t(s.tileItemCount),  placeholder),
        Upload(dev, s.fill,      sizeof(FillEdge) * size_t(s.fillCount),      placeholder),
        Upload(dev, s.bandStart, sizeof(int)      * size_t(s.bandStartCount), placeholder),
        Upload(dev, s.bandItems, sizeof(int)      * size_t(s.bandItemCount),  placeholder),
        Upload(dev, s.styles,    sizeof(Style)    * size_t(s.styleCount),     placeholder),
    };
    for (id<MTLBuffer> x : b) if (!x) return false;

    [enc setBytes:&m length:sizeof(m) atIndex:kSlotScene];
    [enc setBuffer:b[0] offset:0 atIndex:kSlotSegs];
    [enc setBuffer:b[1] offset:0 atIndex:kSlotTileStart];
    [enc setBuffer:b[2] offset:0 atIndex:kSlotTileItems];
    [enc setBuffer:b[3] offset:0 atIndex:kSlotFill];
    [enc setBuffer:b[4] offset:0 atIndex:kSlotBandStart];
    [enc setBuffer:b[5] offset:0 atIndex:kSlotBandItems];
    [enc setBuffer:b[6] offset:0 atIndex:kSlotStyles];
    return true;
}

/// Bytes an image's buffer must hold for its bounds, so a host buffer that is
/// smaller than its own description fails here instead of as a GPU fault.
size_t BytesNeeded(const MetalImage& im)
{
    const long rows = im.y2 - im.y1;
    const long cols = im.x2 - im.x1;
    if (rows <= 0 || cols <= 0) return 0;
    return size_t((rows - 1) * long(im.rowBytes) + cols * 4 * long(sizeof(float)));
}

bool ValidImage(const MetalImage& im, std::string& why, const char* which)
{
    if (!im.buffer)       { why = std::string(which) + " buffer is null"; return false; }
    if (im.rowBytes <= 0 || im.rowBytes % 4 != 0)
    {
        why = std::string(which) + " row stride is not a positive float multiple";
        return false;
    }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)im.buffer;
    if (b.length < BytesNeeded(im))
    {
        why = std::string(which) + " buffer is smaller than its bounds";
        return false;
    }
    return true;
}

} // namespace

bool MetalAvailable()
{
    static const bool ok = []() {
        @autoreleasepool { return MTLCreateSystemDefaultDevice() != nil; }
    }();
    return ok;
}

bool MetalRenderLine(void* cmdQueue,
                     const MetalImage* src, const MetalImage& dst,
                     int x0, int y0, int x1, int y1,
                     const GpuSceneDesc* scenes, int sceneCount,
                     std::string* errOut)
{
    std::string scratch;
    std::string& err = errOut ? *errOut : scratch;

    @autoreleasepool
    {
        if (!cmdQueue)                      { err = "no Metal command queue"; return false; }
        if (!scenes || sceneCount < 1)      { err = "no scene"; return false; }
        if (!ValidImage(dst, err, "destination")) return false;
        if (src && !ValidImage(*src, err, "source")) return false;

        const int w = x1 - x0;
        const int h = y1 - y0;
        if (w <= 0 || h <= 0) return true;  // nothing asked for is not a failure

        // The render window must lie inside the destination, or the kernel's
        // writes would land outside the buffer's rows.
        if (x0 < dst.x1 || y0 < dst.y1 || x1 > dst.x2 || y1 > dst.y2)
        {
            err = "render window outside destination bounds";
            return false;
        }

        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)cmdQueue;
        id<MTLDevice>       dev   = queue.device;

        const Pipelines& pipes = PipelinesFor(dev);
        if (pipes.failed) { err = pipes.error; return false; }

        MetalFrameMeta f;
        f.winX0 = x0; f.winY0 = y0; f.winW = w; f.winH = h;
        f.dstX1 = dst.x1; f.dstY1 = dst.y1;
        f.dstStride = dst.rowBytes / int(sizeof(float));
        f.hasSrc = src ? 1 : 0;
        f.srcX1 = src ? src->x1 : 0; f.srcY1 = src ? src->y1 : 0;
        f.srcX2 = src ? src->x2 : 0; f.srcY2 = src ? src->y2 : 0;
        f.srcStride = src ? src->rowBytes / int(sizeof(float)) : 0;
        f.weight = 1.0f;

        id<MTLBuffer> dstBuf = (__bridge id<MTLBuffer>)dst.buffer;
        id<MTLBuffer> srcBuf = src ? (__bridge id<MTLBuffer>)src->buffer : pipes.placeholder;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (!cb) { err = "could not create a Metal command buffer"; return false; }
        cb.label = @"BetterLine";

        // 2D threadgroups of one SIMD-group's width. Dispatching whole groups
        // and bounds-checking in the kernel works on every Mac GPU, including
        // ones without non-uniform threadgroup support.
        auto groupFor = [](id<MTLComputePipelineState> p) {
            const NSUInteger tw = p.threadExecutionWidth;
            const NSUInteger th = std::max<NSUInteger>(1, std::min<NSUInteger>(
                8, p.maxTotalThreadsPerThreadgroup / tw));
            return MTLSizeMake(tw, th, 1);
        };
        auto gridFor = [w, h](MTLSize g) {
            return MTLSizeMake((NSUInteger(w) + g.width - 1) / g.width,
                               (NSUInteger(h) + g.height - 1) / g.height, 1);
        };

        if (sceneCount == 1)
        {
            // One sample is not a blur, and takes the ordinary path rather than
            // a mathematically equal but differently rounded one -- the same
            // rule as the CPU loop and the CUDA path.
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pipes.line];
            [enc setBuffer:srcBuf offset:0 atIndex:kSlotSrc];
            [enc setBuffer:dstBuf offset:0 atIndex:kSlotDst];
            [enc setBytes:&f length:sizeof(f) atIndex:kSlotFrame];
            if (!BindScene(enc, dev, scenes[0], pipes.placeholder))
            {
                [enc endEncoding];
                err = "scene upload failed";
                return false;   // the command buffer is never committed
            }
            const MTLSize g = groupFor(pipes.line);
            [enc dispatchThreadgroups:gridFor(g) threadsPerThreadgroup:g];
            [enc endEncoding];
        }
        else
        {
            const NSUInteger accumBytes = NSUInteger(w) * NSUInteger(h) * 4 * sizeof(float);
            id<MTLBuffer> accum = [dev newBufferWithLength:accumBytes
                                                   options:MTLResourceStorageModePrivate];
            if (!accum) { err = "could not allocate the motion-blur accumulator"; return false; }

            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit fillBuffer:accum range:NSMakeRange(0, accumBytes) value:0];
            [blit endEncoding];

            // One encoder for every sample. A compute encoder's dispatches run
            // in order by default, so each accumulation sees the last one's
            // writes. Every sample's buffers stay resident until the command
            // buffer completes -- tens of kilobytes each, unlike the frame.
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pipes.accum];
            [enc setBuffer:accum offset:0 atIndex:kSlotAccum];
            f.weight = 1.0f / float(sceneCount);
            [enc setBytes:&f length:sizeof(f) atIndex:kSlotFrame];

            const MTLSize ga = groupFor(pipes.accum);
            for (int k = 0; k < sceneCount; ++k)
            {
                if (!BindScene(enc, dev, scenes[k], pipes.placeholder))
                {
                    [enc endEncoding];
                    err = "scene upload failed";
                    return false;
                }
                [enc dispatchThreadgroups:gridFor(ga) threadsPerThreadgroup:ga];
            }

            f.weight = 1.0f;
            [enc setComputePipelineState:pipes.composite];
            [enc setBuffer:srcBuf offset:0 atIndex:kSlotSrc];
            [enc setBuffer:dstBuf offset:0 atIndex:kSlotDst];
            [enc setBytes:&f length:sizeof(f) atIndex:kSlotFrame];
            const MTLSize gc = groupFor(pipes.composite);
            [enc dispatchThreadgroups:gridFor(gc) threadsPerThreadgroup:gc];
            [enc endEncoding];
        }

        // Committed, NOT waited on. The OFX Metal contract says the plugin
        // should not wait: the host puts its own reads of dst behind this on
        // the same queue, which is what orders them.
        [cb commit];
        return true;
    }
}

} // namespace bl
