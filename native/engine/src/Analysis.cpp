#include "exrc/Analysis.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace exrc {

RegionStats regionStats(const Frame& f, int x0, int y0, int x1, int y1) {
    RegionStats s;
    if (f.missing || f.data.empty()) return s;
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    x0 = std::clamp(x0, 0, f.width - 1);
    x1 = std::clamp(x1, 0, f.width - 1);
    y0 = std::clamp(y0, 0, f.height - 1);
    y1 = std::clamp(y1, 0, f.height - 1);
    std::array<double, 4> sum{};
    std::array<std::vector<float>, 4> values;
    for (auto& v : values) v.reserve(size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1));
    s.min.fill(std::numeric_limits<double>::infinity());
    s.max.fill(-std::numeric_limits<double>::infinity());
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float v[4];
            bool bad = false;
            for (int c = 0; c < 4; ++c) {
                v[c] = f.sample(x, y, c);
                if (std::isnan(v[c])) {
                    ++s.nan;
                    bad = true;
                } else if (std::isinf(v[c])) {
                    ++s.inf;
                    bad = true;
                }
            }
            if (bad) continue;
            ++s.count;
            for (int c = 0; c < 4; ++c) {
                sum[c] += v[c];
                values[c].push_back(v[c]);
                s.min[c] = std::min(s.min[c], double(v[c]));
                s.max[c] = std::max(s.max[c], double(v[c]));
            }
        }
    }
    for (int c = 0; c < 4; ++c) {
        if (s.count) {
            s.mean[c] = sum[c] / s.count;
            auto& v = values[c];
            auto mid = v.begin() + v.size() / 2;
            std::nth_element(v.begin(), mid, v.end());
            s.median[c] = *mid;
        } else {
            s.min[c] = s.max[c] = 0;
        }
    }
    return s;
}

std::vector<float> downsampleRgba(const Frame& f, int maxWidth, int& outW, int& outH) {
    outW = outH = 0;
    if (f.missing || f.data.empty() || f.width <= 0) return {};
    const int step = std::max(1, (f.width + maxWidth - 1) / std::max(1, maxWidth));
    outW = (f.width + step - 1) / step;
    outH = (f.height + step - 1) / step;
    std::vector<float> out(size_t(outW) * outH * 4);
    for (int y = 0; y < outH; ++y)
        for (int x = 0; x < outW; ++x)
            for (int c = 0; c < 4; ++c) out[(size_t(y) * outW + x) * 4 + c] = f.sample(x * step, y * step, c);
    return out;
}

Histogram histogram(const float* rgba, int w, int h, int bins) {
    Histogram hg;
    hg.bins = std::max(2, bins);
    hg.r.assign(hg.bins, 0);
    hg.g.assign(hg.bins, 0);
    hg.b.assign(hg.bins, 0);
    hg.luma.assign(hg.bins, 0);
    auto bin = [&](float v) {
        if (!std::isfinite(v)) return -1;
        return std::clamp(int(v * (hg.bins - 1) + 0.5f), 0, hg.bins - 1);
    };
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const float* p = rgba + i * 4;
        int k;
        if ((k = bin(p[0])) >= 0) ++hg.r[k];
        if ((k = bin(p[1])) >= 0) ++hg.g[k];
        if ((k = bin(p[2])) >= 0) ++hg.b[k];
        if ((k = bin(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2])) >= 0) ++hg.luma[k];
    }
    return hg;
}

std::vector<uint8_t> waveform(const float* rgba, int w, int h, int outW, int outH) {
    std::vector<uint8_t> img(size_t(outW) * outH * 4, 0);
    if (!rgba || w <= 0 || h <= 0 || outW <= 0 || outH <= 0) return img;
    std::vector<uint32_t> acc(size_t(outW) * outH * 3, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int ox = std::min(outW - 1, x * outW / w);
            const float* p = rgba + (size_t(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                if (!std::isfinite(p[c])) continue;
                const int oy = outH - 1 - std::clamp(int(p[c] * (outH - 1) + 0.5f), 0, outH - 1);
                ++acc[(size_t(oy) * outW + ox) * 3 + c];
            }
        }
    }
    // Log-ish intensity so sparse traces stay visible.
    const double norm = std::log1p(double(h) / 4.0);
    for (size_t i = 0; i < size_t(outW) * outH; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double v = std::log1p(double(acc[i * 3 + c])) / norm;
            img[i * 4 + c] = uint8_t(std::clamp(v * 255.0, 0.0, 255.0));
        }
        img[i * 4 + 3] = 255;
    }
    return img;
}

