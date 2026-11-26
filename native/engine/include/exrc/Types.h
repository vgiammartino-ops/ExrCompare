// Core value types shared by the engine, colour pipeline, renderer and C API.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace exrc {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;  // QueryPerformanceCounter on Windows
using TimePoint = Clock::time_point;

// Frame rates are exact fractions so 23.976 (24000/1001) never drifts.
struct Rational {
    int64_t num = 24;
    int64_t den = 1;

    double toDouble() const { return den ? double(num) / double(den) : 0.0; }
    bool operator==(const Rational& o) const { return num * o.den == o.num * den; }
    static Rational fromFps(double fps);  // 23.976 -> 24000/1001 etc.
    std::string label() const;            // "23.976", "24", "29.97"
};

enum class PixelFormat : uint8_t { Half, Float };

struct ChannelStats {
    double min = 0, max = 0, mean = 0;
    uint64_t nan = 0, inf = 0, negative = 0, finite = 0;
};

// One decoded image (one AOV of one frame of one layer slot).
// Pixels are interleaved, top-down rows, `channels` per pixel (1 = greyscale AOV, 4 = RGBA).
struct Frame {
    int frameNumber = 0;
    uint32_t key = 0;        // cache key: see cacheKey()
    int width = 0;           // stored size (smaller than full size when proxy)
    int height = 0;
    int fullWidth = 0;       // display window size
    int fullHeight = 0;
    int channels = 4;        // 1 or 4
    PixelFormat format = PixelFormat::Half;
    std::vector<uint8_t> data;
    bool missing = false;    // no file / out of range / decode error (see error)
    std::string error;

    size_t bytesPerSample() const { return format == PixelFormat::Half ? 2 : 4; }
    size_t rowBytes() const { return size_t(width) * channels * bytesPerSample(); }
    size_t byteSize() const { return data.size() + sizeof(Frame); }
    // Value of channel c (0..3) at stored pixel (x, y). Greyscale frames replicate R; alpha = 1.
    float sample(int x, int y, int c) const;

    // Per-channel statistics, computed once on first request (thread-safe).
    const std::array<ChannelStats, 4>& stats() const;

private:
    mutable std::once_flag statsOnce_;
    mutable std::array<ChannelStats, 4> stats_{};
};
using FramePtr = std::shared_ptr<const Frame>;

// What kind of data an AOV holds; decides the default viewer mode.
enum class AovKind : uint8_t { Color, Depth, Normal, Motion, Position, UV, Crypto, Id, Generic };
const char* aovKindName(AovKind k);

// A viewable AOV / layer inside the EXR.
struct LayerInfo {
    std::string displayName;            // "RGBA", "diffuse", "beauty.specular", "Z"
    int part = 0;                       // multi-part index
    std::array<std::string, 4> source;  // file channel feeding R,G,B,A ("" = fill)
    bool replicateFirst = false;        // single-channel AOV (stored as 1 channel)
    bool isData = false;                // decoded as 32-bit float, no colour transform by default
    AovKind kind = AovKind::Color;
    int channelCount = 0;
    std::string channelSummary;         // "R,G,B,A"
};

struct Chromaticities {
    float rx, ry, gx, gy, bx, by, wx, wy;
};

struct MetadataEntry {
    std::string name, type, value;
    int part = 0;
};

// Header information from the first frame of a sequence.
struct ImageInfo {
    int width = 0, height = 0;                              // display window size
    int displayX0 = 0, displayY0 = 0;
    int dataX0 = 0, dataY0 = 0, dataX1 = -1, dataY1 = -1;   // data window (inclusive)
    float pixelAspect = 1.0f;
    std::optional<Rational> fps;
    std::optional<Chromaticities> chromaticities;
    std::string compression;
    int partCount = 1;
    bool tiled = false;
    bool mipmapped = false;
    std::vector<std::string> views;                          // multi-view (stereo) names; empty = mono
    std::vector<std::string> allChannels;                    // every channel name (alpha picker)
    std::vector<MetadataEntry> metadata;
    // Cryptomatte manifests: layer base name (e.g. "CryptoObject") -> (id bits -> object name)
    std::map<std::string, std::map<uint32_t, std::string>> cryptoManifests;
};

// A detected image sequence on disk, e.g. shot_v01.####.exr frames 1001-1100.
struct SequenceInfo {
    fs::path directory;
    std::string prefix;  // "shot_v01."
    std::string suffix;  // ".exr"
    int padding = 0;
    int first = 1;
    int last = 1;
    std::vector<std::pair<int, fs::path>> files;  // sorted by frame number
    std::vector<int> missing;

    std::optional<fs::path> pathFor(int frame) const;
    std::string pattern() const;
    int frameCount() const { return last - first + 1; }
};

// Cache keys pack the layer slot (A=0, B=1), a per-slot generation (bumped when a slot is
// reopened or its decode options change) and the AOV index.
inline uint32_t cacheKey(int slot, int generation, int aov) {
    return (uint32_t(slot & 0x3) << 30) | (uint32_t(generation & 0x3FFF) << 16) | uint32_t(aov & 0xFFFF);
}

// Virtual AOV indices (computed images, not read from a single channel group).
constexpr int kAovContactSheet = 0xFFF0;
constexpr int kAovRebuildDiff = 0xFFF1;
constexpr int kAovRebuildSum = 0xFFF2;

}  // namespace exrc
