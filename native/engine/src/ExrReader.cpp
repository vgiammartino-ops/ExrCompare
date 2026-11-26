#include "exrc/ExrReader.h"

#include <ImfAttribute.h>
#include <ImfBoxAttribute.h>
#include <ImfChannelList.h>
#include <ImfChannelListAttribute.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfCompressionAttribute.h>
#include <ImfDoubleAttribute.h>
#include <ImfFloatAttribute.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfIntAttribute.h>
#include <ImfLineOrderAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfMultiPartInputFile.h>
#include <ImfMultiView.h>  // hasMultiView / multiView
#include <ImfPartType.h>
#include <ImfRationalAttribute.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>
#include <ImfStringVectorAttribute.h>
#include <ImfThreading.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfTiledInputPart.h>
#include <ImfTimeCodeAttribute.h>
#include <ImfVecAttribute.h>
#include <half.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace exrc {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// OpenEXR treats char* filenames as UTF-8 on Windows; path::string() would use the ANSI code page.
std::string pathToUtf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string compressionName(Imf::Compression c) {
    switch (c) {
        case Imf::NO_COMPRESSION: return "None";
        case Imf::RLE_COMPRESSION: return "RLE";
        case Imf::ZIPS_COMPRESSION: return "ZIPS";
        case Imf::ZIP_COMPRESSION: return "ZIP";
        case Imf::PIZ_COMPRESSION: return "PIZ";
        case Imf::PXR24_COMPRESSION: return "PXR24";
        case Imf::B44_COMPRESSION: return "B44";
        case Imf::B44A_COMPRESSION: return "B44A";
        case Imf::DWAA_COMPRESSION: return "DWAA";
        case Imf::DWAB_COMPRESSION: return "DWAB";
        default: return "Other (" + std::to_string(int(c)) + ")";
    }
}

std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

struct Chan {
    std::string base;  // name after the last '.'
    std::string full;  // full channel name in the file
};

std::string joinBases(const std::vector<Chan>& cs) {
    std::string s;
    for (const auto& c : cs) s += (s.empty() ? "" : ",") + c.base;
    return s;
}

void finishLayer(LayerInfo& L) {
    if (L.kind == AovKind::Color) L.kind = classifyAov(L.displayName);
    L.isData = L.kind != AovKind::Color;
}

