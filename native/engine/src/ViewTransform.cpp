#include "exrc/ViewTransform.h"

#include <algorithm>
#include <cmath>

namespace exrc {

double effectiveZoom(const ViewState& v, int vpW, int vpH, int imgW, int imgH, double pa) {
    if (!v.fit) return v.zoom;
    if (imgW <= 0 || imgH <= 0 || vpW <= 0 || vpH <= 0) return 1.0;
    return std::min(double(vpW) / (imgW * pa), double(vpH) / imgH);
}

PixelRect imageRect(const ViewState& v, int vpW, int vpH, int imgW, int imgH, double pa) {
    const double z = effectiveZoom(v, vpW, vpH, imgW, imgH, pa);
    const double w = imgW * pa * z, h = imgH * z;
    const double cx = vpW * 0.5 + (v.fit ? 0.0 : v.panX);
    const double cy = vpH * 0.5 + (v.fit ? 0.0 : v.panY);
    return {cx - w * 0.5, cy - h * 0.5, cx + w * 0.5, cy + h * 0.5};
}

std::optional<std::pair<int, int>> viewportToImage(const ViewState& v, double px, double py, int vpW, int vpH,
                                                   int imgW, int imgH, double pa) {
    const PixelRect r = imageRect(v, vpW, vpH, imgW, imgH, pa);
    if (r.x1 <= r.x0 || r.y1 <= r.y0) return std::nullopt;
    const int x = int(std::floor((px - r.x0) / (r.x1 - r.x0) * imgW));
    const int y = int(std::floor((py - r.y0) / (r.y1 - r.y0) * imgH));
    if (x < 0 || y < 0 || x >= imgW || y >= imgH) return std::nullopt;
    return std::make_pair(x, y);
}

ViewState zoomAbout(const ViewState& v, double factor, double px, double py, int vpW, int vpH, int imgW,
                    int imgH, double pa) {
    ViewState out = v;
    const double z0 = effectiveZoom(v, vpW, vpH, imgW, imgH, pa);
    const PixelRect r = imageRect(v, vpW, vpH, imgW, imgH, pa);
    const double z1 = std::clamp(z0 * factor, 1.0 / 64.0, 64.0);
    // Image-space position under the cursor stays put.
    const double cx = (r.x0 + r.x1) * 0.5, cy = (r.y0 + r.y1) * 0.5;
    const double newCx = px - (px - cx) * (z1 / z0);
    const double newCy = py - (py - cy) * (z1 / z0);
    out.fit = false;
    out.zoom = z1;
    out.panX = newCx - vpW * 0.5;
    out.panY = newCy - vpH * 0.5;
    return out;
}

}  // namespace exrc