namespace {

std::shared_ptr<Frame> newFloatFrame(int w, int h, int fullW, int fullH, int frameNumber, uint32_t key) {
    auto f = std::make_shared<Frame>();
    f->frameNumber = frameNumber;
    f->key = key;
    f->width = w;
    f->height = h;
    f->fullWidth = fullW;
    f->fullHeight = fullH;
    f->channels = 4;
    f->format = PixelFormat::Float;
    f->data.assign(size_t(w) * h * 16, 0);
    return f;
}

inline void put(Frame& f, int x, int y, int c, float v) {
    std::memcpy(f.data.data() + ((size_t(y) * f.width + x) * 4 + c) * 4, &v, 4);
}

}  // namespace

FramePtr composeContactSheet(const std::vector<ContactTile>& tiles, int cols, int tileW, int tileH, int frameNumber,
                             uint32_t key) {
    cols = std::max(1, cols);
    const int rows = std::max(1, int((tiles.size() + cols - 1) / cols));
    auto out = newFloatFrame(cols * tileW, rows * tileH, cols * tileW, rows * tileH, frameNumber, key);
    for (size_t t = 0; t < tiles.size(); ++t) {
        const FramePtr& f = tiles[t].frame;
        if (!f || f->missing || f->width <= 0) continue;
        const int tx = int(t % cols) * tileW, ty = int(t / cols) * tileH;
        // Fit the frame inside the tile, keeping aspect.
        const double s = std::min(double(tileW) / f->width, double(tileH) / f->height);
        const int w = std::max(1, int(f->width * s)), h = std::max(1, int(f->height * s));
        const int ox = tx + (tileW - w) / 2, oy = ty + (tileH - h) / 2;
        float lo = 0.0f, hi = 1.0f;
        if (tiles[t].normalise) {
            const auto& st = f->stats();
            lo = float(std::min({st[0].min, st[1].min, st[2].min}));
            hi = float(std::max({st[0].max, st[1].max, st[2].max}));
            if (!(hi > lo)) hi = lo + 1.0f;
        }
        for (int y = 0; y < h; ++y) {
            const int sy = std::min(f->height - 1, int(y / s));
            for (int x = 0; x < w; ++x) {
                const int sx = std::min(f->width - 1, int(x / s));
                for (int c = 0; c < 3; ++c) {
                    float v = f->sample(sx, sy, c);
                    if (tiles[t].normalise) v = std::isfinite(v) ? (v - lo) / (hi - lo) : 0.0f;
                    put(*out, ox + x, oy + y, c, v);
                }
                put(*out, ox + x, oy + y, 3, 1.0f);
            }
        }
    }
    return out;
}

FramePtr rebuildFrame(const FramePtr& beauty, const std::vector<FramePtr>& parts, bool difference, int frameNumber,
                      uint32_t key) {
    // Size from the beauty, or the first part when no beauty is given.
    FramePtr ref = beauty && !beauty->missing ? beauty : nullptr;
    for (const auto& p : parts)
        if (!ref && p && !p->missing) ref = p;
    if (!ref) {
        auto f = std::make_shared<Frame>();
        f->frameNumber = frameNumber;
        f->key = key;
        f->missing = true;
        f->error = "Rebuild: no readable AOVs";
        return f;
    }
    auto out = newFloatFrame(ref->width, ref->height, ref->fullWidth, ref->fullHeight, frameNumber, key);
    for (int y = 0; y < ref->height; ++y) {
        for (int x = 0; x < ref->width; ++x) {
            for (int c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (const auto& p : parts)
                    if (p && !p->missing && p->width == ref->width && p->height == ref->height) sum += p->sample(x, y, c);
                float v = sum;
                if (difference) v = std::abs((beauty && !beauty->missing ? beauty->sample(x, y, c) : 0.0f) - sum);
                put(*out, x, y, c, v);
            }
            put(*out, x, y, 3, 1.0f);
        }
    }
    return out;
}

}  // namespace exrc