// Turns one channel group (e.g. everything named "diffuse.*") into one or more layers.
void addGroupLayers(std::vector<LayerInfo>& out, int part, const std::string& groupName, const std::vector<Chan>& chans) {
    auto find = [&](std::initializer_list<const char*> names) -> const Chan* {
        for (const auto& c : chans) {
            const std::string lb = toLower(c.base);
            for (const char* n : names)
                if (lb == n) return &c;
        }
        return nullptr;
    };
    std::set<std::string> used;
    bool made = false;

    const Chan* r = find({"r", "red"});
    const Chan* g = find({"g", "green"});
    const Chan* b = find({"b", "blue"});
    const Chan* a = find({"a", "alpha"});
    if (r || g || b) {
        LayerInfo L;
        L.part = part;
        L.displayName = groupName.empty() ? (a ? "RGBA" : "RGB") : groupName;
        std::vector<const Chan*> rgb;
        for (const Chan* c : {r, g, b})
            if (c) rgb.push_back(c);
        if (rgb.size() == 1) {  // e.g. a mask stored only in .R
            L.source[0] = rgb[0]->full;
            L.replicateFirst = true;
        } else {
            L.source[0] = r ? r->full : "";
            L.source[1] = g ? g->full : "";
            L.source[2] = b ? b->full : "";
        }
        if (a) L.source[3] = a->full;
        std::vector<Chan> summary;
        for (const Chan* c : {r, g, b, a})
            if (c) {
                used.insert(c->full);
                summary.push_back(*c);
            }
        L.channelCount = int(summary.size());
        L.channelSummary = joinBases(summary);
        if (rgb.size() == 1 && a) L.replicateFirst = true;  // R + A: still greyscale colour, alpha kept
        finishLayer(L);
        out.push_back(std::move(L));
        made = true;
    } else {
        const Chan* x = find({"x"});
        const Chan* y = find({"y"});
        const Chan* z = find({"z"});
        const Chan* u = find({"u"});
        const Chan* v = find({"v"});
        if (int(!!x) + int(!!y) + int(!!z) >= 2) {  // normals, positions, vectors
            LayerInfo L;
            L.part = part;
            L.displayName = groupName.empty() ? "XYZ" : groupName;
            L.source = {x ? x->full : "", y ? y->full : "", z ? z->full : "", ""};
            L.kind = classifyAov(L.displayName);
            if (L.kind == AovKind::Color || L.kind == AovKind::Depth) L.kind = AovKind::Generic;
            std::vector<Chan> summary;
            for (const Chan* c : {x, y, z})
                if (c) {
                    used.insert(c->full);
                    summary.push_back(*c);
                }
            L.channelCount = int(summary.size());
            L.channelSummary = joinBases(summary);
            finishLayer(L);
            out.push_back(std::move(L));
            made = true;
        } else if (u && v) {
            LayerInfo L;
            L.part = part;
            L.displayName = groupName.empty() ? "UV" : groupName;
            L.source = {u->full, v->full, "", ""};
            L.kind = AovKind::UV;
            L.channelCount = 2;
            L.channelSummary = u->base + "," + v->base;
            used.insert(u->full);
            used.insert(v->full);
            finishLayer(L);
            out.push_back(std::move(L));
            made = true;
        }
    }

    std::vector<Chan> rest;
    for (const auto& c : chans)
        if (!used.count(c.full)) rest.push_back(c);
    if (rest.empty()) return;

    if (!made && rest.size() >= 2 && rest.size() <= 4) {
        LayerInfo L;  // unknown naming: map the first channels to R,G,B,A in order
        L.part = part;
        L.displayName = groupName.empty() ? joinBases(rest) : groupName;
        for (size_t i = 0; i < rest.size(); ++i) L.source[i] = rest[i].full;
        L.channelCount = int(rest.size());
        L.channelSummary = joinBases(rest);
        finishLayer(L);
        out.push_back(std::move(L));
        return;
    }
    for (const auto& c : rest) {  // each leftover channel becomes a greyscale layer
        LayerInfo L;
        L.part = part;
        if (!made && rest.size() == 1)
            L.displayName = groupName.empty() ? c.base : groupName;
        else
            L.displayName = groupName.empty() ? c.base : groupName + "." + c.base;
        L.source[0] = c.full;
        L.replicateFirst = true;
        L.channelCount = 1;
        L.channelSummary = c.base;
        L.kind = classifyAov(L.displayName);
        if (L.kind == AovKind::Color) L.kind = classifyAov(c.base);
        finishLayer(L);
        out.push_back(std::move(L));
    }
}

template <class T>
const T* attrAs(const Imf::Attribute& a) {
    return dynamic_cast<const T*>(&a);
}

