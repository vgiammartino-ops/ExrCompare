#include "exrc/Waveform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <regex>

namespace exrc {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string fmtValue(double v) {
    char buf[32];
    if (v >= 100) std::snprintf(buf, sizeof buf, "%.0f", v);
    else if (v >= 10) std::snprintf(buf, sizeof buf, "%.1f", v);
    else if (v >= 0.1) std::snprintf(buf, sizeof buf, "%.2f", v);
    else if (v >= 0.01) std::snprintf(buf, sizeof buf, "%.3f", v);
    else std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

}  // namespace

WaveScale waveScale(const std::string& scale, int units, float peakNits) {
    WaveScale s;
    if (units == 1) {
        // Display light: log2 from 0.01 nits to a little above the display peak; decade lines.
        const float peak = std::max(peakNits, 1.0f);
        s.logScale = true;
        s.lo = std::log2(0.01f);
        s.hi = std::log2(peak * 1.25f);
        for (double d = 0.01; d <= peak * 1.25; d *= 10) {
            if (std::abs(d - peak) / peak < 0.05) continue;  // the peak line replaces it
            s.lines.push_back({float((std::log2(d) - s.lo) / (s.hi - s.lo)), 0.35f, fmtValue(d) + " nits"});
        }
        s.lines.push_back({float((std::log2(peak) - s.lo) / (s.hi - s.lo)), 0.9f, fmtValue(peak) + " nits peak"});
        return s;
    }
    if (scale.rfind("linear", 0) == 0) {
        const float top = scale == "linear16" ? 16.0f : scale == "linear4" ? 4.0f : 1.0f;
        s.logScale = false;
        s.lo = 0.0f;
        s.hi = top;
        for (int i = 1; i <= 8; ++i) {
            const double v = top * i / 8.0;
            const bool labelled = i % 2 == 0;
            s.lines.push_back({float(i / 8.0), labelled ? 0.4f : 0.22f, labelled ? fmtValue(v) : ""});
        }
        if (top > 1.0f) s.lines.push_back({1.0f / top, 0.9f, "1.0"});
        return s;
    }
    int range = 8;
    if (scale == "stops6") range = 6;
    else if (scale == "stops10") range = 10;
    else if (scale == "stops14") range = 14;
    s.logScale = true;
    const float grey = std::log2(0.18f);
    s.lo = grey - float(range);
    s.hi = grey + float(range);
    const int labelEvery = range >= 10 ? 4 : 2;
    for (int k = -range; k <= range; ++k) {
        const float t = float(k + range) / float(2 * range);
        if (k == 0) {
            s.lines.push_back({t, 0.75f, "0.18  0 st"});
            continue;
        }
        const bool labelled = k % labelEvery == 0;
        char buf[48];
        std::snprintf(buf, sizeof buf, "%+d st  %s", k, fmtValue(0.18 * std::exp2(double(k))).c_str());
        s.lines.push_back({t, labelled ? 0.38f : 0.2f, labelled ? std::string(buf) : ""});
    }
    s.lines.push_back({(0.0f - s.lo) / (s.hi - s.lo), 0.9f, "1.0"});  // display white
    return s;
}

DisplayLight displayLight(const std::string& display, const std::string& view) {
    DisplayLight d;
    const std::string dl = lower(display), vl = lower(view);
    auto in = [](const std::string& s, const char* k) { return s.find(k) != std::string::npos; };
    auto both = [&](const char* k) { return in(dl, k) || in(vl, k); };
    // Decide the decoding curve from the display (the view name only when it names a curve).
    if (both("st2084") || both("st-2084") || both("st 2084") || both("-pq") || both(" pq") || both("pq ")) {
        d.eotf = 3;
        d.peakNits = 1000;
    } else if (in(vl, "gamma 2.2") || in(dl, "gamma 2.2")) {
        d.eotf = 1;
    } else if (in(vl, "1886") || in(vl, "gamma 2.4") || in(dl, "1886") || in(dl, "rec.709") || in(dl, "rec709")) {
        d.eotf = 2;
    } else if (in(dl, "dci") || dl.rfind("p3-d65", 0) == 0 || in(dl, "gamma 2.6")) {
        d.eotf = 4;  // cinema P3 projection, gamma 2.6
        d.peakNits = 48;
    } else {
        d.eotf = 0;  // sRGB (also Display P3)
    }
    // "SDR 100 nits", "HDR 1000 nits (P3 D65)": the view names the target peak.
    static const std::regex nits(R"((\d+(\.\d+)?)\s*nits)");
    std::smatch m;
    if (std::regex_search(vl, m, nits)) d.peakNits = std::stof(m[1].str());
    if (both("2100") || both("2020")) d.luma = {0.2627f, 0.6780f, 0.0593f};
    else if (both("p3")) d.luma = {0.228975f, 0.691739f, 0.079287f};
    return d;
}

std::vector<uint32_t> waveformReference(const Frame& f, int x0, int y0, int x1, int y1, int cols, int rows,
                                        const WaveScale& scale, const std::array<float, 3>& luma) {
    std::vector<uint32_t> bins(waveBinCount(cols, rows), 0);
    const size_t nanBase = size_t(4) * size_t(rows + 2) * size_t(cols);
    x0 = std::clamp(x0, 0, f.width);
    x1 = std::clamp(x1, x0, f.width);
    y0 = std::clamp(y0, 0, f.height);
    y1 = std::clamp(y1, y0, f.height);
    const int w = std::max(1, x1 - x0);
    float peak = 0.0f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const int col = std::min(cols - 1, int(int64_t(x - x0) * cols / w));
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = f.sample(x, y, f.channels == 1 ? 0 : k);
            const float l = c[0] * luma[0] + c[1] * luma[1] + c[2] * luma[2];
            const float v[4] = {c[0], c[1], c[2], l};
            for (int p = 0; p < 4; ++p) {
                const int r = waveRow(v[p], scale.lo, scale.hi, scale.logScale, rows);
                if (r < 0) {
                    ++bins[nanBase + size_t(p)];
                    continue;
                }
                ++bins[(size_t(p) * size_t(rows + 2) + size_t(r)) * size_t(cols) + size_t(col)];
                if (p < 3 && std::isfinite(v[p]) && v[p] > peak) peak = v[p];
            }
        }
    }
    uint32_t bits;
    std::memcpy(&bits, &peak, 4);
    bins[nanBase + 4] = bits;
    return bins;
}

void waveformBox(int vpW, int vpH, const std::string& corner, const std::string& size, int headerPx, PixelRect& box,
                 PixelRect& plot) {
    double fw = 0.38, fh = 0.36;
    if (size == "s") fw = 0.28, fh = 0.26;
    else if (size == "l") fw = 0.52, fh = 0.48;
    const double margin = 12;
    const double w = std::clamp(vpW * fw, std::min(260.0, vpW - 2 * margin), std::max(1.0, vpW - 2 * margin));
    const double h = std::clamp(vpH * fh, std::min(170.0, vpH - 2 * margin), std::max(1.0, vpH - 2 * margin));
    const bool right = corner.empty() || corner[corner.size() - 1] == 'r';
    const bool bottom = corner.empty() || corner[0] == 'b';
    box.x0 = right ? vpW - margin - w : margin;
    box.y0 = bottom ? vpH - margin - h : margin;
    box.x1 = box.x0 + w;
    box.y1 = box.y0 + h;
    plot = {box.x0 + 2, box.y0 + headerPx, box.x1 - 2, box.y1 - 2};
}

}  // namespace exrc
