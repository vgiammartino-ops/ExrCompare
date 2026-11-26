#include "exrc/Types.h"

#include <half.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace exrc {

Rational Rational::fromFps(double fps) {
    struct Known {
        double fps;
        int64_t num, den;
    };
    static const Known known[] = {{23.976, 24000, 1001}, {29.97, 30000, 1001}, {47.952, 48000, 1001},
                                  {59.94, 60000, 1001},  {119.88, 120000, 1001}};
    for (const auto& k : known)
        if (std::abs(fps - k.fps) < 0.005) return {k.num, k.den};
    if (std::abs(fps - std::round(fps)) < 1e-6) return {int64_t(std::llround(fps)), 1};
    return {int64_t(std::llround(fps * 1000.0)), 1000};
}

std::string Rational::label() const {
    if (den == 1) return std::to_string(num);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", toDouble());
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

const char* aovKindName(AovKind k) {
    switch (k) {
        case AovKind::Color: return "color";
        case AovKind::Depth: return "depth";
        case AovKind::Normal: return "normal";
        case AovKind::Motion: return "motion";
        case AovKind::Position: return "position";
        case AovKind::UV: return "uv";
        case AovKind::Crypto: return "crypto";
        case AovKind::Id: return "id";
        case AovKind::Matte: return "matte";
        case AovKind::Generic: return "generic";
    }
    return "color";
}

float Frame::sample(int x, int y, int c) const {
    if (x < 0 || y < 0 || x >= width || y >= height || data.empty()) return 0.0f;
    if (channels == 1) {
        if (c == 3) return 1.0f;
        c = 0;
    }
    const size_t index = (size_t(y) * width + x) * channels + c;
    if (format == PixelFormat::Float) {
        float v;
        std::memcpy(&v, data.data() + index * 4, 4);
        return v;
    }
    uint16_t bits;
    std::memcpy(&bits, data.data() + index * 2, 2);
    half h;
    h.setBits(bits);
    return float(h);
}

const std::array<ChannelStats, 4>& Frame::stats() const {
    std::call_once(statsOnce_, [this] {
        const int stored = channels;
        for (int c = 0; c < 4; ++c) {
            ChannelStats& s = stats_[c];
            if (c >= stored && !(stored == 1 && c < 3)) {  // missing alpha in a 1-channel frame
                s.min = s.max = s.mean = 1.0;
                continue;
            }
            const int src = stored == 1 ? 0 : c;
            double sum = 0, mn = std::numeric_limits<double>::infinity(), mx = -mn;
            const size_t n = size_t(width) * height;
            for (size_t i = 0; i < n; ++i) {
                float v;
                const size_t idx = i * stored + src;
                if (format == PixelFormat::Float) {
                    std::memcpy(&v, data.data() + idx * 4, 4);
                } else {
                    uint16_t bits;
                    std::memcpy(&bits, data.data() + idx * 2, 2);
                    half h;
                    h.setBits(bits);
                    v = float(h);
                }
                if (std::isnan(v)) {
                    ++s.nan;
                    continue;
                }
                if (std::isinf(v)) {
                    ++s.inf;
                    continue;
                }
                if (v < 0) ++s.negative;
                ++s.finite;
                sum += v;
                mn = std::min(mn, double(v));
                mx = std::max(mx, double(v));
            }
            if (s.finite) {
                s.min = mn;
                s.max = mx;
                s.mean = sum / double(s.finite);
            }
        }
    });
    return stats_;
}

}  // namespace exrc
