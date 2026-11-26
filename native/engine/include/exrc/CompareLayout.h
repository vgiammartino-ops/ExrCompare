// Where layers A and B are drawn in the viewport for each compare mode.
// All rectangles are in viewport (device) pixels.
#pragma once

#include "exrc/ViewTransform.h"

#include <optional>
#include <string>
#include <utility>

namespace exrc {

enum class CompareMode { A = 0, B, Wipe, Stack, Difference, SideBySide, OverUnder };
enum class AlignMode { Fit = 0, Pixel };  // B scaled to A's size, or B at the same pixel scale as A

const char* compareModeName(CompareMode m);
std::optional<CompareMode> compareModeFromName(const std::string& name);

struct LayerGeom {
    bool present = false;
    int width = 0, height = 0;  // display window size (full resolution)
    float pixelAspect = 1.0f;
};

struct Layout {
    PixelRect canvas{0, 0, 0, 0};
    PixelRect a{0, 0, 0, 0}, b{0, 0, 0, 0};
    bool showA = false, showB = false;
    int canvasW = 0, canvasH = 0;  // canvas size in canvas pixels (for zoom maths)
};

Layout computeLayout(const ViewState& view, int vpW, int vpH, CompareMode mode, AlignMode align, const LayerGeom& A,
                     const LayerGeom& B);

// Viewport pixel -> full-resolution image pixel of a layer drawn in `rect`.
std::optional<std::pair<int, int>> rectToImage(const PixelRect& rect, double px, double py, int imgW, int imgH);

}  // namespace exrc
