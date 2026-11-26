#include "exrc/Session.h"

#include "exrc/Analysis.h"
#include "exrc/ExrReader.h"
#include "exrc/SequenceScanner.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>
#include <thread>

namespace exrc {

using json = nlohmann::json;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string toUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

int autoThreads(int requested, int divisor, int minimum) {
    if (requested > 0) return requested;
    const int hw = int(std::max(1u, std::thread::hardware_concurrency()));
    return std::max(minimum, hw / divisor);
}

LayerMode defaultMode(AovKind k) {
    switch (k) {
        case AovKind::Color: return LayerMode::Color;
        case AovKind::Depth: return LayerMode::Depth;
        case AovKind::Normal: return LayerMode::Normal;
        case AovKind::Motion: return LayerMode::Motion;
        case AovKind::Crypto:
        case AovKind::Id: return LayerMode::Id;
        case AovKind::Matte: return LayerMode::Raw;  // no OCIO: 1,0,0 shows as pure red
        default: return LayerMode::Raw;
    }
}

const char* modeName(LayerMode m) {
    switch (m) {
        case LayerMode::Color: return "color";
        case LayerMode::Raw: return "raw";
        case LayerMode::Depth: return "depth";
        case LayerMode::Normal: return "normal";
        case LayerMode::Motion: return "motion";
        case LayerMode::Id: return "id";
    }
    return "color";
}

std::optional<LayerMode> modeFromName(const std::string& s) {
    for (int i = 0; i <= int(LayerMode::Id); ++i)
        if (s == modeName(LayerMode(i))) return LayerMode(i);
    return std::nullopt;
}

const char* cacheModeName(CacheMode m) {
    return m == CacheMode::Off ? "off" : m == CacheMode::Region ? "region" : "lookahead";
}

size_t estimateBytes(int w, int h, int channels, PixelFormat fmt, bool proxy) {
    if (proxy) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
    return size_t(w) * h * channels * (fmt == PixelFormat::Half ? 2 : 4) + sizeof(Frame);
}

// ----------------------------------------------------------------------------- sources --

class AovSource : public DecodeSource {
public:
    std::shared_ptr<const SequenceInfo> seq;
    LayerInfo layer;
    DecodeOptions options;
    FramePtr decode(int frame) const override {
        const auto path = seq->pathFor(frame);
        if (!path) return makeMissingFrame(frame, key, frame < seq->first || frame > seq->last ? "Out of range" : "Missing frame");
        FramePtr f = decodeFrame(*path, layer, frame, key, options);
        if (layer.kind == AovKind::Depth && !f->missing) f->stats();  // warm auto-range off the render thread
        return f;
    }
};

class ContactSheetSource : public DecodeSource {
public:
    std::shared_ptr<const SequenceInfo> seq;
    std::vector<LayerInfo> aovs;
    int cols = 1, tileW = 1, tileH = 1;
    FramePtr decode(int frame) const override {
        const auto path = seq->pathFor(frame);
        if (!path) return makeMissingFrame(frame, key, "Missing frame");
        std::vector<ContactTile> tiles;
        DecodeOptions opts;
        opts.proxy = true;  // tiles are small: half resolution is plenty and twice as fast
        for (const auto& l : aovs) {
            DecodeOptions o = opts;
            if (l.kind == AovKind::Crypto || l.kind == AovKind::Id) o.proxy = false;
            tiles.push_back({decodeFrame(*path, l, frame, 0, o), l.isData});
        }
        return composeContactSheet(tiles, cols, tileW, tileH, frame, key);
    }
};

class RebuildSource : public DecodeSource {
public:
    std::shared_ptr<const SequenceInfo> seq;
    std::optional<LayerInfo> beauty;
    std::vector<LayerInfo> parts;
    bool difference = true;
    DecodeOptions options;
    FramePtr decode(int frame) const override {
        const auto path = seq->pathFor(frame);
        if (!path) return makeMissingFrame(frame, key, "Missing frame");
        FramePtr b = beauty ? decodeFrame(*path, *beauty, frame, 0, options) : nullptr;
        std::vector<FramePtr> ps;
        for (const auto& l : parts) ps.push_back(decodeFrame(*path, l, frame, 0, options));
        return rebuildFrame(b, ps, difference, frame, key);
    }
};

}  // namespace

// ================================================================================ slots ==

struct Session::Slot {
    bool loaded = false;
    std::shared_ptr<const SequenceInfo> seq;
    ImageInfo info;
    std::vector<LayerInfo> aovs;
    int aov = 0;
    int generation = 0;
    std::string view;  // stereo view
    std::string inputSpace;
    std::string modeSetting = "auto";
    float opacity = 1.0f;
    bool visible = true;
    int offset = 0;
    bool depthAuto = true;
    float depthNear = 0.0f, depthFar = 1.0f;
    bool depthInvert = false;
    float motionScale = 0.05f;
    std::string alpha;  // alpha channel override
    uint32_t isolate = 0;
    bool hasIsolate = false;
    bool contactSheet = false;
    int rebuild = 0;  // 0 off, 1 difference, 2 sum
    std::vector<std::string> rebuildAovs;
    std::string rebuildBeauty;
    std::map<int, DecodeSourcePtr> sources;

    int effectiveAov() const {
        if (contactSheet) return kAovContactSheet;
        if (rebuild == 1) return kAovRebuildDiff;
        if (rebuild == 2) return kAovRebuildSum;
        return aov;
    }
    const LayerInfo* currentAov() const {
        return aov >= 0 && aov < int(aovs.size()) ? &aovs[aov] : nullptr;
    }
    LayerMode mode() const {
        if (effectiveAov() >= kAovContactSheet) return LayerMode::Color;
        if (auto m = modeFromName(modeSetting)) return *m;
        const LayerInfo* l = currentAov();
        return l ? defaultMode(l->kind) : LayerMode::Color;
    }
    void invalidate() {
        ++generation;
        sources.clear();
    }
};

struct Session::Impl {
    SessionConfig config;
    std::mutex m;
    FrameCache cache;
    std::unique_ptr<DecodeScheduler> scheduler;
    PlaybackController ctrl;
    Slot slots[2];
    ColorPipeline ocio;
    std::string ocioError;

    CompareMode compare = CompareMode::A;
    AlignMode align = AlignMode::Fit;
    bool lockAov = true;
    bool proxy = false;
    CacheMode cacheMode = CacheMode::LookAhead;
    size_t budget;

    ViewState view;
    int vpW = 1, vpH = 1;
    float wipeX = 0.5f, wipeY = 0.5f, wipeAngle = 0.0f, wipeMix = 0.0f, wipeSoft = 0.0f;
    // Pixel Analyzer sample region, normalised to the image (0..1) so it follows pan/zoom and
    // samples the same image area in A and B.
    bool regionActive = false;
    double regU0 = 0, regV0 = 0, regU1 = 0, regV1 = 0;
    float diffGain = 1.0f;
    float exposureStops = 0.0f, gamma = 1.0f, saturation = 1.0f;
    int channel = 0;
    bool nanHighlight = false, falseColor = false, matte = false;

    std::shared_ptr<ShaderProgram> program;
    std::string programKey;
    uint64_t programCounter = 0;

    std::deque<TimePoint> shown;
    int lastShown = INT32_MIN;
    std::string lastRequestKey;