std::string formatAttribute(const Imf::Attribute& a) {
    std::ostringstream o;
    if (auto s = attrAs<Imf::StringAttribute>(a)) return s->value();
    if (auto v = attrAs<Imf::IntAttribute>(a)) return std::to_string(v->value());
    if (auto v = attrAs<Imf::FloatAttribute>(a)) {
        o << v->value();
        return o.str();
    }
    if (auto v = attrAs<Imf::DoubleAttribute>(a)) {
        o << v->value();
        return o.str();
    }
    if (auto v = attrAs<Imf::V2iAttribute>(a)) {
        o << v->value().x << ", " << v->value().y;
        return o.str();
    }
    if (auto v = attrAs<Imf::V2fAttribute>(a)) {
        o << v->value().x << ", " << v->value().y;
        return o.str();
    }
    if (auto v = attrAs<Imf::V3fAttribute>(a)) {
        o << v->value().x << ", " << v->value().y << ", " << v->value().z;
        return o.str();
    }
    if (auto v = attrAs<Imf::Box2iAttribute>(a)) {
        const auto& b = v->value();
        o << "(" << b.min.x << ", " << b.min.y << ") - (" << b.max.x << ", " << b.max.y << ")";
        return o.str();
    }
    if (auto v = attrAs<Imf::Box2fAttribute>(a)) {
        const auto& b = v->value();
        o << "(" << b.min.x << ", " << b.min.y << ") - (" << b.max.x << ", " << b.max.y << ")";
        return o.str();
    }
    if (auto v = attrAs<Imf::CompressionAttribute>(a)) return compressionName(v->value());
    if (auto v = attrAs<Imf::LineOrderAttribute>(a)) {
        switch (v->value()) {
            case Imf::INCREASING_Y: return "increasing Y";
            case Imf::DECREASING_Y: return "decreasing Y";
            case Imf::RANDOM_Y: return "random Y";
            default: return "unknown";
        }
    }
    if (auto v = attrAs<Imf::ChromaticitiesAttribute>(a)) {
        const auto& c = v->value();
        o << "R(" << c.red.x << ", " << c.red.y << ") G(" << c.green.x << ", " << c.green.y << ") B(" << c.blue.x
          << ", " << c.blue.y << ") W(" << c.white.x << ", " << c.white.y << ")";
        return o.str();
    }
    if (auto v = attrAs<Imf::RationalAttribute>(a)) {
        o << v->value().n << "/" << v->value().d << " (" << double(v->value()) << ")";
        return o.str();
    }
    if (auto v = attrAs<Imf::TimeCodeAttribute>(a)) {
        const auto& t = v->value();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", t.hours(), t.minutes(), t.seconds(), t.frame());
        return buf;
    }
    if (auto v = attrAs<Imf::StringVectorAttribute>(a)) {
        for (size_t i = 0; i < v->value().size(); ++i) o << (i ? ", " : "") << v->value()[i];
        return o.str();
    }
    if (auto v = attrAs<Imf::M44fAttribute>(a)) {
        const auto& m = v->value();
        for (int r = 0; r < 4; ++r) {
            o << (r ? " | " : "");
            for (int c = 0; c < 4; ++c) o << (c ? " " : "") << m[r][c];
        }
        return o.str();
    }
    if (auto v = attrAs<Imf::ChannelListAttribute>(a)) {
        int n = 0;
        for (auto it = v->value().begin(); it != v->value().end(); ++it) ++n;
        return std::to_string(n) + " channels";
    }
    if (auto v = attrAs<Imf::TileDescriptionAttribute>(a)) {
        const auto& t = v->value();
        const char* mode = t.mode == Imf::ONE_LEVEL ? "one level" : t.mode == Imf::MIPMAP_LEVELS ? "mipmap" : "ripmap";
        o << t.xSize << " x " << t.ySize << ", " << mode;
        return o.str();
    }
    return std::string("<") + a.typeName() + ">";
}

// Strips the view name from a stereo channel name ("diffuse.left.R" -> "diffuse.R").
std::string stripView(const std::string& name, const std::string& view) {
    auto parts = splitDots(name);
    if (parts.size() >= 2 && parts[parts.size() - 2] == view) parts.erase(parts.end() - 2);
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? "." : "") + parts[i];
    return s;
}

// View a channel belongs to: the penultimate name component when it is a view name,
// otherwise the default (first) view.
std::string viewOfChannel(const std::string& name, const std::vector<std::string>& multiView) {
    const auto parts = splitDots(name);
    if (parts.size() >= 2) {
        const std::string& v = parts[parts.size() - 2];
        if (std::find(multiView.begin(), multiView.end(), v) != multiView.end()) return v;
    }
    return multiView.empty() ? std::string() : multiView.front();
}

// Part names of stereo multi-part files usually end in the view ("rgba.left").
std::string stripTrailingView(const std::string& name, const std::string& view) {
    auto parts = splitDots(name);
    if (parts.size() >= 2 && parts.back() == view) parts.pop_back();
    else return stripView(name, view);
    std::string s;
    for (size_t i = 0; i < parts.size(); ++i) s += (i ? "." : "") + parts[i];
    return s;
}

