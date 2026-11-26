#include "exrc/CompareLayout.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace exrc {

const char* compareModeName(CompareMode m) {
    switch (m) {
        case CompareMode::A: return "a";
        case CompareMode::B: return "b";
        case CompareMode::Wipe: return "wipe";
        case CompareMode::Stack: return "stack";
        case CompareMode::Difference: return "difference";
        case CompareMode::SideBySide: return "sidebyside";
        case CompareMode::OverUnder: return "overunder";
    }
    return "a";
}

std::optional<CompareMode> compareModeFromName(const std::string& n) {
    for (int i = 0; i <= int(CompareMode::OverUnder); ++i)
        if (n == compareModeName(CompareMode(i))) return CompareMode(i);
    return std::nullopt;
}

namespace {
struct Size {
    double w = 0, h = 0;
};
Size displaySize(const LayerGeom& g) { return {g.width * double(g.pixelAspect), double(g.height)}; }

PixelRect centred(double cx, double cy, double w, double h) { return {cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2}; }
}  // namespace

Layout computeLayout(const ViewState& view, int vpW, int vpH, CompareMode mode, AlignMode align, const LayerGeom& A,
                     const LayerGeom& B) {
    Layout L;
    const bool hasA = A.present && A.width > 0 && A.height > 0;
    const bool hasB = B.present && B.width > 0 && B.height > 0;
    if (!hasA && !hasB) return L;

    // Which layers are drawn.
    switch (mode) {
        case CompareMode::A: L.showA = hasA; L.showB = !hasA && hasB; break;
        case CompareMode::B: L.showB = hasB; L.showA = !hasB && hasA; break;
        default: L.showA = hasA; L.showB = hasB; break;
    }
    const Size sa = displaySize(A), sb = displaySize(B);
    const bool sideBySide = mode == CompareMode::SideBySide && L.showA && L.showB;
    const bool overUnder = mode == CompareMode::OverUnder && L.showA && L.showB;

    // B's size in canvas pixels, relative to A.
    Size bInA = sb;
    if (L.showA && L.showB && align == AlignMode::Fit) {
        if (overUnder)
            bInA = {sa.w, sb.h * sa.w / sb.w};
        else if (sideBySide)
            bInA = {sb.w * sa.h / sb.h, sa.h};
        else
            bInA = {sa.w, sb.h * sa.w / sb.w};
    }

    Size canvas;
    if (sideBySide)
        canvas = {sa.w + bInA.w, std::max(sa.h, bInA.h)};
    else if (overUnder)
        canvas = {std::max(sa.w, bInA.w), sa.h + bInA.h};
    else
        canvas = L.showA ? sa : sb;
    L.canvasW = int(std::lround(canvas.w));
    L.canvasH = int(std::lround(canvas.h));
    L.canvas = imageRect(view, vpW, vpH, std::max(1, L.canvasW), std::max(1, L.canvasH), 1.0);
    const double s = (L.canvas.x1 - L.canvas.x0) / std::max(1.0, canvas.w);
    const double cx = (L.canvas.x0 + L.canvas.x1) / 2, cy = (L.canvas.y0 + L.canvas.y1) / 2;

    if (sideBySide) {
        L.a = centred(L.canvas.x0 + sa.w * s / 2, cy, sa.w * s, sa.h * s);
        L.b = centred(L.canvas.x0 + (sa.w + bInA.w / 2) * s, cy, bInA.w * s, bInA.h * s);
    } else if (overUnder) {
        L.a = centred(cx, L.canvas.y0 + sa.h * s / 2, sa.w * s, sa.h * s);
        L.b = centred(cx, L.canvas.y0 + (sa.h + bInA.h / 2) * s, bInA.w * s, bInA.h * s);
    } else {
        if (L.showA) L.a = centred(cx, cy, sa.w * s, sa.h * s);
        if (L.showB) L.b = L.showA ? centred(cx, cy, bInA.w * s, bInA.h * s) : centred(cx, cy, sb.w * s, sb.h * s);
    }
    return L;
}

std::optional<std::pair<int, int>> rectToImage(const PixelRect& r, double px, double py, int imgW, int imgH) {
    if (r.x1 <= r.x0 || r.y1 <= r.y0 || imgW <= 0 || imgH <= 0) return std::nullopt;
    const int x = int(std::floor((px - r.x0) / (r.x1 - r.x0) * imgW));
    const int y = int(std::floor((py - r.y0) / (r.y1 - r.y0) * imgH));
    if (x < 0 || y < 0 || x >= imgW || y >= imgH) return std::nullopt;
    return std::make_pair(x, y);
}

}  // namespace exrc