    explicit Impl(const SessionConfig& c) : config(c), cache(c.cacheBudgetBytes), budget(c.cacheBudgetBytes) {
        setExrThreadCount(c.exrThreads >= 0 ? c.exrThreads : autoThreads(0, 1, 1));
        scheduler = std::make_unique<DecodeScheduler>(cache, autoThreads(c.decodeThreads, 2, 2));
        ocio.load(c.ocioConfig, &ocioError);
    }

    // ------------------------------------------------------------------ helpers --
    bool anyLoaded() const { return slots[0].loaded || slots[1].loaded; }

    bool needed(int s) const {
        if (!slots[s].loaded) return false;
        if (compare == CompareMode::A) return s == 0 || !slots[0].loaded;
        if (compare == CompareMode::B) return s == 1 || !slots[1].loaded;
        return true;
    }

    int layerFrame(int s, int global) const { return global - slots[s].offset; }
    bool inRange(int s, int lf) const { return slots[s].seq && lf >= slots[s].seq->first && lf <= slots[s].seq->last; }

    void recomputeRange() {
        int first = INT32_MAX, last = INT32_MIN;
        for (const Slot& sl : slots) {
            if (!sl.loaded) continue;
            first = std::min(first, sl.seq->first + sl.offset);
            last = std::max(last, sl.seq->last + sl.offset);
        }
        if (first > last) first = last = 1;
        const int in = ctrl.inPoint(), out = ctrl.outPoint();
        const bool hadInOut = !(in == ctrl.first() && out == ctrl.last());
        const int cur = ctrl.current();
        ctrl.setRange(first, last);
        if (hadInOut) ctrl.setInOut(in, out);
        ctrl.seek(std::clamp(cur, first, last), Clock::now());
    }

    DecodeSourcePtr source(int s, int effAov) {
        Slot& sl = slots[s];
        if (auto it = sl.sources.find(effAov); it != sl.sources.end()) return it->second;
        if (!sl.loaded) return nullptr;
        const uint32_t key = cacheKey(s, sl.generation, effAov);
        const int w = sl.info.width, h = sl.info.height;
        DecodeSourcePtr made;
        if (effAov == kAovContactSheet) {
            auto src = std::make_shared<ContactSheetSource>();
            src->seq = sl.seq;
            src->aovs = sl.aovs;
            if (src->aovs.size() > 36) src->aovs.resize(36);
            const int n = int(src->aovs.size());
            src->cols = std::max(1, int(std::ceil(std::sqrt(double(n)))));
            const int rows = std::max(1, (n + src->cols - 1) / src->cols);
            src->tileW = std::max(16, w / src->cols);
            src->tileH = std::max(16, h / src->cols);
            (void)rows;
            src->key = key;
            src->estimatedBytes = size_t(src->cols * src->tileW) * rows * src->tileH * 16 + sizeof(Frame);
            made = src;
        } else if (effAov == kAovRebuildDiff || effAov == kAovRebuildSum) {
            auto src = std::make_shared<RebuildSource>();
            src->seq = sl.seq;
            src->difference = effAov == kAovRebuildDiff;
            src->options.proxy = proxy;
            for (const auto& l : sl.aovs) {
                if (l.displayName == sl.rebuildBeauty) src->beauty = l;
                if (std::find(sl.rebuildAovs.begin(), sl.rebuildAovs.end(), l.displayName) != sl.rebuildAovs.end())
                    src->parts.push_back(l);
            }
            src->key = key;
            src->estimatedBytes = estimateBytes(w, h, 4, PixelFormat::Float, proxy);
            made = src;
        } else {
            if (effAov < 0 || effAov >= int(sl.aovs.size())) return nullptr;
            auto src = std::make_shared<AovSource>();
            src->seq = sl.seq;
            src->layer = sl.aovs[effAov];
            const bool idLike = src->layer.kind == AovKind::Crypto || src->layer.kind == AovKind::Id;
            src->options.proxy = proxy && !idLike;  // averaging IDs would invent new IDs
            if (effAov == sl.aov) src->options.alphaOverride = sl.alpha;
            const bool single = src->layer.replicateFirst && src->options.alphaOverride.empty() && src->layer.source[3].empty();
            src->key = key;
            src->estimatedBytes = estimateBytes(w, h, single ? 1 : 4,
                                                src->layer.isData ? PixelFormat::Float : PixelFormat::Half,
                                                src->options.proxy);
            made = src;
        }
        sl.sources[effAov] = made;
        return made;
    }

    FramePtr lookup(int s, int lf) {
        auto src = source(s, slots[s].effectiveAov());
        if (!src) return nullptr;
        if (FramePtr f = cache.get(src->key, lf)) return f;
        return scheduler->lastDecoded(src->key, lf);
    }

    bool readyAt(int global) {
        for (int s = 0; s < 2; ++s) {
            if (!needed(s)) continue;
            const int lf = layerFrame(s, global);
            if (!inRange(s, lf) || !slots[s].seq->pathFor(lf)) continue;  // nothing to wait for
            if (!lookup(s, lf)) return false;
        }
        return true;
    }

    void refreshRequests(bool force) {
        if (!anyLoaded()) {
            scheduler->setRequests({});
            lastRequestKey.clear();
            return;
        }
        const int cur = ctrl.current(), dir = ctrl.direction();
        const int in = ctrl.inPoint(), out = ctrl.outPoint();
        std::ostringstream k;
        k << cur << '|' << dir << '|' << ctrl.playing() << '|' << in << '|' << out << '|' << ctrl.loop() << '|'
          << int(cacheMode) << '|' << budget << '|' << int(compare) << '|' << proxy;
        for (int s = 0; s < 2; ++s)
            k << '|' << needed(s) << ',' << slots[s].generation << ',' << slots[s].effectiveAov() << ',' << slots[s].offset
              << ',' << slots[s].aov;
        if (!force && k.str() == lastRequestKey) return;
        lastRequestKey = k.str();

        std::vector<DecodeRequest> reqs;
        PlayheadHint hint;
        hint.direction = dir;
        hint.loop = ctrl.loop();
        size_t perFrame = 0;
        std::vector<int> active;
        for (int s = 0; s < 2; ++s) {
            if (!needed(s)) continue;
            auto src = source(s, slots[s].effectiveAov());
            if (!src) continue;
            active.push_back(s);
            perFrame += src->estimatedBytes;
            const int lf = layerFrame(s, cur);
            if (inRange(s, lf)) reqs.push_back({src, lf, true});
            PlayheadHint::Active a;
            a.key = src->key;
            a.frame = cacheMode == CacheMode::Region ? layerFrame(s, in) : lf;
            a.rangeStart = layerFrame(s, in);
            a.rangeEnd = layerFrame(s, out);
            hint.active.push_back(a);
        }
        if (cacheMode == CacheMode::Region) {
            hint.direction = 1;
            hint.loop = true;
        }
        const int len = out - in + 1;
        const int fit = int(std::clamp<size_t>(budget / std::max<size_t>(1, perFrame), 1, size_t(len)));
        auto addGlobal = [&](int g) {
            for (int s : active) {
                const int lf = layerFrame(s, g);
                if (inRange(s, lf)) reqs.push_back({source(s, slots[s].effectiveAov()), lf, false});
            }
        };
        if (cacheMode == CacheMode::LookAhead) {
            int count = 1;
            if (!ctrl.playing()) {
                const int back = cur - dir;
                if (back >= in && back <= out) {
                    addGlobal(back);
                    ++count;
                }
            }
            for (int i = 1; count < fit && i < len; ++i, ++count) {
                int64_t off = int64_t(cur) - in + int64_t(dir) * i;
                if (ctrl.loop())
                    off = ((off % len) + len) % len;
                else if (off < 0 || off >= len)
                    break;
                addGlobal(in + int(off));
            }
        } else if (cacheMode == CacheMode::Region) {
            int count = 1;
            for (int g = in; g <= out && count < fit; ++g)
                if (g != cur) {
                    addGlobal(g);
                    ++count;
                }
        }
        // While paused, pre-read the neighbouring AOVs of the current frame (instant PgUp/PgDn).
        if (!ctrl.playing() && cacheMode != CacheMode::Off) {
            for (int s : active) {
                const Slot& sl = slots[s];
                if (sl.effectiveAov() >= kAovContactSheet) continue;
                const int lf = layerFrame(s, cur);
                if (!inRange(s, lf)) continue;
                for (int d : {1, -1}) {
                    const int a = sl.aov + d;
                    if (a >= 0 && a < int(sl.aovs.size())) reqs.push_back({source(s, a), lf, false});
                }
            }
        }
        cache.setPlayhead(hint);
        scheduler->setRequests(std::move(reqs));
    }