uint32_t parseHexId(const std::string& hex) {
    try {
        return uint32_t(std::stoul(hex, nullptr, 16));
    } catch (...) {
        return 0;
    }
}

}  // namespace

void setExrThreadCount(int threads) { Imf::setGlobalThreadCount(std::max(0, threads)); }

AovKind classifyAov(const std::string& name) {
    const std::string lower = toLower(name);
    auto has = [&](const char* s) { return lower.find(s) != std::string::npos; };
    std::set<std::string> tokens;
    std::string tok;
    for (size_t i = 0; i <= lower.size(); ++i) {
        const char c = i < lower.size() ? lower[i] : '\0';
        if (std::isalnum(static_cast<unsigned char>(c))) {
            tok += c;
        } else {
            if (!tok.empty()) tokens.insert(tok);
            tok.clear();
        }
    }
    auto tokenIs = [&](std::initializer_list<const char*> ts) {
        for (const char* t : ts)
            if (tokens.count(t)) return true;
        return false;
    };
    if (has("crypto")) return AovKind::Crypto;
    // Puzzle mattes ("PuzzleMatte_ObjectID", "PuzzleMatte_MaterialID") hold pure R/G/B masks.
    // Checked before the ID rule, which "ObjectID" would otherwise hit (random hash colours).
    if (has("puzzlematte") || tokenIs({"matte", "rgbmatte", "holdout"})) return AovKind::Matte;
    if (has("depth") || tokenIs({"z", "zdepth"})) return AovKind::Depth;
    if (has("normal") || tokenIs({"n", "nn", "nworld", "ncam"})) return AovKind::Normal;
    if (has("motion") || has("velocity") || tokenIs({"mv", "vel", "mvec"})) return AovKind::Motion;
    if (has("position") || has("pworld") || has("pref") || tokenIs({"p", "pos", "pcam"})) return AovKind::Position;
    if (tokenIs({"uv", "st"})) return AovKind::UV;
    if (has("objectid") || has("materialid") || tokenIs({"id", "objid", "matid"})) return AovKind::Id;
    return AovKind::Color;
}

std::vector<LayerInfo> buildLayers(const std::vector<PartChannels>& parts, const std::vector<std::string>& multiView,
                                   const std::string& view) {
    std::vector<LayerInfo> out;
    const bool multi = parts.size() > 1;
    const std::string wantView = view.empty() && !multiView.empty() ? multiView.front() : view;
    for (size_t pi = 0; pi < parts.size(); ++pi) {
        const PartChannels& p = parts[pi];
        if (p.deep) continue;  // deep data cannot be shown as a flat image
        if (!p.view.empty() && !wantView.empty() && p.view != wantView) continue;
        std::map<std::string, std::vector<Chan>> groups;  // sorted by prefix
        for (const auto& full : p.channels) {
            std::string shown = full;
            if (!multiView.empty()) {
                if (viewOfChannel(full, multiView) != wantView) continue;
                shown = stripView(full, wantView);
            }
            const auto dot = shown.rfind('.');
            const std::string prefix = dot == std::string::npos ? "" : shown.substr(0, dot);
            const std::string base = dot == std::string::npos ? shown : shown.substr(dot + 1);
            groups[prefix].push_back({base, full});
        }
        // Parts that only differ by view share a name; drop the view suffix from the label.
        std::string partName = p.partName;
        if (!p.view.empty()) partName = stripTrailingView(partName, p.view);
        for (const auto& [prefix, chans] : groups) {
            std::string name = prefix;
            if (multi && !partName.empty()) name = prefix.empty() ? partName : partName + "." + prefix;
            addGroupLayers(out, int(pi), name, chans);
        }
    }
    // The beauty (first un-prefixed RGB(A) colour AOV) always comes first, like RV and Nuke.
    auto beauty = std::find_if(out.begin(), out.end(), [](const LayerInfo& l) {
        return l.displayName == "RGBA" || l.displayName == "RGB";
    });
    if (beauty == out.end())
        beauty = std::find_if(out.begin(), out.end(), [](const LayerInfo& l) { return l.kind == AovKind::Color; });
    if (beauty != out.end()) std::rotate(out.begin(), beauty, beauty + 1);
    return out;
}

