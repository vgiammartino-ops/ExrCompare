// Helpers that write small EXR files for tests (generated on the fly, no assets).
#pragma once

#include "exrc/Types.h"

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfMultiPartOutputFile.h>
#include <ImfOutputFile.h>
#include <ImfOutputPart.h>
#include <ImfPartType.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>
#include <ImfTiledOutputFile.h>
#include <half.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace testexr {

namespace fs = std::filesystem;

struct Chan {
    std::string name;
    Imf::PixelType type = Imf::HALF;
    float base = 0.0f;  // value = base + x*0.01 + y*0.1
    float constant = -1e30f;  // if set (> -1e29), every pixel has this value
};

inline float valueAt(const Chan& c, int x, int y) { return c.constant > -1e29f ? c.constant : c.base + x * 0.01f + y * 0.1f; }

struct Buffers {
    std::vector<std::vector<half>> halfs;
    std::vector<std::vector<float>> floats;
};

inline Imf::FrameBuffer makeFrameBuffer(const std::vector<Chan>& chans, const IMATH_NAMESPACE::Box2i& dw, Buffers& b) {
    const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
    b.halfs.reserve(chans.size());
    b.floats.reserve(chans.size());
    Imf::FrameBuffer fb;
    for (const auto& c : chans) {
        if (c.type == Imf::FLOAT) {
            auto& v = b.floats.emplace_back(size_t(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) v[size_t(y) * w + x] = valueAt(c, x + dw.min.x, y + dw.min.y);
            fb.insert(c.name, Imf::Slice::Make(Imf::FLOAT, v.data(), dw, sizeof(float), sizeof(float) * w));
        } else {
            auto& v = b.halfs.emplace_back(size_t(w) * h);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) v[size_t(y) * w + x] = half(valueAt(c, x + dw.min.x, y + dw.min.y));
            fb.insert(c.name, Imf::Slice::Make(Imf::HALF, v.data(), dw, sizeof(half), sizeof(half) * w));
        }
    }
    return fb;
}

inline void writeExr(const fs::path& p, int w, int h, const std::vector<Chan>& chans,
                     std::optional<IMATH_NAMESPACE::Box2i> dataWindow = std::nullopt,
                     std::optional<Imf::Rational> fps = std::nullopt,
                     const std::vector<std::pair<std::string, std::string>>& stringAttrs = {}) {
    const IMATH_NAMESPACE::Box2i disp(IMATH_NAMESPACE::V2i(0, 0), IMATH_NAMESPACE::V2i(w - 1, h - 1));
    const IMATH_NAMESPACE::Box2i dw = dataWindow.value_or(disp);
    Imf::Header header(disp, dw);
    header.compression() = Imf::ZIP_COMPRESSION;
    if (fps) Imf::addFramesPerSecond(header, *fps);
    for (const auto& [k, v] : stringAttrs) header.insert(k, Imf::StringAttribute(v));
    for (const auto& c : chans) header.channels().insert(c.name, Imf::Channel(c.type));
    Buffers b;
    Imf::FrameBuffer fb = makeFrameBuffer(chans, dw, b);
    Imf::OutputFile out(p.string().c_str(), header);
    out.setFrameBuffer(fb);
    out.writePixels(dw.max.y - dw.min.y + 1);
}

// Tiled, mip-mapped file where every level is filled with `levelValue(level)`.
inline void writeTiledMip(const fs::path& p, int w, int h, float level0, float level1) {
    const IMATH_NAMESPACE::Box2i disp(IMATH_NAMESPACE::V2i(0, 0), IMATH_NAMESPACE::V2i(w - 1, h - 1));
    Imf::Header header(disp, disp);
    header.setTileDescription(Imf::TileDescription(16, 16, Imf::MIPMAP_LEVELS, Imf::ROUND_DOWN));
    for (const char* n : {"R", "G", "B"}) header.channels().insert(n, Imf::Channel(Imf::HALF));
    Imf::TiledOutputFile out(p.string().c_str(), header);
    for (int level = 0; level < out.numLevels(); ++level) {
        const IMATH_NAMESPACE::Box2i dw = out.dataWindowForLevel(level);
        std::vector<Chan> chans;
        const float v = level == 0 ? level0 : level == 1 ? level1 : 0.0f;
        for (const char* n : {"R", "G", "B"}) chans.push_back({n, Imf::HALF, 0, v});
        Buffers b;
        Imf::FrameBuffer fb = makeFrameBuffer(chans, dw, b);
        out.setFrameBuffer(fb);
        out.writeTiles(0, out.numXTiles(level) - 1, 0, out.numYTiles(level) - 1, level);
    }
}

inline void writeMultipart(const fs::path& p, int w, int h,
                           const std::vector<std::tuple<std::string, std::string, std::vector<Chan>>>& parts) {
    const IMATH_NAMESPACE::Box2i disp(IMATH_NAMESPACE::V2i(0, 0), IMATH_NAMESPACE::V2i(w - 1, h - 1));
    std::vector<Imf::Header> headers;
    for (const auto& [name, view, chans] : parts) {
        Imf::Header hd(disp, disp);
        hd.setName(name);
        hd.setType(Imf::SCANLINEIMAGE);
        if (!view.empty()) hd.setView(view);
        for (const auto& c : chans) hd.channels().insert(c.name, Imf::Channel(c.type));
        headers.push_back(hd);
    }
    Imf::MultiPartOutputFile file(p.string().c_str(), headers.data(), int(headers.size()));
    for (size_t i = 0; i < parts.size(); ++i) {
        Buffers b;
        Imf::FrameBuffer fb = makeFrameBuffer(std::get<2>(parts[i]), disp, b);
        Imf::OutputPart part(file, int(i));
        part.setFrameBuffer(fb);
        part.writePixels(h);
    }
}

struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("exrc_test_" + std::to_string(stamp) + "_" + std::to_string(n++));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

inline std::string frameName(const std::string& prefix, int f, int pad, const std::string& suffix = ".exr") {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*d", pad, f);
    return prefix + buf + suffix;
}

// A sequence with beauty RGB(A), diffuse + specular (which sum to the beauty), depth Z and N.
inline void makeShot(const fs::path& dir, const std::string& prefix, int first, int last, int w = 16, int h = 8,
                     float tint = 0.0f) {
    fs::create_directories(dir);
    for (int f = first; f <= last; ++f) {
        writeExr(dir / frameName(prefix, f, 4), w, h,
                 {{"R", Imf::HALF, 0, 0.5f + tint + f * 0.01f},
                  {"G", Imf::HALF, 0, 0.5f},
                  {"B", Imf::HALF, 0, 0.5f},
                  {"A", Imf::HALF, 0, 1.0f},
                  {"diffuse.R", Imf::HALF, 0, 0.25f + tint + f * 0.01f},
                  {"diffuse.G", Imf::HALF, 0, 0.25f},
                  {"diffuse.B", Imf::HALF, 0, 0.25f},
                  {"specular.R", Imf::HALF, 0, 0.25f},
                  {"specular.G", Imf::HALF, 0, 0.25f},
                  {"specular.B", Imf::HALF, 0, 0.25f},
                  {"Z", Imf::FLOAT, 100.0f},
                  {"N.X", Imf::FLOAT, 0, 0.0f},
                  {"N.Y", Imf::FLOAT, 0, 1.0f},
                  {"N.Z", Imf::FLOAT, 0, 0.0f}},
                 std::nullopt, Imf::Rational(24000, 1001));
    }
}

}  // namespace testexr
