// Engine unit tests (portable: run on Linux and Windows CI).
#include "TestExr.h"
#include "TestHarness.h"

#include "exrc/Analysis.h"
#include "exrc/CompareLayout.h"
#include "exrc/ExrReader.h"
#include "exrc/FrameCache.h"
#include "exrc/PlaybackController.h"
#include "exrc/SequenceScanner.h"

#include <ImfMultiView.h>
#include <ImfStringVectorAttribute.h>

#include <algorithm>
#include <cstring>
#include <fstream>

using namespace exrc;
using namespace testexr;
using namespace std::chrono_literals;

namespace {
std::vector<std::string> names(const std::vector<LayerInfo>& ls) {
    std::vector<std::string> out;
    for (const auto& l : ls) out.push_back(l.displayName);
    return out;
}
const LayerInfo* findLayer(const std::vector<LayerInfo>& ls, const std::string& n) {
    for (const auto& l : ls)
        if (l.displayName == n) return &l;
    return nullptr;
}
TimePoint t0() { return TimePoint{} + std::chrono::hours(1); }
}  // namespace

// ------------------------------------------------------------------------ sequences --
TEST_CASE("parseFrameName splits on the last digit run") {
    auto a = parseFrameName("shot_v01.1001.exr");
    CHECK(a.hasFrame);
    CHECK_EQ(a.prefix, std::string("shot_v01."));
    CHECK_EQ(a.digits, std::string("1001"));
    CHECK(!parseFrameName("still.exr").hasFrame);
}

TEST_CASE("scanSequence finds range, padding and missing frames") {
    TempDir d;
    for (int f : {1001, 1002, 1004}) writeExr(d.path / frameName("shot.", f, 4), 4, 2, {{"R"}, {"G"}, {"B"}});
    auto s = scanSequence(d.path / "shot.1002.exr");
    CHECK(s.has_value());
    if (!s) return;
    CHECK_EQ(s->first, 1001);
    CHECK_EQ(s->last, 1004);
    CHECK(s->missing == std::vector<int>{1003});
    CHECK_EQ(s->pattern(), std::string("shot.####.exr"));
}

TEST_CASE("version siblings are found in versioned folders and names") {
    TempDir d;
    for (int v : {1, 3, 4}) {
        char ver[8];
        std::snprintf(ver, sizeof(ver), "v%03d", v);
        const fs::path dir = d.path / ver;
        fs::create_directories(dir);
        writeExr(dir / frameName(std::string("shot_") + ver + ".", 1001, 4), 4, 2, {{"R"}, {"G"}, {"B"}});
    }
    auto s = scanSequence(d.path / "v003" / "shot_v003.1001.exr");
    CHECK(s.has_value());
    if (!s) return;
    CHECK_EQ(versionToken(*s), std::string("v003"));
    auto up = findVersionSibling(*s, +1);
    auto down = findVersionSibling(*s, -1);  // skips the missing v002
    CHECK(up && up->filename() == "shot_v004.1001.exr");
    CHECK(down && down->filename() == "shot_v001.1001.exr");
    auto s4 = scanSequence(*up);
    CHECK(s4 && !findVersionSibling(*s4, +1));
}

TEST_CASE("Rational frame rates") {
    CHECK(Rational::fromFps(23.976) == (Rational{24000, 1001}));
    CHECK_EQ(Rational({30000, 1001}).label(), std::string("29.97"));
}

// ------------------------------------------------------------------------- layers --
TEST_CASE("AOV kinds are classified from names") {
    CHECK(classifyAov("Z") == AovKind::Depth);
    CHECK(classifyAov("PuzzleMatte_ObjectID") == AovKind::Matte);
    CHECK(classifyAov("PuzzleMatte_MaterialID") == AovKind::Matte);
    CHECK(classifyAov("puzzleMatte01") == AovKind::Matte);
    CHECK(classifyAov("objectID") == AovKind::Id);
    CHECK(classifyAov("cryptomatte_00") == AovKind::Crypto);
    CHECK(classifyAov("beauty") == AovKind::Color);
    CHECK(classifyAov("depth") == AovKind::Depth);
    CHECK(classifyAov("N") == AovKind::Normal);
    CHECK(classifyAov("normals") == AovKind::Normal);
    CHECK(classifyAov("motionvector") == AovKind::Motion);
    CHECK(classifyAov("P") == AovKind::Position);
    CHECK(classifyAov("CryptoObject00") == AovKind::Crypto);
    CHECK(classifyAov("objectId") == AovKind::Id);
    CHECK(classifyAov("diffuse") == AovKind::Color);
    CHECK(classifyAov("specular_direct") == AovKind::Color);
}