ImageInfo readImageInfo(const fs::path& file, const std::string& view, std::vector<LayerInfo>& layersOut) {
    Imf::MultiPartInputFile in(pathToUtf8(file).c_str());
    const Imf::Header& h = in.header(0);
    ImageInfo info;
    const auto& disp = h.displayWindow();
    const auto& dw = h.dataWindow();
    info.width = disp.max.x - disp.min.x + 1;
    info.height = disp.max.y - disp.min.y + 1;
    info.displayX0 = disp.min.x;
    info.displayY0 = disp.min.y;
    info.dataX0 = dw.min.x;
    info.dataY0 = dw.min.y;
    info.dataX1 = dw.max.x;
    info.dataY1 = dw.max.y;
    info.pixelAspect = h.pixelAspectRatio();
    info.compression = compressionName(h.compression());
    info.partCount = in.parts();
    if (h.hasTileDescription()) {
        info.tiled = true;
        info.mipmapped = h.tileDescription().mode != Imf::ONE_LEVEL;
    }
    if (Imf::hasFramesPerSecond(h)) {
        const Imf::Rational r = Imf::framesPerSecond(h);
        if (r.n > 0 && r.d > 0) info.fps = Rational{r.n, int64_t(r.d)};
    }
    if (Imf::hasChromaticities(h)) {
        const Imf::Chromaticities c = Imf::chromaticities(h);
        info.chromaticities = Chromaticities{c.red.x, c.red.y, c.green.x, c.green.y, c.blue.x, c.blue.y, c.white.x, c.white.y};
    }

    std::vector<std::string> multiView;
    if (Imf::hasMultiView(h)) multiView = Imf::multiView(h);
    std::set<std::string> partViews;

    std::map<std::string, std::string> cryptoNames, cryptoManifests;  // key -> name / manifest
    std::vector<PartChannels> parts;
    for (int i = 0; i < in.parts(); ++i) {
        const Imf::Header& ph = in.header(i);
        PartChannels pc;
        pc.partName = ph.hasName() ? ph.name() : "";
        pc.view = ph.hasView() ? ph.view() : "";
        if (!pc.view.empty()) partViews.insert(pc.view);
        pc.deep = ph.hasType() && (ph.type() == Imf::DEEPSCANLINE || ph.type() == Imf::DEEPTILE);
        for (auto it = ph.channels().begin(); it != ph.channels().end(); ++it) {
            pc.channels.push_back(it.name());
            info.allChannels.push_back(it.name());
        }
        for (auto it = ph.begin(); it != ph.end(); ++it) {
            const std::string name = it.name();
            const Imf::Attribute& attr = it.attribute();
            MetadataEntry e;
            e.part = i;
            e.name = name;
            e.type = attr.typeName();
            e.value = formatAttribute(attr);
            if (name.rfind("cryptomatte/", 0) == 0) {
                const auto slash = name.find('/', 12);
                if (slash != std::string::npos) {
                    const std::string key = name.substr(12, slash - 12);
                    const std::string field = name.substr(slash + 1);
                    if (field == "name") cryptoNames[key] = e.value;
                    if (field == "manifest") cryptoManifests[key] = e.value;
                }
            }
            if (e.value.size() > 4000) e.value = e.value.substr(0, 4000) + " ...";
            info.metadata.push_back(std::move(e));
        }
        parts.push_back(std::move(pc));
    }
    if (!multiView.empty())
        info.views = multiView;
    else
        info.views.assign(partViews.begin(), partViews.end());

    for (const auto& [key, cname] : cryptoNames) {
        auto m = cryptoManifests.find(key);
        if (m == cryptoManifests.end()) continue;
        try {
            const auto j = nlohmann::json::parse(m->second);
            auto& table = info.cryptoManifests[cname];
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_string()) table[parseHexId(it.value().get<std::string>())] = it.key();
        } catch (...) {
            // Unparseable manifest (or a sidecar manifest file): names just won't be shown.
        }
    }

    layersOut = buildLayers(parts, multiView, view);
    if (layersOut.empty()) throw std::runtime_error("No displayable channels in " + file.filename().string());
    return info;
}

