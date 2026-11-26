// Session tests: the JSON API the C# UI uses, without any rendering.
#include "TestExr.h"
#include "TestHarness.h"

#include "exrc/Session.h"

#include <nlohmann/json.hpp>

using namespace exrc;
using namespace testexr;
using json = nlohmann::json;

namespace {
json call(Session& s, const json& j) {
    json r = json::parse(s.call(j.dump()));
    if (!r.value("ok", false)) std::cerr << "  call failed: " << j.dump() << " -> " << r.dump() << "\n";
    return r;
}
std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
SessionConfig testConfig() {
    SessionConfig c;
    c.decodeThreads = 2;
    c.exrThreads = 0;
    c.cacheBudgetBytes = size_t(256) << 20;
    return c;
}
}  // namespace

TEST_CASE("Session: open A and B, AOV lock, compare modes, render state") {
    TempDir d;
    makeShot(d.path / "v001", "shot_v001.", 1001, 1010);
    makeShot(d.path / "v002", "shot_v002.", 1001, 1010, 16, 8, 0.1f);
    Session s(testConfig());

    json a = call(s, {{"cmd", "open"}, {"slot", 0}, {"path", u8(d.path / "v001" / "shot_v001.1005.exr")}});
    CHECK(a["ok"].get<bool>());
    CHECK_EQ(a["first"].get<int>(), 1001);
    CHECK_EQ(a["aovs"][0]["name"].get<std::string>(), std::string("RGBA"));
    CHECK_EQ(a["version"].get<std::string>(), std::string("v001"));
    json st = call(s, {{"cmd", "status"}});
    CHECK_EQ(st["fpsLabel"].get<std::string>(), std::string("23.976"));
    CHECK_EQ(st["compare"].get<std::string>(), std::string("a"));

    call(s, {{"cmd", "aov"}, {"slot", 0}, {"name", "diffuse"}});
    json b = call(s, {{"cmd", "open"}, {"slot", 1}, {"path", u8(d.path / "v002" / "shot_v002.1001.exr")}});
    CHECK(b["ok"].get<bool>());
    CHECK_EQ(b["aovs"][b["aov"].get<int>()]["name"].get<std::string>(), std::string("diffuse"));  // locked AOV
    st = call(s, {{"cmd", "status"}});
    CHECK_EQ(st["compare"].get<std::string>(), std::string("wipe"));  // second layer -> wipe

    call(s, {{"cmd", "aov"}, {"slot", 1}, {"name", "Z"}});
    json la = call(s, {{"cmd", "layer"}, {"slot", 0}});
    CHECK_EQ(la["aovs"][la["aov"].get<int>()]["name"].get<std::string>(), std::string("Z"));
    CHECK_EQ(la["effectiveMode"].get<std::string>(), std::string("depth"));

    s.waitForDecodes();
    RenderState rs = s.update(Clock::now(), 1600, 900);
    CHECK(rs.hasContent && rs.a.present && rs.b.present);
    CHECK(rs.a.frame && rs.b.frame);
    CHECK(rs.a.mode == LayerMode::Depth);
    CHECK_NEAR(rs.a.depthNear, 100.0, 0.01);  // auto range from the frame
    CHECK(rs.program && !rs.program->display.code.empty());
    CHECK(rs.compare == CompareMode::Wipe);
    CHECK_NEAR(rs.wipeX, 800, 1e-3);

    for (const char* m : {"stack", "difference", "sidebyside", "overunder", "b", "a"}) {
        json r = call(s, {{"cmd", "compare"}, {"mode", m}});
        CHECK(r["ok"].get<bool>());
    }
    CHECK(!call(s, {{"cmd", "compare"}, {"mode", "nope"}})["ok"].get<bool>());
}