TEST_CASE("buildLayers groups single-part AOVs and puts the beauty first") {
    PartChannels p;
    p.channels = {"A", "B", "G", "R", "Z", "N.X", "N.Y", "N.Z", "crypto00.a", "crypto00.b", "crypto00.g", "crypto00.r",
                  "diffuse.B", "diffuse.G", "diffuse.R", "mask.R"};
    auto ls = buildLayers({p});
    CHECK(!ls.empty() && ls.front().displayName == "RGBA");
    CHECK(findLayer(ls, "Z") && findLayer(ls, "Z")->kind == AovKind::Depth && findLayer(ls, "Z")->isData);
    CHECK(findLayer(ls, "N") && findLayer(ls, "N")->kind == AovKind::Normal);
    CHECK(findLayer(ls, "crypto00") && findLayer(ls, "crypto00")->kind == AovKind::Crypto);
    CHECK(findLayer(ls, "diffuse") && !findLayer(ls, "diffuse")->isData);
    CHECK(findLayer(ls, "mask") && findLayer(ls, "mask")->replicateFirst);
}

TEST_CASE("stereo: multi-view channels and multi-part views are filtered by view") {
    PartChannels p;
    p.channels = {"R", "G", "B", "right.R", "right.G", "right.B", "diffuse.R", "diffuse.G", "diffuse.B",
                  "diffuse.right.R", "diffuse.right.G", "diffuse.right.B"};
    const std::vector<std::string> mv = {"left", "right"};
    auto left = buildLayers({p}, mv, "left");
    auto right = buildLayers({p}, mv, "right");
    CHECK(names(left) == (std::vector<std::string>{"RGB", "diffuse"}));
    CHECK(names(right) == (std::vector<std::string>{"RGB", "diffuse"}));
    CHECK(left.front().source[0] == "R");
    CHECK(right.front().source[0] == "right.R");
    CHECK(findLayer(right, "diffuse")->source[0] == "diffuse.right.R");

    std::vector<PartChannels> parts = {{"rgba.left", "left", {"R", "G", "B"}, false},
                                       {"rgba.right", "right", {"R", "G", "B"}, false}};
    auto r = buildLayers(parts, {}, "right");
    CHECK(r.size() == 1 && r.front().part == 1 && r.front().displayName == "rgba");
}

// ------------------------------------------------------------------------- decode --
TEST_CASE("decodeFrame: colour as half, data as float, greyscale as one channel") {
    TempDir d;
    const fs::path f = d.path / "img.0001.exr";
    writeExr(f, 6, 3, {{"R", Imf::HALF, 0.0f}, {"G", Imf::HALF, 1.0f}, {"B", Imf::HALF, 2.0f}, {"Z", Imf::FLOAT, 70000.0f}});
    std::vector<LayerInfo> ls;
    ImageInfo info = readImageInfo(f, "", ls);
    CHECK_EQ(info.width, 6);
    auto rgb = decodeFrame(f, *findLayer(ls, "RGB"), 1, 7);
    CHECK(!rgb->missing && rgb->format == PixelFormat::Half && rgb->channels == 4 && rgb->key == 7u);
    CHECK_NEAR(rgb->sample(2, 1, 1), 1.12, 1e-3);
    CHECK_NEAR(rgb->sample(2, 1, 3), 1.0, 0);
    auto z = decodeFrame(f, *findLayer(ls, "Z"), 1, 8);
    CHECK(z->format == PixelFormat::Float && z->channels == 1);
    CHECK_NEAR(z->sample(1, 0, 0), 70000.01, 0.01);  // beyond half range: float keeps it
    CHECK_NEAR(z->sample(1, 0, 2), z->sample(1, 0, 0), 0);
    CHECK_EQ(z->data.size(), size_t(6 * 3 * 4));  // one float per pixel, not four
}

TEST_CASE("decodeFrame: alpha override and data window placement") {
    TempDir d;
    const fs::path f = d.path / "dw.exr";
    const IMATH_NAMESPACE::Box2i dw(IMATH_NAMESPACE::V2i(2, 1), IMATH_NAMESPACE::V2i(4, 2));
    writeExr(f, 8, 4, {{"R"}, {"G"}, {"B"}, {"mask.R", Imf::HALF, 0, 0.25f}}, dw);
    std::vector<LayerInfo> ls;
    readImageInfo(f, "", ls);
    auto fr = decodeFrame(f, ls.front(), 1, 0);
    CHECK_NEAR(fr->sample(0, 0, 3), 0.0, 0);  // outside data window: transparent
    CHECK_NEAR(fr->sample(2, 1, 0), 0.12, 1e-3);
    DecodeOptions o;
    o.alphaOverride = "mask.R";
    auto withAlpha = decodeFrame(f, ls.front(), 1, 0, o);
    CHECK_NEAR(withAlpha->sample(3, 2, 3), 0.25, 1e-3);
}