FramePtr makeMissingFrame(int frameNumber, uint32_t key, std::string reason) {
    auto f = std::make_shared<Frame>();
    f->frameNumber = frameNumber;
    f->key = key;
    f->missing = true;
    f->error = std::move(reason);
    return f;
}

namespace {

inline float loadValue(const uint8_t* base, size_t index, PixelFormat fmt) {
    if (fmt == PixelFormat::Float) {
        float v;
        std::memcpy(&v, base + index * 4, 4);
        return v;
    }
    uint16_t bits;
    std::memcpy(&bits, base + index * 2, 2);
    half h;
    h.setBits(bits);
    return float(h);
}

inline void storeValue(uint8_t* base, size_t index, PixelFormat fmt, float v) {
    if (fmt == PixelFormat::Float) {
        std::memcpy(base + index * 4, &v, 4);
    } else {
        const uint16_t bits = half(v).bits();
        std::memcpy(base + index * 2, &bits, 2);
    }
}

}  // namespace

FramePtr decodeFrame(const fs::path& file, const LayerInfo& layer, int frameNumber, uint32_t key,
                     const DecodeOptions& options) {
    try {
        Imf::MultiPartInputFile in(pathToUtf8(file).c_str());
        if (layer.part >= in.parts()) return makeMissingFrame(frameNumber, key, "AOV part not in file");
        const Imf::Header& h = in.header(layer.part);
        const IMATH_NAMESPACE::Box2i disp = h.displayWindow();
        const IMATH_NAMESPACE::Box2i dw = h.dataWindow();
        const int fullW = disp.max.x - disp.min.x + 1;
        const int fullH = disp.max.y - disp.min.y + 1;

        const PixelFormat fmt = layer.isData ? PixelFormat::Float : PixelFormat::Half;
        const Imf::PixelType pt = fmt == PixelFormat::Float ? Imf::FLOAT : Imf::HALF;
        const size_t bps = fmt == PixelFormat::Float ? 4 : 2;
        std::array<std::string, 4> src = layer.source;
        if (!options.alphaOverride.empty()) src[3] = options.alphaOverride;
        const bool single = layer.replicateFirst && options.alphaOverride.empty() && src[3].empty();
        const int chans = single ? 1 : 4;

        // Read source: full-resolution data window, or mip level 1 for proxies of tiled files.
        const bool tiledMip = h.hasTileDescription() && h.tileDescription().mode != Imf::ONE_LEVEL;
        bool useLevel = false;
        IMATH_NAMESPACE::Box2i rb = dw;
        std::unique_ptr<Imf::TiledInputPart> tiled;
        if (options.proxy && tiledMip) {
            tiled = std::make_unique<Imf::TiledInputPart>(in, layer.part);
            if (tiled->numXLevels() > 1 && tiled->numYLevels() > 1) {
                useLevel = true;
                rb = tiled->dataWindowForLevel(1, 1);
            }
        }
        const int rw = rb.max.x - rb.min.x + 1;
        const int rh = rb.max.y - rb.min.y + 1;
        std::vector<uint8_t> buf(size_t(rw) * rh * chans * bps, 0);
        if (chans == 4)
            for (size_t i = 0; i < size_t(rw) * rh; ++i) storeValue(buf.data(), i * 4 + 3, fmt, 1.0f);

        Imf::FrameBuffer fb;
        const size_t xStride = chans * bps;
        const size_t yStride = xStride * size_t(rw);
        for (int c = 0; c < chans; ++c) {
            if (src[c].empty()) continue;
            const Imf::Channel* ch = h.channels().findChannel(src[c]);
            if (!ch || ch->xSampling != 1 || ch->ySampling != 1) continue;  // subsampled: unsupported
            fb.insert(src[c], Imf::Slice::Make(pt, buf.data() + c * bps, rb, xStride, yStride, 1, 1, c == 3 ? 1.0 : 0.0));
        }
        if (useLevel) {
            tiled->setFrameBuffer(fb);
            tiled->readTiles(0, tiled->numXTiles(1) - 1, 0, tiled->numYTiles(1) - 1, 1, 1);
        } else {
            Imf::InputPart part(in, layer.part);
            part.setFrameBuffer(fb);
            part.readPixels(rb.min.y, rb.max.y);
        }
        if (layer.replicateFirst && chans == 4) {  // greyscale + separate alpha
            for (size_t i = 0; i < size_t(rw) * rh; ++i) {
                std::memcpy(buf.data() + (i * 4 + 1) * bps, buf.data() + (i * 4) * bps, bps);
                std::memcpy(buf.data() + (i * 4 + 2) * bps, buf.data() + (i * 4) * bps, bps);
            }
        }

        auto frame = std::make_shared<Frame>();
        frame->frameNumber = frameNumber;
        frame->key = key;
        frame->fullWidth = fullW;
        frame->fullHeight = fullH;
        frame->channels = chans;
        frame->format = fmt;
        const int scale = options.proxy ? 2 : 1;
        frame->width = (fullW + scale - 1) / scale;
        frame->height = (fullH + scale - 1) / scale;

        if (scale == 1 && rb == disp) {  // common case: no copy
            frame->data = std::move(buf);
            return frame;
        }
        frame->data.assign(frame->rowBytes() * frame->height, 0);  // outside data window: transparent black

        if (scale == 1 || useLevel) {
            // Buffer already at output resolution: copy the overlapping rows.
            const int ox = useLevel ? (rb.min.x - disp.min.x) / 2 : rb.min.x - disp.min.x;  // buffer origin in output
            const int oy = useLevel ? (rb.min.y - disp.min.y) / 2 : rb.min.y - disp.min.y;
            const int x0 = std::max(0, ox), x1 = std::min(frame->width, ox + rw);
            const int y0 = std::max(0, oy), y1 = std::min(frame->height, oy + rh);
            for (int y = y0; y < y1 && x0 < x1; ++y) {
                const uint8_t* s = buf.data() + (size_t(y - oy) * rw + (x0 - ox)) * xStride;
                uint8_t* d = frame->data.data() + (size_t(y) * frame->width + x0) * xStride;
                std::memcpy(d, s, size_t(x1 - x0) * xStride);
            }
            return frame;
        }

        // Proxy without mip levels: 2x2 box filter of the full-resolution read.
        for (int y = 0; y < frame->height; ++y) {
            for (int x = 0; x < frame->width; ++x) {
                float acc[4] = {0, 0, 0, 0};
                for (int j = 0; j < 2; ++j) {
                    for (int i = 0; i < 2; ++i) {
                        const int bx = disp.min.x + 2 * x + i - rb.min.x;
                        const int by = disp.min.y + 2 * y + j - rb.min.y;
                        if (bx < 0 || by < 0 || bx >= rw || by >= rh) continue;
                        const size_t base = (size_t(by) * rw + bx) * chans;
                        for (int c = 0; c < chans; ++c) acc[c] += loadValue(buf.data(), base + c, fmt);
                    }
                }
                const size_t o = (size_t(y) * frame->width + x) * chans;
                for (int c = 0; c < chans; ++c) storeValue(frame->data.data(), o + c, fmt, acc[c] * 0.25f);
            }
        }
        return frame;
    } catch (const std::exception& e) {
        return makeMissingFrame(frameNumber, key, e.what());
    }
}

float halfToFloat(uint16_t bits) {
    half h;
    h.setBits(bits);
    return float(h);
}

uint16_t floatToHalf(float value) { return half(value).bits(); }

}  // namespace exrc
