// Pan / zoom maths for the viewer, shared by the renderer (where to draw the image) and
// the UI (pixel probe, zoom-about-cursor). All units are physical (device) pixels.
#pragma once

#include <optional>
#include <utility>

namespace exrc {

struct ViewState {
    bool fit = true;     // RV "Frame" (F): scale image to fit the viewport
    double zoom = 1.0;   // used when fit == false; 1.0 = one image pixel per device pixel
    double panX = 0.0;   // offset of the image centre from the viewport centre
    double panY = 0.0;
};

struct PixelRect {
    double x0, y0, x1, y1;  // top-left / bottom-right, device pixels
};

double effectiveZoom(const ViewState& v, int vpW, int vpH, int imgW, int imgH, double pixelAspect = 1.0);
PixelRect imageRect(const ViewState& v, int vpW, int vpH, int imgW, int imgH, double pixelAspect = 1.0);

// Device pixel in the viewport -> image pixel, or nullopt if outside the image.
std::optional<std::pair<int, int>> viewportToImage(const ViewState& v, double px, double py, int vpW, int vpH,
                                                   int imgW, int imgH, double pixelAspect = 1.0);

// Zoom by `factor` keeping the image point under (px, py) fixed (mouse-wheel zoom).
ViewState zoomAbout(const ViewState& v, double factor, double px, double py, int vpW, int vpH, int imgW,
                    int imgH, double pixelAspect = 1.0);

}  // namespace exrc