    std::shared_ptr<ShaderProgram> ensureProgram() {
        const std::string inA = slots[0].inputSpace.empty() ? ocio.sceneLinear() : slots[0].inputSpace;
        const std::string inB = slots[1].inputSpace.empty() ? ocio.sceneLinear() : slots[1].inputSpace;
        const std::string key = std::to_string(ocio.generation()) + "|" + inA + "|" + inB;
        if (program && key == programKey) return program;
        auto p = std::make_shared<ShaderProgram>();
        try {
            p->inputA = ocio.inputShader(inA, "OCIO_InputA", "ocioa_");
            p->inputB = ocio.inputShader(inB, "OCIO_InputB", "ociob_");
            p->display = ocio.displayShader("OCIO_Display", "ociod_");
        } catch (const std::exception& e) {
            ocioError = e.what();
            return program;  // keep the last working program
        }
        p->id = ++programCounter;
        program = p;
        programKey = key;
        return program;
    }

    Layout layout(int w, int h, const ViewState& v) const {
        LayerGeom ga, gb;
        for (int s = 0; s < 2; ++s) {
            LayerGeom& g = s == 0 ? ga : gb;
            const Slot& sl = slots[s];
            g.present = sl.loaded;
            if (!sl.loaded) continue;
            g.width = sl.info.width;
            g.height = sl.info.height;
            g.pixelAspect = sl.info.pixelAspect > 0 ? sl.info.pixelAspect : 1.0f;
            if (sl.effectiveAov() == kAovContactSheet) {  // contact sheet has its own size
                const auto it = sl.sources.find(kAovContactSheet);
                if (it != sl.sources.end()) {
                    auto cs = std::static_pointer_cast<const ContactSheetSource>(it->second);
                    const int rows = std::max(1, int((cs->aovs.size() + cs->cols - 1) / cs->cols));
                    g.width = cs->cols * cs->tileW;
                    g.height = rows * cs->tileH;
                    g.pixelAspect = 1.0f;
                }
            }
        }
        return computeLayout(v, w, h, compare, align, ga, gb);
    }

    void fillLayer(LayerRender& lr, int s, int global, const PixelRect& rect, bool shown, bool syncDecode) {
        Slot& sl = slots[s];
        lr.present = shown && sl.loaded;
        if (!lr.present) return;
        lr.rect = rect;
        lr.visible = sl.visible;
        lr.opacity = sl.opacity;
        lr.mode = sl.mode();
        lr.motionScale = sl.motionScale;
        lr.depthInvert = sl.depthInvert;
        lr.isolateId = sl.isolate;
        lr.hasIsolate = sl.hasIsolate;
        const int lf = layerFrame(s, global);
        if (!inRange(s, lf)) {
            lr.frame = makeMissingFrame(lf, 0, "Out of range");
            return;
        }
        lr.frame = lookup(s, lf);
        if (!lr.frame && syncDecode) {
            if (auto src = source(s, sl.effectiveAov())) lr.frame = src->decode(lf);
        }
        lr.depthNear = sl.depthNear;
        lr.depthFar = sl.depthFar;
        if (lr.mode == LayerMode::Depth && sl.depthAuto && lr.frame && !lr.frame->missing) {
            const auto& st = lr.frame->stats()[0];
            lr.depthNear = float(st.min);
            lr.depthFar = float(st.max > st.min ? st.max : st.min + 1.0);
        }
    }

    void fillCommon(RenderState& rs, int w, int h) {
        rs.compare = compare;
        rs.wipeX = wipeX * w;
        rs.wipeY = wipeY * h;
        rs.wipeAngle = wipeAngle;
        rs.wipeMix = wipeMix;
        rs.wipeSoftness = wipeSoft;
        rs.diffGain = diffGain;
        rs.exposure = std::exp2(exposureStops);
        rs.gamma = gamma > 0.01f ? gamma : 1.0f;
        rs.saturation = saturation;
        rs.channel = channel;
        rs.flags = (nanHighlight ? uint32_t(kFlagNanHighlight) : 0u) | (falseColor ? uint32_t(kFlagFalseColor) : 0u) |
                   (matte ? uint32_t(kFlagMatteOverlay) : 0u);
        rs.program = ensureProgram();
    }

    // --------------------------------------------------------------------- open --
    json openSlot(int s, const fs::path& path, bool keepSettings, std::string* error) {
        auto seq = scanSequence(path, error);
        if (!seq) return nullptr;
        std::vector<LayerInfo> aovs;
        ImageInfo info;
        Slot& sl = slots[s];
        const std::string stereoView = keepSettings ? sl.view : "";
        try {
            info = readImageInfo(seq->files.front().second, stereoView, aovs);
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return nullptr;
        }
        const bool firstLoad = !anyLoaded();
        const std::string keepAovName = keepSettings && sl.currentAov() ? sl.currentAov()->displayName : "";
        if (!keepSettings) {
            Slot fresh;
            fresh.generation = sl.generation;
            sl = std::move(fresh);
        }
        sl.invalidate();
        sl.loaded = true;
        sl.seq = std::make_shared<const SequenceInfo>(std::move(*seq));
        sl.info = std::move(info);
        sl.aovs = std::move(aovs);
        sl.aov = 0;
        sl.contactSheet = false;
        sl.rebuild = 0;
        std::string wantAov = keepAovName;
        const Slot& other = slots[1 - s];
        if (wantAov.empty() && lockAov && other.loaded && other.currentAov()) wantAov = other.currentAov()->displayName;
        for (size_t i = 0; i < sl.aovs.size(); ++i)
            if (sl.aovs[i].displayName == wantAov) sl.aov = int(i);
        if (!keepSettings || !ocio.hasColorSpace(sl.inputSpace)) sl.inputSpace = ocio.guessInputSpace(sl.seq->files.front().second);
        if (sl.rebuildBeauty.empty() && !sl.aovs.empty()) sl.rebuildBeauty = sl.aovs.front().displayName;

        recomputeRange();
        if (firstLoad) {
            ctrl.pause();
            ctrl.seek(sl.seq->first + sl.offset, Clock::now());
            ctrl.setFrameRate(sl.info.fps.value_or(Rational{24, 1}), Clock::now());
            view = ViewState{};
            if (s == 1) compare = CompareMode::B;
        } else if (s == 1 && compare == CompareMode::A && slots[0].loaded) {
            compare = CompareMode::Wipe;  // second layer loaded: start comparing straight away
        }
        refreshRequests(true);
        return slotJson(s);
    }

