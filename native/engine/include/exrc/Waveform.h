#pragma once

// Waveform monitor: scale, display-light model, overlay layout and a CPU reference of the GPU
// counting (used by tests to prove the GPU result).
//
// Counting layout (shared by the GPU buffer and waveformReference):
//   plane p (0 R, 1 G, 2 B, 3 luma) x row r (0 = below scale / <= 0 on a log scale,
//   1..rows = in range bottom to top, rows + 1 = above scale) x column c:
//     index = (p * (rows + 2) + r) * cols + c
//   then 4 NaN/Inf counters (one per plane) and the peak (max finite value, as float bits).

#include "exrc/Types.h"
#include "exrc/ViewTransform.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace exrc {

struct WaveLine {
    float t = 0;         // 0 = bottom of the plot, 1 = top
    float strength = 0;  // 0..1 line brightness
    std::string label;   // empty = no label
};

struct WaveScale {
    float lo = 0, hi = 1;  // log2 bounds when logScale, else linear bounds
    bool logScale = true;
    std::vector<WaveLine> lines;
};

// scale: "stops6" | "stops8" | "stops10" | "stops14" (scene-linear, log2 around 0.18)
//        "linear1" | "linear4" | "linear16"
// units: 0 scene-linear, 1 display light in nits (always log, 0.01 nits to above the peak).
WaveScale waveScale(const std::string& scale, int units, float peakNits);

// Display light model for the current OCIO display/view, from their names.
struct DisplayLight {
    int eotf = 0;  // 0 sRGB piecewise, 1 gamma 2.2, 2 gamma 2.4 (BT.1886), 3 PQ (ST 2084), 4 gamma 2.6
    float peakNits = 100;
    std::array<float, 3> luma{0.2126f, 0.7152f, 0.0722f};  // of the display primaries
};
DisplayLight displayLight(const std::string& display, const std::string& view);

// Row for a value: 0 below, 1..rows in range, rows + 1 above; -1 for NaN / Inf.
inline int waveRow(float v, float lo, float hi, bool logScale, int rows) {
    if (std::isnan(v) || std::isinf(v)) return -1;
    if (logScale && !(v > 0.0f)) return 0;
    const float t = ((logScale ? std::log2(v) : v) - lo) / (hi - lo);
    if (t >= 1.0f) return rows + 1;
    if (t < 0.0f) return 0;
    return 1 + std::min(int(t * float(rows)), rows - 1);
}

inline size_t waveBinCount(int cols, int rows) { return size_t(4) * size_t(rows + 2) * size_t(cols) + 5; }

// CPU reference of the GPU count for a full frame (or a texel box) of raw scene-linear values.
std::vector<uint32_t> waveformReference(const Frame& f, int x0, int y0, int x1, int y1, int cols, int rows,
                                        const WaveScale& scale, const std::array<float, 3>& luma);

// Overlay placement in the viewport. corner: "br" | "bl" | "tr" | "tl"; size: "s" | "m" | "l".
void waveformBox(int vpW, int vpH, const std::string& corner, const std::string& size, int headerPx, PixelRect& box,
                 PixelRect& plot);

}  // namespace exrc