TEST_CASE("Session: transport, offsets, probe, stats, metadata") {
    TempDir d;
    makeShot(d.path, "beauty.", 1, 20);
    Session s(testConfig());
    call(s, {{"cmd", "open"}, {"slot", 0}, {"path", u8(d.path)}});
    call(s, {{"cmd", "step"}, {"delta", 2}});
    json st = call(s, {{"cmd", "status"}});
    CHECK_EQ(st["frame"].get<int>(), 3);
    call(s, {{"cmd", "layer_set"}, {"slot", 0}, {"offset", 10}});
    st = call(s, {{"cmd", "status"}});
    CHECK_EQ(st["first"].get<int>(), 11);
    CHECK_EQ(st["last"].get<int>(), 30);
    call(s, {{"cmd", "seek"}, {"frame", 15}});
    st = call(s, {{"cmd", "status"}});
    CHECK_EQ(st["layers"][0]["frame"].get<int>(), 5);

    s.waitForDecodes();
    s.update(Clock::now(), 160, 80);  // viewport = image size, fit
    json p = call(s, {{"cmd", "probe"}, {"x", 8.5}, {"y", 4.5}});
    CHECK(p.contains("a"));
    if (p.contains("a")) {
        CHECK_EQ(p["a"]["x"].get<int>(), 0);  // 16x8 image in a 160x80 viewport: 10x zoom
        CHECK_NEAR(p["a"]["rgba"][0].get<double>(), 0.55, 2e-3);
    }
    json reg = call(s, {{"cmd", "region"}, {"x0", 0}, {"y0", 0}, {"x1", 159}, {"y1", 79}});
    CHECK(reg.contains("a") && reg["a"]["count"].get<int>() == 16 * 8);
    json stats = call(s, {{"cmd", "stats"}});
    CHECK(stats.contains("a"));
    json md = call(s, {{"cmd", "metadata"}, {"slot", 0}});
    CHECK(md["metadata"].size() > 3);
    json geo = call(s, {{"cmd", "geometry"}});
    CHECK(geo["a"].is_array() && geo["dataWindowA"].is_array());

    // Pixel Analyzer: pinned single pixel, then a box; stays until cleared.
    json px = call(s, {{"cmd", "sample_pixel"}, {"x", 15.0}, {"y", 15.0}});  // pixel (1,1)
    CHECK(px["regionActive"].get<bool>());
    CHECK_EQ(px["a"]["region"]["w"].get<int>(), 1);
    CHECK_EQ(px["a"]["region"]["x"].get<int>(), 1);
    json box = call(s, {{"cmd", "sample_region"}, {"x0", 0.0}, {"y0", 0.0}, {"x1", 80.0}, {"y1", 40.0}});
    CHECK_EQ(box["a"]["region"]["w"].get<int>(), 8);
    CHECK_EQ(box["a"]["region"]["h"].get<int>(), 4);
    CHECK_EQ(box["a"]["region"]["count"].get<int>(), 32);
    CHECK_NEAR(box["a"]["region"]["median"][1].get<double>(), 0.5, 1e-3);
    CHECK(box["a"]["region"]["display"].size() == 3);
    json an = call(s, {{"cmd", "analyzer"}, {"x", 8.5}, {"y", 4.5}});
    CHECK(an["a"]["pixel"].is_object() && an["a"]["region"].is_object());
    CHECK(call(s, {{"cmd", "geometry"}})["region"].is_array());
    call(s, {{"cmd", "clear_region"}});
    CHECK(!call(s, {{"cmd", "analyzer"}})["a"].contains("region"));
    CHECK(call(s, {{"cmd", "wipe"}, {"x", 0.3}})["ok"].get<bool>());
    json col = call(s, {{"cmd", "color"}, {"saturation", 0.0}});
    CHECK_NEAR(col["color"]["saturation"].get<double>(), 0.0, 1e-6);
    json grey = call(s, {{"cmd", "analyzer"}, {"x", 8.5}, {"y", 4.5}});
    json sw = grey["a"]["pixel"]["display"];
    CHECK_NEAR(sw[0].get<double>(), sw[1].get<double>(), 1e-4);  // saturation 0 -> neutral swatch
    call(s, {{"cmd", "color"}, {"saturation", 1.0}});
}