    // ------------------------------------------------------------------- JSON --
    json slotJson(int s) const {
        const Slot& sl = slots[s];
        json j;
        j["slot"] = s;
        j["loaded"] = sl.loaded;
        if (!sl.loaded) return j;
        j["pattern"] = sl.seq->pattern();
        j["directory"] = toUtf8(sl.seq->directory);
        j["path"] = toUtf8(sl.seq->files.front().second);
        j["first"] = sl.seq->first;
        j["last"] = sl.seq->last;
        j["missing"] = sl.seq->missing;
        j["version"] = versionToken(*sl.seq);
        j["width"] = sl.info.width;
        j["height"] = sl.info.height;
        j["pixelAspect"] = sl.info.pixelAspect;
        j["compression"] = sl.info.compression;
        j["tiled"] = sl.info.tiled;
        j["mipmapped"] = sl.info.mipmapped;
        j["dataWindow"] = {sl.info.dataX0, sl.info.dataY0, sl.info.dataX1, sl.info.dataY1};
        j["displayOrigin"] = {sl.info.displayX0, sl.info.displayY0};
        if (sl.info.fps) j["fps"] = {sl.info.fps->num, sl.info.fps->den};
        j["views"] = sl.info.views;
        j["view"] = sl.view;
        j["channels"] = sl.info.allChannels;
        json aovs = json::array();
        for (const auto& l : sl.aovs)
            aovs.push_back({{"name", l.displayName}, {"kind", aovKindName(l.kind)}, {"channels", l.channelSummary},
                            {"data", l.isData}});
        j["aovs"] = aovs;
        j["aov"] = sl.aov;
        j["inputSpace"] = sl.inputSpace;
        j["mode"] = sl.modeSetting;
        j["effectiveMode"] = modeName(sl.mode());
        j["opacity"] = sl.opacity;
        j["visible"] = sl.visible;
        j["offset"] = sl.offset;
        j["depthAuto"] = sl.depthAuto;
        j["depthNear"] = sl.depthNear;
        j["depthFar"] = sl.depthFar;
        j["depthInvert"] = sl.depthInvert;
        j["motionScale"] = sl.motionScale;
        j["alpha"] = sl.alpha;
        j["contactSheet"] = sl.contactSheet;
        j["rebuild"] = sl.rebuild;
        j["rebuildAovs"] = sl.rebuildAovs;
        j["rebuildBeauty"] = sl.rebuildBeauty;
        return j;
    }

    json statusJson() {
        json j;
        j["hasContent"] = anyLoaded();
        j["first"] = ctrl.first();
        j["last"] = ctrl.last();
        j["in"] = ctrl.inPoint();
        j["out"] = ctrl.outPoint();
        j["frame"] = ctrl.current();
        j["playing"] = ctrl.playing();
        j["direction"] = ctrl.direction();
        j["loop"] = ctrl.loop();
        j["fps"] = {ctrl.frameRate().num, ctrl.frameRate().den};
        j["fpsLabel"] = ctrl.frameRate().label();
        j["playAllFrames"] = ctrl.mode() == PlaybackMode::PlayAllFrames;
        double measured = 0;
        if (shown.size() >= 2) {
            const double secs = std::chrono::duration<double>(shown.back() - shown.front()).count();
            if (secs > 0) measured = double(shown.size() - 1) / secs;
        }
        j["measuredFps"] = measured;
        j["cacheMode"] = cacheModeName(cacheMode);
        j["cacheBytes"] = cache.bytesUsed();
        j["cacheBudget"] = budget;
        j["compare"] = compareModeName(compare);
        j["align"] = align == AlignMode::Fit ? "fit" : "pixel";
        j["lockAov"] = lockAov;
        j["proxy"] = proxy;
        json layers = json::array();
        for (int s = 0; s < 2; ++s) {
            const Slot& sl = slots[s];
            json l = {{"loaded", sl.loaded}};
            if (sl.loaded) {
                const int lf = layerFrame(s, ctrl.current());
                l["frame"] = lf;
                l["inRange"] = inRange(s, lf);
                l["aov"] = sl.aov;
                l["aovName"] = sl.currentAov() ? sl.currentAov()->displayName : "";
                l["pattern"] = sl.seq->pattern();
                l["mode"] = modeName(sl.mode());
                FramePtr f = inRange(s, lf) ? const_cast<Impl*>(this)->lookup(s, lf) : nullptr;
                l["ready"] = f != nullptr || !inRange(s, lf);
                l["missing"] = !inRange(s, lf) || (f && f->missing) || !sl.seq->pathFor(lf);
                l["error"] = f ? f->error : (!inRange(s, lf) ? "Out of range" : "");
                l["proxy"] = f && f->width < f->fullWidth;
            }
            layers.push_back(l);
        }
        j["layers"] = layers;
        j["color"] = colorJson();
        j["wipe"] = {{"x", wipeX}, {"y", wipeY}, {"angle", wipeAngle}, {"mix", wipeMix}, {"softness", wipeSoft}};
        j["view"] = {{"fit", view.fit}, {"zoom", view.zoom}, {"panX", view.panX}, {"panY", view.panY}};
        return j;
    }

    json colorJson() const {
        return {{"exposure", exposureStops}, {"gamma", gamma}, {"saturation", saturation}, {"channel", channel},
                {"display", ocio.display()}, {"view", ocio.view()},  {"look", ocio.look()},
                {"falseColor", falseColor},  {"nanHighlight", nanHighlight}, {"matte", matte},
                {"diffGain", diffGain}};
    }

    json ocioJson() const {
        json j;
        j["source"] = ocio.source();
        j["name"] = ocio.configName();
        j["version"] = ocio.ocioVersion();
        j["error"] = ocioError;
        j["displays"] = ocio.displays();
        j["views"] = ocio.views(ocio.display());
        j["looks"] = ocio.looks();
        j["colorSpaces"] = ocio.colorSpaces();
        j["sceneLinear"] = ocio.sceneLinear();
        json builtins = json::array();
        for (const auto& b : ColorPipeline::builtinConfigs())
            builtins.push_back({{"name", b.name}, {"uiName", b.uiName}, {"recommended", b.recommended}});
        j["builtins"] = builtins;
        j["display"] = ocio.display();
        j["view"] = ocio.view();
        j["look"] = ocio.look();
        return j;
    }