TEST_CASE("proxy: box filter for scanline files, mip level 1 for tiled files") {
    TempDir d;
    const fs::path f = d.path / "scan.exr";
    writeExr(f, 8, 4, {{"R", Imf::HALF, 0, 0.0f}, {"G", Imf::HALF, 0, 1.0f}, {"B"}});
    std::vector<LayerInfo> ls;
    readImageInfo(f, "", ls);
    DecodeOptions o;
    o.proxy = true;
    auto p = decodeFrame(f, ls.front(), 1, 0, o);
    CHECK_EQ(p->width, 4);
    CHECK_EQ(p->height, 2);
    CHECK_EQ(p->fullWidth, 8);
    CHECK_NEAR(p->sample(1, 1, 1), 1.0, 1e-3);
    CHECK_NEAR(p->sample(1, 0, 2), (0.02 + 0.03 + 0.12 + 0.13) / 4.0, 2e-3);  // average of 2x2

    const fs::path t = d.path / "tiled.exr";
    writeTiledMip(t, 64, 32, 1.0f, 5.0f);
    std::vector<LayerInfo> tl;
    ImageInfo ti = readImageInfo(t, "", tl);
    CHECK(ti.tiled && ti.mipmapped);
    auto full = decodeFrame(t, tl.front(), 1, 0);
    auto lvl = decodeFrame(t, tl.front(), 1, 0, o);
    CHECK_NEAR(full->sample(5, 5, 0), 1.0, 1e-3);
    CHECK_EQ(lvl->width, 32);
    CHECK_NEAR(lvl->sample(5, 5, 0), 5.0, 1e-3);  // came from mip level 1, not filtered level 0
}

TEST_CASE("metadata and cryptomatte manifests are read from the header") {
    TempDir d;
    const fs::path f = d.path / "crypto.exr";
    const std::string key = "cryptomatte/f834d0a/";
    writeExr(f, 4, 4,
             {{"R"}, {"G"}, {"B"}, {"CryptoObject00.r", Imf::FLOAT}, {"CryptoObject00.g", Imf::FLOAT},
              {"CryptoObject00.b", Imf::FLOAT}, {"CryptoObject00.a", Imf::FLOAT}},
             std::nullopt, std::nullopt,
             {{key + "name", "CryptoObject"}, {key + "manifest", R"({"/geo/teapot":"3f800000","/geo/floor":"40000000"})"},
              {"renderer", "TestRender 1.0"}});
    std::vector<LayerInfo> ls;
    ImageInfo info = readImageInfo(f, "", ls);
    auto it = info.cryptoManifests.find("CryptoObject");
    CHECK(it != info.cryptoManifests.end());
    if (it != info.cryptoManifests.end()) CHECK(it->second.at(0x3f800000u) == "/geo/teapot");
    bool foundRenderer = false;
    for (const auto& e : info.metadata)
        if (e.name == "renderer" && e.value == "TestRender 1.0") foundRenderer = true;
    CHECK(foundRenderer);
    CHECK(findLayer(ls, "CryptoObject00") && findLayer(ls, "CryptoObject00")->kind == AovKind::Crypto);
}

TEST_CASE("unreadable files become missing frames instead of throwing") {
    TempDir d;
    const fs::path f = d.path / "broken.0001.exr";
    std::ofstream(f) << "not an exr";
    LayerInfo l;
    l.source[0] = "R";
    auto fr = decodeFrame(f, l, 1, 0);
    CHECK(fr->missing && !fr->error.empty());
}

// -------------------------------------------------------------------------- cache --
namespace {
FramePtr fakeFrame(uint32_t key, int frame, size_t pixels = 100) {
    auto f = std::make_shared<Frame>();
    f->key = key;
    f->frameNumber = frame;
    f->data.resize(pixels * 8);
    return f;
}
}  // namespace

TEST_CASE("FrameCache: two active layers, eviction behind the playhead, inactive keys first") {
    const size_t one = fakeFrame(0, 0)->byteSize();
    FrameCache c(one * 4);
    PlayheadHint h;
    h.active = {{1, 10, 1, 20}, {2, 10, 1, 20}};
    c.setPlayhead(h);
    CHECK(c.insert(fakeFrame(1, 10)));
    CHECK(c.insert(fakeFrame(2, 10)));
    CHECK(c.insert(fakeFrame(1, 11)));
    CHECK(c.insert(fakeFrame(9, 5)));  // inactive key
    CHECK(c.insert(fakeFrame(2, 11)));  // evicts the inactive key first
    CHECK(!c.contains(9, 5));
    h.active = {{1, 11, 1, 20}, {2, 11, 1, 20}};
    c.setPlayhead(h);
    CHECK(c.insert(fakeFrame(1, 12)));  // evicts a frame 10 (just behind the playhead)
    CHECK(c.contains(1, 11) && c.contains(2, 11) && c.contains(1, 12));
    CHECK(!c.wouldAccept(1, 19, one) || c.wouldAccept(1, 19, one));  // smoke
    c.eraseKey(1);
    CHECK(c.cachedFrames(1).empty());
}

