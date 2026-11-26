// CPU image analysis for the Info and Scopes panels, and the computed (virtual) AOVs.
#pragma once

#include "exrc/Types.h"

namespace exrc {

struct RegionStats {
    int count = 0;  // finite pixels
    std::array<double, 4> mean{}, min{}, max{}, median{};
    uint64_t nan = 0, inf = 0;
};
// Stored-pixel rectangle (inclusive), clipped to the frame.
RegionStats regionStats(const Frame& f, int x0, int y0, int x1, int y1);

// Downsampled float RGBA copy of a frame (at most maxWidth wide), for scopes.
std::vector<float> downsampleRgba(const Frame& f, int maxWidth, int& outW, int& outH);

struct Histogram {
    int bins = 256;
    std::vector<uint32_t> r, g, b, luma;
};
// rgba: display-referred values (0..1 range is binned; outside values go to the end bins).
Histogram histogram(const float* rgba, int w, int h, int bins);

// RGB parade-style overlay waveform: x = image column, y = value (0 at bottom, 1 at top).
// Returns outW*outH RGBA8 pixels.
std::vector<uint8_t> waveform(const float* rgba, int w, int h, int outW, int outH);

// Contact sheet: tiles of frames (already decoded) laid out in a grid of `cols` columns.
// Each tile is resized to tileW x tileH; data AOVs are normalised to 0..1 per tile.
struct ContactTile {
    FramePtr frame;
    bool normalise = false;
};
FramePtr composeContactSheet(const std::vector<ContactTile>& tiles, int cols, int tileW, int tileH, int frameNumber,
                             uint32_t key);

// Sum of AOVs (rebuild) and |beauty - sum| (rebuild difference), as float RGBA frames.
FramePtr rebuildFrame(const FramePtr& beauty, const std::vector<FramePtr>& parts, bool difference, int frameNumber,
                      uint32_t key);

}  // namespace exrc