    json geometryJson() {
        const Layout L = layout(vpW, vpH, view);
        auto r = [](const PixelRect& p) { return json{p.x0, p.y0, p.x1, p.y1}; };
        json j;
        j["viewport"] = {vpW, vpH};
        j["canvas"] = r(L.canvas);
        j["a"] = L.showA ? r(L.a) : json(nullptr);
        j["b"] = L.showB ? r(L.b) : json(nullptr);
        // Data windows mapped into the viewport.
        for (int s = 0; s < 2; ++s) {
            const Slot& sl = slots[s];
            const bool show = s == 0 ? L.showA : L.showB;
            if (!sl.loaded || !show || sl.effectiveAov() == kAovContactSheet) continue;
            const PixelRect& rr = s == 0 ? L.a : L.b;
            const double sx = (rr.x1 - rr.x0) / std::max(1, sl.info.width), sy = (rr.y1 - rr.y0) / std::max(1, sl.info.height);
            const double x0 = rr.x0 + (sl.info.dataX0 - sl.info.displayX0) * sx;
            const double y0 = rr.y0 + (sl.info.dataY0 - sl.info.displayY0) * sy;
            const double x1 = rr.x0 + (sl.info.dataX1 + 1 - sl.info.displayX0) * sx;
            const double y1 = rr.y0 + (sl.info.dataY1 + 1 - sl.info.displayY0) * sy;
            j[s == 0 ? "dataWindowA" : "dataWindowB"] = {x0, y0, x1, y1};
            j[s == 0 ? "imageA" : "imageB"] = {sl.info.width, sl.info.height};
        }
        // Pinned Pixel Analyzer region, drawn on the first visible layer.
        if (regionActive) {
            const PixelRect* rr = L.showA ? &L.a : (L.showB ? &L.b : nullptr);
            if (rr) {
                const double w = rr->x1 - rr->x0, h = rr->y1 - rr->y0;
                j["region"] = {rr->x0 + regU0 * w, rr->y0 + regV0 * h, rr->x0 + regU1 * w, rr->y0 + regV1 * h};
            }
        }
        // Contact sheet tiles (for labels and click-to-select).
        for (int s = 0; s < 2; ++s) {
            const Slot& sl = slots[s];
            const bool show = s == 0 ? L.showA : L.showB;
            if (!sl.loaded || !show || sl.effectiveAov() != kAovContactSheet) continue;
            auto it = sl.sources.find(kAovContactSheet);
            if (it == sl.sources.end()) continue;
            auto cs = std::static_pointer_cast<const ContactSheetSource>(it->second);
            const PixelRect& rr = s == 0 ? L.a : L.b;
            const int rows = std::max(1, int((cs->aovs.size() + cs->cols - 1) / cs->cols));
            const double tw = (rr.x1 - rr.x0) / cs->cols, th = (rr.y1 - rr.y0) / rows;
            json tiles = json::array();
            for (size_t t = 0; t < cs->aovs.size(); ++t) {
                const double x0 = rr.x0 + (t % cs->cols) * tw, y0 = rr.y0 + (t / cs->cols) * th;
                tiles.push_back({{"name", cs->aovs[t].displayName}, {"aov", int(t)}, {"rect", {x0, y0, x0 + tw, y0 + th}}});
            }
            j[s == 0 ? "tilesA" : "tilesB"] = tiles;
        }
        return j;
    }

