#pragma once

// Host-side description of one frame's scene, ready to upload to a GPU.
//
// Shared by the CUDA path (Windows) and the Metal path (macOS): both upload the
// same seven arrays, so there is one description of them rather than two that
// can drift. Free of any GPU API header, so the plugin translation unit is
// compiled by the host compiler alone.

#include "core/Raster.h"

namespace bl {

struct GpuSceneDesc
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

} // namespace bl