TEST_CASE("Session: contact sheet, rebuild check, proxy, cache modes, versions, swap, OCIO") {
    TempDir d;
    makeShot(d.path / "v001", "shot_v001.", 1, 4);
    makeShot(d.path / "v002", "shot_v002.", 1, 4);
    Session s(testConfig());
    call(s, {{"cmd", "open"}, {"slot", 0}, {"path", u8(d.path / "v001" / "shot_v001.0001.exr")}});

    call(s, {{"cmd", "layer_set"}, {"slot", 0}, {"contactSheet", true}});
    s.waitForDecodes();
    RenderState rs = s.update(Clock::now(), 400, 400);
    CHECK(rs.a.frame && !rs.a.frame->missing);
    json geo = call(s, {{"cmd", "geometry"}});
    CHECK(geo["tilesA"].is_array() && geo["tilesA"].size() == 5);  // RGBA, diffuse, specular, Z, N
    call(s, {{"cmd", "layer_set"}, {"slot", 0}, {"contactSheet", false}});

    call(s, {{"cmd", "layer_set"}, {"slot", 0}, {"rebuild", 1}, {"rebuildBeauty", "RGBA"},
             {"rebuildAovs", json::array({"diffuse", "specular"})}});
    s.waitForDecodes();
    rs = s.update(Clock::now(), 400, 400);
    CHECK(rs.a.frame && !rs.a.frame->missing);
    if (rs.a.frame) CHECK_NEAR(rs.a.frame->sample(3, 3, 1), 0.0, 1e-3);  // diffuse + specular == beauty
    call(s, {{"cmd", "layer_set"}, {"slot", 0}, {"rebuild", 0}});

    call(s, {{"cmd", "proxy"}, {"on", true}});
    s.waitForDecodes();
    rs = s.update(Clock::now(), 400, 400);
    CHECK(rs.a.frame && rs.a.frame->width == 8 && rs.a.frame->fullWidth == 16);
    call(s, {{"cmd", "proxy"}, {"on", false}});

    call(s, {{"cmd", "cache"}, {"mode", "region"}});
    s.waitForDecodes();
    json cached = call(s, {{"cmd", "cached"}, {"slot", 0}});
    CHECK_EQ(cached["frames"].size(), size_t(4));
    call(s, {{"cmd", "cache"}, {"mode", "off"}});
    cached = call(s, {{"cmd", "cached"}, {"slot", 0}});
    CHECK(cached["frames"].empty());
    call(s, {{"cmd", "cache"}, {"mode", "lookahead"}});

    json v = call(s, {{"cmd", "version"}, {"slot", 0}, {"delta", 1}});
    CHECK(v["ok"].get<bool>() && v["version"].get<std::string>() == "v002");
    CHECK(!call(s, {{"cmd", "version"}, {"slot", 0}, {"delta", 1}})["ok"].get<bool>());

    call(s, {{"cmd", "open"}, {"slot", 1}, {"path", u8(d.path / "v001" / "shot_v001.0001.exr")}});
    call(s, {{"cmd", "swap"}});
    json la = call(s, {{"cmd", "layer"}, {"slot", 0}});
    CHECK_EQ(la["version"].get<std::string>(), std::string("v001"));

    json o = call(s, {{"cmd", "ocio"}});
    CHECK(o["displays"].size() >= 1);
    json c = call(s, {{"cmd", "color"}, {"exposure", 1.5}, {"gamma", 1.2}, {"nanHighlight", true}});
    CHECK_NEAR(c["color"]["exposure"].get<double>(), 1.5, 1e-6);
    CHECK(!call(s, {{"cmd", "ocio_load"}, {"path", "/no/such/config.ocio"}})["ok"].get<bool>());
    CHECK(!call(s, {{"cmd", "bogus"}})["ok"].get<bool>());
    CHECK(!json::parse(s.call("not json")).value("ok", true));
}