    // Pixel value(s) under a viewport position, for both layers.
    json probeJson(double px, double py) {
        const Layout L = layout(vpW, vpH, view);
        json out = json::object();
        for (int s = 0; s < 2; ++s) {
            const bool show = s == 0 ? L.showA : L.showB;
            const Slot& sl = slots[s];
            if (!show || !sl.loaded) continue;
            const PixelRect& rr = s == 0 ? L.a : L.b;
            const int lf = layerFrame(s, ctrl.current());
            FramePtr f = inRange(s, lf) ? lookup(s, lf) : nullptr;
            if (!f || f->missing) continue;
            auto p = rectToImage(rr, px, py, f->fullWidth, f->fullHeight);
            if (!p) continue;
            const int sx = f->fullWidth ? p->first * f->width / f->fullWidth : 0;
            const int sy = f->fullHeight ? p->second * f->height / f->fullHeight : 0;
            json v;
            v["x"] = p->first;
            v["y"] = p->second;
            float c[4];
            for (int k = 0; k < 4; ++k) c[k] = f->sample(sx, sy, k);
            auto num = [](float x) { return std::isfinite(x) ? json(x) : json(std::isnan(x) ? "NaN" : (x > 0 ? "+Inf" : "-Inf")); };
            v["rgba"] = {num(c[0]), num(c[1]), num(c[2]), num(c[3])};
            const LayerInfo* l = sl.currentAov();
            if (l && l->kind == AovKind::Crypto && sl.effectiveAov() == sl.aov && f->format == PixelFormat::Float) {
                std::string base = l->displayName;
                while (!base.empty() && std::isdigit(static_cast<unsigned char>(base.back()))) base.pop_back();
                auto m = sl.info.cryptoManifests.find(base);
                json names = json::array();
                for (int k : {0, 2}) {
                    uint32_t bits;
                    std::memcpy(&bits, &c[k], 4);
                    std::string name;
                    if (m != sl.info.cryptoManifests.end()) {
                        auto it = m->second.find(bits);
                        if (it != m->second.end()) name = it->second;
                    }
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "%08x", bits);
                    names.push_back({{"id", hex}, {"bits", bits}, {"name", name}, {"coverage", c[k + 1]}});
                }
                v["crypto"] = names;
            }
            out[s == 0 ? "a" : "b"] = v;
        }
        return out;
    }

    json regionJson(double x0, double y0, double x1, double y1) {
        const Layout L = layout(vpW, vpH, view);
        json out = json::object();
        for (int s = 0; s < 2; ++s) {
            const bool show = s == 0 ? L.showA : L.showB;
            if (!show || !slots[s].loaded) continue;
            const PixelRect& rr = s == 0 ? L.a : L.b;
            const int lf = layerFrame(s, ctrl.current());
            FramePtr f = inRange(s, lf) ? lookup(s, lf) : nullptr;
            if (!f || f->missing) continue;
            auto toStored = [&](double px, double py, int& ox, int& oy) {
                ox = int(std::floor((px - rr.x0) / (rr.x1 - rr.x0) * f->width));
                oy = int(std::floor((py - rr.y0) / (rr.y1 - rr.y0) * f->height));
            };
            int ax, ay, bx, by;
            toStored(x0, y0, ax, ay);
            toStored(x1, y1, bx, by);
            const RegionStats st = regionStats(*f, ax, ay, bx, by);
            out[s == 0 ? "a" : "b"] = {{"count", st.count}, {"mean", st.mean}, {"min", st.min}, {"max", st.max},
                                       {"nan", st.nan}, {"inf", st.inf}};
        }
        return out;
    }

    // Viewport pixel -> normalised image position of whichever layer is under it (A first).
    std::optional<std::pair<double, double>> viewportToNormalised(const Layout& L, double px, double py) const {
        for (int s = 0; s < 2; ++s) {
            const bool show = s == 0 ? L.showA : L.showB;
            if (!show || !slots[s].loaded) continue;
            const PixelRect& rr = s == 0 ? L.a : L.b;
            const double u = (px - rr.x0) / (rr.x1 - rr.x0), v = (py - rr.y0) / (rr.y1 - rr.y0);
            if (u >= 0 && v >= 0 && u < 1 && v < 1) return std::make_pair(u, v);
        }
        return std::nullopt;
    }

    // Converts linear values of a slot to what the viewer shows (exposure + OCIO), for swatches.
    json displaySwatch(int s, const float rgba[4]) {
        float px[4] = {rgba[0], rgba[1], rgba[2], 1.0f};
        const float gain = std::exp2(exposureStops);
        for (int c = 0; c < 3; ++c) px[c] = std::isfinite(px[c]) ? px[c] * gain : 0.0f;
        const float luma = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
        for (int c = 0; c < 3; ++c) px[c] = luma + (px[c] - luma) * saturation;  // as the viewer shows it
        if (slots[s].mode() == LayerMode::Color) {
            try {
                ocio.toDisplayCpu(slots[s].inputSpace, px, 1, 1);
            } catch (...) {
            }
        }
        return {std::clamp(px[0], 0.0f, 1.0f), std::clamp(px[1], 0.0f, 1.0f), std::clamp(px[2], 0.0f, 1.0f)};
    }

    // Pixel Analyzer: current pixel (optional x,y in viewport pixels) and the pinned region.
    json analyzerJson(std::optional<std::pair<double, double>> at, bool withRegion = true) {
        const Layout L = layout(vpW, vpH, view);
        json out = json::object();
        out["regionActive"] = regionActive;
        auto num = [](double x) { return std::isfinite(x) ? json(x) : json(std::isnan(x) ? "NaN" : (x > 0 ? "+Inf" : "-Inf")); };
        for (int s = 0; s < 2; ++s) {
            const bool show = s == 0 ? L.showA : L.showB;
            const Slot& sl = slots[s];
            if (!show || !sl.loaded) continue;
            const PixelRect& rr = s == 0 ? L.a : L.b;
            const int lf = layerFrame(s, ctrl.current());
            FramePtr f = inRange(s, lf) ? lookup(s, lf) : nullptr;
            if (!f || f->missing) continue;
            json lj;
            if (at) {
                auto p = rectToImage(rr, at->first, at->second, f->fullWidth, f->fullHeight);
                if (p) {
                    const int sx = p->first * f->width / std::max(1, f->fullWidth);
                    const int sy = p->second * f->height / std::max(1, f->fullHeight);
                    float c[4];
                    for (int k = 0; k < 4; ++k) c[k] = f->sample(sx, sy, k);
                    lj["pixel"] = {{"x", p->first}, {"y", p->second}, {"rgba", {num(c[0]), num(c[1]), num(c[2]), num(c[3])}},
                                   {"display", displaySwatch(s, c)}};
                }
            }
            if (regionActive && withRegion) {
                const int x0 = int(std::floor(regU0 * f->width)), y0 = int(std::floor(regV0 * f->height));
                const int x1 = std::max(x0, int(std::ceil(regU1 * f->width)) - 1);
                const int y1 = std::max(y0, int(std::ceil(regV1 * f->height)) - 1);
                const RegionStats st = regionStats(*f, x0, y0, x1, y1);
                const float mean[4] = {float(st.mean[0]), float(st.mean[1]), float(st.mean[2]), float(st.mean[3])};
                json arr[4];
                for (int k = 0; k < 4; ++k) arr[k] = json::array();
                for (int k = 0; k < 4; ++k) {
                    arr[0].push_back(num(st.min[k]));
                    arr[1].push_back(num(st.max[k]));
                    arr[2].push_back(num(st.mean[k]));
                    arr[3].push_back(num(st.median[k]));
                }
                lj["region"] = {{"x", int(std::floor(regU0 * f->fullWidth))},
                                {"y", int(std::floor(regV0 * f->fullHeight))},
                                {"w", std::max(1, int(std::lround((regU1 - regU0) * f->fullWidth)))},
                                {"h", std::max(1, int(std::lround((regV1 - regV0) * f->fullHeight)))},
                                {"count", st.count}, {"nan", st.nan}, {"inf", st.inf},
                                {"min", arr[0]}, {"max", arr[1]}, {"mean", arr[2]}, {"median", arr[3]},
                                {"display", displaySwatch(s, mean)}};
            }
            out[s == 0 ? "a" : "b"] = lj;
        }
        return out;
    }

    json statsJson() {
        json out = json::object();
        for (int s = 0; s < 2; ++s) {
            if (!slots[s].loaded) continue;
            const int lf = layerFrame(s, ctrl.current());
            FramePtr f = inRange(s, lf) ? lookup(s, lf) : nullptr;
            if (!f || f->missing) continue;
            json chans = json::array();
            for (const auto& c : f->stats())
                chans.push_back({{"min", c.min}, {"max", c.max}, {"mean", c.mean}, {"nan", c.nan}, {"inf", c.inf},
                                 {"negative", c.negative}});
            out[s == 0 ? "a" : "b"] = chans;
        }
        return out;
    }

    void syncLockedAov(int changedSlot) {
        if (!lockAov) return;
        const Slot& src = slots[changedSlot];
        Slot& dst = slots[1 - changedSlot];
        if (!src.loaded || !dst.loaded || !src.currentAov()) return;
        for (size_t i = 0; i < dst.aovs.size(); ++i)
            if (dst.aovs[i].displayName == src.currentAov()->displayName && int(i) != dst.aov) {
                dst.aov = int(i);
                dst.isolate = 0;
                dst.hasIsolate = false;
                if (!dst.alpha.empty()) dst.invalidate();
            }
    }

    // ------------------------------------------------------------------ dispatch --
    json dispatch(const json& j) {
        const std::string cmd = j.value("cmd", "");
        auto slotArg = [&]() {
            const int s = j.value("slot", 0);
            if (s < 0 || s > 1) throw std::runtime_error("slot must be 0 (A) or 1 (B)");
            return s;
        };
        const TimePoint now = Clock::now();

        // ---------------- queries
        if (cmd == "status") return statusJson();
        if (cmd == "layer") return slotJson(slotArg());
        if (cmd == "ocio") return ocioJson();
        if (cmd == "geometry") return geometryJson();
        if (cmd == "probe") return probeJson(j.value("x", 0.0), j.value("y", 0.0));
        if (cmd == "region") return regionJson(j.value("x0", 0.0), j.value("y0", 0.0), j.value("x1", 0.0), j.value("y1", 0.0));
        if (cmd == "stats") return statsJson();
        if (cmd == "analyzer") {
            std::optional<std::pair<double, double>> at;
            if (j.contains("x") && j.contains("y")) at = std::make_pair(j["x"].get<double>(), j["y"].get<double>());
            return analyzerJson(at, j.value("region", true));
        }
        if (cmd == "sample_region" || cmd == "sample_pixel") {
            const Layout L = layout(vpW, vpH, view);
            auto a = viewportToNormalised(L, j.value("x0", j.value("x", 0.0)), j.value("y0", j.value("y", 0.0)));
            if (cmd == "sample_pixel") {
                // Snap to the pixel under the pointer of the layer that was clicked.
                if (!a) return {{"regionActive", regionActive}};
                const Slot& ref = slots[0].loaded && L.showA ? slots[0] : slots[1];
                const double w = std::max(1, ref.info.width), h = std::max(1, ref.info.height);
                regU0 = std::floor(a->first * w) / w;
                regV0 = std::floor(a->second * h) / h;
                regU1 = regU0 + 1.0 / w;
                regV1 = regV0 + 1.0 / h;
                regionActive = true;
            } else {
                const PixelRect& rr = L.showA ? L.a : L.b;
                auto clampU = [&](double px) { return std::clamp((px - rr.x0) / (rr.x1 - rr.x0), 0.0, 1.0); };
                auto clampV = [&](double py) { return std::clamp((py - rr.y0) / (rr.y1 - rr.y0), 0.0, 1.0); };
                const double u0 = clampU(j.value("x0", 0.0)), u1 = clampU(j.value("x1", 0.0));
                const double v0 = clampV(j.value("y0", 0.0)), v1 = clampV(j.value("y1", 0.0));
                regU0 = std::min(u0, u1);
                regU1 = std::max(u0, u1);
                regV0 = std::min(v0, v1);
                regV1 = std::max(v0, v1);
                regionActive = regU1 > regU0 && regV1 > regV0;
            }
            return analyzerJson(std::nullopt);
        }
        if (cmd == "clear_region") {
            regionActive = false;
            return {{"regionActive", false}};
        }
        if (cmd == "metadata") {
            const int s = slotArg();
            json rows = json::array();
            if (slots[s].loaded)
                for (const auto& e : slots[s].info.metadata)
                    rows.push_back({{"part", e.part}, {"name", e.name}, {"type", e.type}, {"value", e.value}});
            return {{"metadata", rows}};
        }
        if (cmd == "cached") {
            const int s = slotArg();
            json frames = json::array();
            if (slots[s].loaded)
                if (auto src = source(s, slots[s].effectiveAov()))
                    for (int f : cache.cachedFrames(src->key)) frames.push_back(f + slots[s].offset);
            return {{"frames", frames}};
        }

        // ---------------- layers
        if (cmd == "open") {
            const int s = slotArg();
            std::string err;
            json r = openSlot(s, fromUtf8(j.value("path", "")), false, &err);
            if (r.is_null()) throw std::runtime_error(err.empty() ? "Could not open" : err);
            return r;
        }
        if (cmd == "close") {
            const int s = slotArg();
            Slot fresh;
            fresh.generation = slots[s].generation + 1;
            slots[s] = std::move(fresh);
            if (compare != CompareMode::A && compare != CompareMode::B) compare = slots[0].loaded ? CompareMode::A : CompareMode::B;
            recomputeRange();
            refreshRequests(true);
            return statusJson();
        }
        if (cmd == "version") {
            const int s = slotArg();
            if (!slots[s].loaded) throw std::runtime_error("Layer not loaded");
            auto next = findVersionSibling(*slots[s].seq, j.value("delta", 1));
            if (!next) throw std::runtime_error("No " + std::string(j.value("delta", 1) > 0 ? "newer" : "older") + " version found");
            std::string err;
            json r = openSlot(s, *next, true, &err);
            if (r.is_null()) throw std::runtime_error(err);
            return r;
        }
        if (cmd == "swap") {
            std::swap(slots[0], slots[1]);
            slots[0].invalidate();
            slots[1].invalidate();
            refreshRequests(true);
            return statusJson();
        }
        if (cmd == "aov") {
            const int s = slotArg();
            Slot& sl = slots[s];
            if (!sl.loaded) throw std::runtime_error("Layer not loaded");
            int index = sl.aov;
            if (j.contains("index")) index = j["index"].get<int>();
            if (j.contains("name"))
                for (size_t i = 0; i < sl.aovs.size(); ++i)
                    if (sl.aovs[i].displayName == j["name"].get<std::string>()) index = int(i);
            if (j.contains("delta")) index = std::clamp(sl.aov + j["delta"].get<int>(), 0, int(sl.aovs.size()) - 1);
            if (index < 0 || index >= int(sl.aovs.size())) throw std::runtime_error("No such AOV");
            if (index != sl.aov) {
                sl.aov = index;
                sl.isolate = 0;
                sl.hasIsolate = false;
                if (!sl.alpha.empty()) sl.invalidate();  // alpha override belongs to the AOV
            }
            sl.contactSheet = false;
            syncLockedAov(s);
            refreshRequests(true);
            return slotJson(s);
        }
        if (cmd == "layer_set") {
            const int s = slotArg();
            Slot& sl = slots[s];
            bool invalidate = false, range = false;
            if (j.contains("opacity")) sl.opacity = std::clamp(j["opacity"].get<float>(), 0.0f, 1.0f);
            if (j.contains("visible")) sl.visible = j["visible"].get<bool>();
            if (j.contains("offset")) {
                sl.offset = j["offset"].get<int>();
                range = true;
            }
            if (j.contains("inputSpace")) sl.inputSpace = j["inputSpace"].get<std::string>();
            if (j.contains("mode")) sl.modeSetting = j["mode"].get<std::string>();
            if (j.contains("depthAuto")) sl.depthAuto = j["depthAuto"].get<bool>();
            if (j.contains("depthNear")) sl.depthNear = j["depthNear"].get<float>();
            if (j.contains("depthFar")) sl.depthFar = j["depthFar"].get<float>();
            if (j.contains("depthInvert")) sl.depthInvert = j["depthInvert"].get<bool>();
            if (j.contains("motionScale")) sl.motionScale = j["motionScale"].get<float>();
            if (j.contains("alpha")) {
                sl.alpha = j["alpha"].get<std::string>();
                invalidate = true;
            }
            if (j.contains("isolate")) {  // cryptomatte/ID isolate: bits of the ID, or null to clear
                sl.hasIsolate = !j["isolate"].is_null();
                sl.isolate = sl.hasIsolate ? j["isolate"].get<uint32_t>() : 0;
            }
            if (j.contains("contactSheet")) sl.contactSheet = j["contactSheet"].get<bool>();
            if (j.contains("rebuild")) sl.rebuild = j["rebuild"].get<int>();
            if (j.contains("rebuildAovs")) {
                sl.rebuildAovs = j["rebuildAovs"].get<std::vector<std::string>>();
                sl.sources.erase(kAovRebuildDiff);
                sl.sources.erase(kAovRebuildSum);
                ++sl.generation;  // new keys: old rebuild frames are stale
            }
            if (j.contains("rebuildBeauty")) {
                sl.rebuildBeauty = j["rebuildBeauty"].get<std::string>();
                sl.sources.erase(kAovRebuildDiff);
                sl.sources.erase(kAovRebuildSum);
                ++sl.generation;
            }
            if (j.contains("view") && sl.loaded) {  // stereo view: re-read the AOV list
                sl.view = j["view"].get<std::string>();
                std::string err;
                if (openSlot(s, sl.seq->files.front().second, true, &err).is_null()) throw std::runtime_error(err);
            }
            if (invalidate) sl.invalidate();
            if (range) recomputeRange();
            refreshRequests(true);
            return slotJson(s);
        }
        if (cmd == "lock_aov") {
            lockAov = j.value("on", true);
            syncLockedAov(0);
            refreshRequests(true);
            return statusJson();
        }

        // ---------------- transport
        if (cmd == "play") ctrl.play(j.value("direction", 1), now);
        else if (cmd == "pause") ctrl.pause();
        else if (cmd == "toggle") {
            if (ctrl.playing())
                ctrl.pause();
            else
                ctrl.play(ctrl.direction(), now);
        } else if (cmd == "step") ctrl.step(j.value("delta", 1));
        else if (cmd == "seek") ctrl.seek(j.value("frame", ctrl.current()), now);
        else if (cmd == "start") ctrl.goToStart(now);
        else if (cmd == "end") ctrl.goToEnd(now);
        else if (cmd == "loop") ctrl.setLoop(j.value("on", true));
        else if (cmd == "inout") ctrl.setInOut(j.value("in", ctrl.inPoint()), j.value("out", ctrl.outPoint()));
        else if (cmd == "clear_inout") ctrl.clearInOut();
        else if (cmd == "fps") ctrl.setFrameRate(Rational{j.value("num", int64_t(24)), j.value("den", int64_t(1))}, now);
        else if (cmd == "play_all_frames") ctrl.setMode(j.value("on", false) ? PlaybackMode::PlayAllFrames : PlaybackMode::Realtime, now);
        // ---------------- cache
        else if (cmd == "cache") {
            if (j.contains("mode")) {
                const std::string m = j["mode"].get<std::string>();
                cacheMode = m == "off" ? CacheMode::Off : m == "region" ? CacheMode::Region : CacheMode::LookAhead;
            }
            if (j.contains("budget")) budget = j["budget"].get<size_t>();
            cache.setBudget(cacheMode == CacheMode::Off ? 0 : budget);
        } else if (cmd == "proxy") {
            proxy = j.value("on", false);
            slots[0].invalidate();
            slots[1].invalidate();
        }
        // ---------------- view
        else if (cmd == "view") {
            const std::string action = j.value("action", "fit");
            const Layout L = layout(vpW, vpH, view);
            if (action == "fit") {
                view = ViewState{};
            } else if (action == "1:1") {
                view = ViewState{false, 1.0, 0.0, 0.0};
            } else if (action == "zoom") {
                view = zoomAbout(view, j.value("factor", 1.0), j.value("x", vpW / 2.0), j.value("y", vpH / 2.0), vpW, vpH,
                                 std::max(1, L.canvasW), std::max(1, L.canvasH), 1.0);
            } else if (action == "pan") {
                if (view.fit) {
                    view.zoom = effectiveZoom(view, vpW, vpH, std::max(1, L.canvasW), std::max(1, L.canvasH), 1.0);
                    view.fit = false;
                    view.panX = view.panY = 0;
                }
                view.panX += j.value("dx", 0.0);
                view.panY += j.value("dy", 0.0);
            }
            return geometryJson();
        } else if (cmd == "compare") {
            if (j.contains("mode")) {
                auto m = compareModeFromName(j["mode"].get<std::string>());
                if (!m) throw std::runtime_error("Unknown compare mode");
                compare = *m;
            }
            if (j.contains("align")) align = j["align"].get<std::string>() == "pixel" ? AlignMode::Pixel : AlignMode::Fit;
            if (j.contains("diffGain")) diffGain = j["diffGain"].get<float>();
        } else if (cmd == "wipe") {
            if (j.contains("x")) wipeX = j["x"].get<float>();
            if (j.contains("y")) wipeY = j["y"].get<float>();
            if (j.contains("angle")) wipeAngle = float(std::remainder(j["angle"].get<double>(), 2 * kPi));
            if (j.contains("mix")) wipeMix = std::clamp(j["mix"].get<float>(), 0.0f, 1.0f);
            if (j.contains("softness")) wipeSoft = std::max(0.0f, j["softness"].get<float>());
            if (j.value("reset", false)) {
                wipeX = wipeY = 0.5f;
                wipeAngle = wipeMix = wipeSoft = 0.0f;
            }
            return json::object();  // sent on every pointer move while dragging: keep it cheap
        }
        // ---------------- colour
        else if (cmd == "color") {
            if (j.contains("exposure")) exposureStops = j["exposure"].get<float>();
            if (j.contains("gamma")) gamma = j["gamma"].get<float>();
            if (j.contains("saturation")) saturation = std::clamp(j["saturation"].get<float>(), 0.0f, 4.0f);
            if (j.contains("channel")) channel = std::clamp(j["channel"].get<int>(), 0, 5);
            if (j.contains("falseColor")) falseColor = j["falseColor"].get<bool>();
            if (j.contains("nanHighlight")) nanHighlight = j["nanHighlight"].get<bool>();
            if (j.contains("matte")) matte = j["matte"].get<bool>();
            if (j.contains("diffGain")) diffGain = j["diffGain"].get<float>();
            if (j.contains("display") || j.contains("view") || j.contains("look")) {
                std::string err;
                if (!ocio.setDisplayView(j.value("display", ocio.display()), j.value("view", ocio.view()),
                                         j.value("look", ocio.look()), &err))
                    throw std::runtime_error(err);
            }
            return {{"color", colorJson()}, {"views", ocio.views(ocio.display())}};
        } else if (cmd == "ocio_load") {
            std::string err;
            if (!ocio.load(j.value("path", ""), &err)) throw std::runtime_error(err);
            ocioError.clear();
            for (int s = 0; s < 2; ++s)
                if (slots[s].loaded)  // new workflow: each layer starts in its linear working space
                    slots[s].inputSpace = ocio.guessInputSpace(slots[s].seq->files.front().second);
            return ocioJson();
        } else {
            throw std::runtime_error("Unknown command: " + cmd);
        }
        refreshRequests(false);
        return statusJson();
    }
};