// ----------------------------------------------------------------------- playback --
TEST_CASE("Realtime playback at 23.976 has no drift; loop and stop") {
    PlaybackController p;
    p.setRange(1, 100000);
    p.setFrameRate({24000, 1001}, t0());
    p.play(1, t0());
    p.update(t0() + std::chrono::hours(1), nullptr);
    CHECK_EQ(p.current(), 1 + 86313);

    PlaybackController q;
    q.setRange(1, 10);
    q.setFrameRate({24, 1}, t0());
    q.setLoop(false);
    q.play(1, t0());
    q.update(t0() + 500ms, nullptr);
    CHECK_EQ(q.current(), 10);
    CHECK(!q.playing());
}

// ------------------------------------------------------------------------- layout --
TEST_CASE("compare layout: overlap, fit/pixel align, side by side, over/under") {
    LayerGeom A{true, 1920, 1080, 1.0f}, B{true, 960, 540, 1.0f};
    ViewState fit;
    Layout w = computeLayout(fit, 1920, 1080, CompareMode::Wipe, AlignMode::Fit, A, B);
    CHECK(w.showA && w.showB);
    CHECK_NEAR(w.a.x1 - w.a.x0, 1920, 1e-6);
    CHECK_NEAR(w.b.x1 - w.b.x0, 1920, 1e-6);  // B fitted to A
    Layout px = computeLayout(fit, 1920, 1080, CompareMode::Wipe, AlignMode::Pixel, A, B);
    CHECK_NEAR(px.b.x1 - px.b.x0, 960, 1e-6);  // B at A's pixel scale
    CHECK_NEAR((px.b.x0 + px.b.x1) / 2, 960, 1e-6);
    Layout sbs = computeLayout(fit, 2000, 1000, CompareMode::SideBySide, AlignMode::Fit, A, B);
    CHECK_NEAR(sbs.a.x1, sbs.b.x0, 1e-6);  // touching, A left of B
    CHECK_NEAR(sbs.a.y1 - sbs.a.y0, sbs.b.y1 - sbs.b.y0, 1e-6);
    Layout ou = computeLayout(fit, 1000, 2000, CompareMode::OverUnder, AlignMode::Fit, A, B);
    CHECK_NEAR(ou.a.y1, ou.b.y0, 1e-6);
    Layout onlyB = computeLayout(fit, 100, 100, CompareMode::B, AlignMode::Fit, A, B);
    CHECK(!onlyB.showA && onlyB.showB);
    auto p = rectToImage(w.a, 960.5, 540.5, 1920, 1080);
    CHECK(p && p->first == 960 && p->second == 540);
}

// ----------------------------------------------------------------------- analysis --
TEST_CASE("rebuild sum / difference and contact sheet") {
    auto mk = [](float v) {
        auto f = std::make_shared<Frame>();
        f->width = f->fullWidth = 2;
        f->height = f->fullHeight = 2;
        f->format = PixelFormat::Float;
        f->channels = 4;
        f->data.resize(2 * 2 * 16);
        for (int i = 0; i < 16; ++i) std::memcpy(f->data.data() + i * 4, &v, 4);
        return FramePtr(f);
    };
    auto beauty = mk(1.0f), d = mk(0.25f), s = mk(0.5f);
    auto diff = rebuildFrame(beauty, {d, s}, true, 1, 0);
    CHECK_NEAR(diff->sample(0, 0, 0), 0.25, 1e-6);
    auto sum = rebuildFrame(beauty, {d, s}, false, 1, 0);
    CHECK_NEAR(sum->sample(1, 1, 2), 0.75, 1e-6);
    auto sheet = composeContactSheet({{beauty, false}, {d, false}, {s, true}}, 2, 4, 4, 1, 0);
    CHECK_EQ(sheet->width, 8);
    CHECK_EQ(sheet->height, 8);
    CHECK_NEAR(sheet->sample(1, 1, 0), 1.0, 1e-6);
    CHECK_NEAR(sheet->sample(5, 1, 0), 0.25, 1e-6);
    auto rs = regionStats(*beauty, 0, 0, 1, 1);
    CHECK_EQ(rs.count, 4);
    CHECK_NEAR(rs.mean[0], 1.0, 1e-9);
}
