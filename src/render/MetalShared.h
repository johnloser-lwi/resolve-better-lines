#pragma once

// The small per-dispatch parameter blocks, shared VERBATIM by the host
// (MetalRender.mm, which fills them with setBytes) and the shader (which reads
// them from `constant` memory). Ints and one float only: no vector types, no
// pointers, nothing whose size or alignment could differ between the two
// compilers, so one definition describes both sides of the copy.

namespace bl {

/** @brief The geometry of one scene's arrays. One per shutter sample. */
struct MetalSceneMeta
{
    int segCount;
    int fillCount;
    int styleCount;

    int tileStartCount;
    int tileItemCount;
    int bandStartCount;
    int bandItemCount;

    // TileGridView, minus its pointers.
    int tileSize;
    int tileCols;
    int tileRows;
    int tileOriginX;
    int tileOriginY;

    // BandGridView, minus its pointers.
    int bandHeight;
    int bandRows;
    int bandOriginY;
};

/** @brief Where the render window sits in the images, and how they are laid out.
 *
 * Every coordinate is in absolute frame pixels. Metal images arrive as whole
 * buffers rather than as pointers to a pixel, so the shader does the
 * addressing getPixelAddress would do on the CPU: offset from the image's
 * bounds origin, by its own row stride.
 */
struct MetalFrameMeta
{
    int winX0;      ///< render window origin
    int winY0;
    int winW;       ///< render window size
    int winH;

    int dstX1;      ///< destination bounds origin
    int dstY1;
    int dstStride;  ///< destination row stride, in FLOATS

    int hasSrc;     ///< 0: unconnected source, composite over transparent black
    int srcX1;      ///< source bounds, [x1,x2) x [y1,y2); outside reads as transparent
    int srcY1;
    int srcX2;
    int srcY2;
    int srcStride;  ///< source row stride, in FLOATS

    float weight;   ///< 1/N for a shutter-sample accumulation pass
};

/// Buffer argument slots. One list so the host and the kernels cannot disagree.
enum MetalSlot
{
    kSlotSrc       = 0,
    kSlotDst       = 1,
    kSlotFrame     = 2,
    kSlotScene     = 3,
    kSlotSegs      = 4,
    kSlotTileStart = 5,
    kSlotTileItems = 6,
    kSlotFill      = 7,
    kSlotBandStart = 8,
    kSlotBandItems = 9,
    kSlotStyles    = 10,
    kSlotAccum     = 11
};

} // namespace bl