// ============================================================================ Session ==

Session::Session(SessionConfig config) : impl_(std::make_unique<Impl>(config)) {}
Session::~Session() = default;

std::string Session::call(const std::string& text) {
    json result;
    try {
        const json j = json::parse(text);
        std::lock_guard lock(impl_->m);
        json r = impl_->dispatch(j);
        result = r.is_object() ? r : json{{"result", r}};
        result["ok"] = true;
    } catch (const std::exception& e) {
        result = {{"ok", false}, {"error", e.what()}};
    }
    return result.dump(-1, ' ', false, json::error_handler_t::replace);
}

RenderState Session::update(TimePoint now, int w, int h) {
    std::lock_guard lock(impl_->m);
    Impl& I = *impl_;
    I.vpW = std::max(1, w);
    I.vpH = std::max(1, h);
    RenderState rs;
    if (!I.anyLoaded()) return rs;
    rs.hasContent = true;

    I.ctrl.update(now, [&](int g) { return I.readyAt(g); });
    I.refreshRequests(false);
    const int g = I.ctrl.current();
    rs.globalFrame = g;
    const Layout L = I.layout(I.vpW, I.vpH, I.view);
    I.fillLayer(rs.a, 0, g, L.a, L.showA, false);
    I.fillLayer(rs.b, 1, g, L.b, L.showB, false);
    I.fillCommon(rs, I.vpW, I.vpH);

    const bool ready = (!rs.a.present || rs.a.frame) && (!rs.b.present || rs.b.frame);
    if (I.ctrl.playing() && ready && g != I.lastShown) I.shown.push_back(now);
    if (ready) I.lastShown = g;
    while (!I.shown.empty() && now - I.shown.front() > std::chrono::seconds(1)) I.shown.pop_front();
    if (!I.ctrl.playing()) I.shown.clear();
    return rs;
}

RenderState Session::stillState(int w, int h) {
    std::lock_guard lock(impl_->m);
    Impl& I = *impl_;
    RenderState rs;
    if (!I.anyLoaded()) return rs;
    rs.hasContent = true;
    const int g = I.ctrl.current();
    rs.globalFrame = g;
    const Layout L = I.layout(w, h, ViewState{});
    I.fillLayer(rs.a, 0, g, L.a, L.showA, true);
    I.fillLayer(rs.b, 1, g, L.b, L.showB, true);
    // The wipe line keeps its relative position.
    I.fillCommon(rs, w, h);
    return rs;
}

bool Session::canvasSize(int& w, int& h) {
    std::lock_guard lock(impl_->m);
    if (!impl_->anyLoaded()) return false;
    const Layout L = impl_->layout(1000, 1000, ViewState{});
    w = std::max(1, L.canvasW);
    h = std::max(1, L.canvasH);
    return true;
}

void Session::waitForDecodes() { impl_->scheduler->waitIdle(); }

}  // namespace exrc
