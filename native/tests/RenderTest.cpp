// Windows-only: drives exrcore.dll headless on a WARP device. Checks that the compositor shader
// (with the real OCIO-generated code and LUTs) compiles and every compare mode renders.
#include "TestExr.h"
#include "exrcore.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using json = nlohmann::json;
using namespace testexr;

static int failures = 0;
#define EXPECT(c)                                                                 \
    do {                                                                          \
        if (!(c)) {                                                               \
            ++failures;                                                           \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << " " #c "\n"; \
        }                                                                         \
    } while (0)

static json call(ExrcHandle* h, const json& j) {
    std::string buf(1 << 20, '\0');
    int n = exrc_call(h, j.dump().c_str(), buf.data(), int(buf.size()));
    if (n >= int(buf.size())) {
        buf.assign(size_t(n) + 1, '\0');
        n = exrc_call(h, j.dump().c_str(), buf.data(), int(buf.size()));
    }
    json r = json::parse(buf.c_str());
    if (!r.value("ok", false)) std::cerr << "call failed: " << j.dump() << " -> " << r.dump() << "\n";
    return r;
}

static std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

int main(int argc, char** argv) {
    // --samples <dir>: write two small test shots for the app smoke test, then exit.
    if (argc >= 3 && std::string(argv[1]) == "--samples") {
        const fs::path dir = argv[2];
        makeShot(dir / "a", "a.", 1, 10, 256, 128);
        makeShot(dir / "b", "b.", 1, 10, 256, 128, 0.3f);
        std::cout << "samples written to " << dir.string() << "\n";
        return 0;
    }
    _putenv_s("EXRC_FORCE_WARP", "1");
    TempDir d;
    makeShot(d.path / "a", "a.", 1, 3, 64, 32);
    makeShot(d.path / "b", "b.", 1, 3, 64, 32, 0.3f);

    ExrcHandle* h = exrc_create(R"({"cacheBudget": 268435456})");
    EXPECT(h != nullptr);
    if (!h) return 1;
    json o = call(h, {{"cmd", "ocio"}});
    std::cout << "OCIO " << o.value("version", "?") << ", config: " << o.value("name", "?") << " (" << o.value("source", "?")
              << "), display " << o.value("display", "?") << " / " << o.value("view", "?") << "\n";

    EXPECT(call(h, {{"cmd", "open"}, {"slot", 0}, {"path", u8(d.path / "a" / "a.0001.exr")}})["ok"].get<bool>());
    EXPECT(call(h, {{"cmd", "open"}, {"slot", 1}, {"path", u8(d.path / "b" / "b.0001.exr")}})["ok"].get<bool>());

    for (const char* mode : {"a", "b", "wipe", "stack", "difference", "sidebyside", "overunder"}) {
        call(h, {{"cmd", "compare"}, {"mode", mode}});
        const fs::path out = d.path / (std::string("snap_") + mode + ".png");
        json stroke = {{"points", json::array({json::array({1, 1}), json::array({30, 20})})},
                       {"color", json::array({1, 0, 0})},
                       {"width", 2}};
        json req = {{"cmd", "snapshot"},
                    {"path", u8(out)},
                    {"burnin", json::array({std::string("mode ") + mode, "frame 1"})},
                    {"annotations", json::array({stroke})}};
        json r = call(h, req);
        EXPECT(r.value("ok", false));
        EXPECT(r.value("shaderError", "x").empty());
        EXPECT(fs::exists(out) && fs::file_size(out) > 100);
        if (r.contains("centerBGRA")) std::cout << mode << " centre BGRA " << r["centerBGRA"].dump() << "\n";
    }
    // Data AOV modes and analysis tools also go through the shader.
    call(h, {{"cmd", "compare"}, {"mode", "a"}});
    for (const char* aov : {"Z", "N"}) {
        call(h, {{"cmd", "aov"}, {"slot", 0}, {"name", aov}});
        json r = call(h, {{"cmd", "snapshot"}, {"path", u8(d.path / (std::string("aov_") + aov + ".jpg"))}});
        EXPECT(r.value("ok", false) && r.value("shaderError", "x").empty());
    }
    call(h, {{"cmd", "color"}, {"nanHighlight", true}, {"falseColor", true}, {"matte", true}, {"exposure", 1.0}, {"gamma", 2.2}});
    EXPECT(call(h, {{"cmd", "snapshot"}, {"path", u8(d.path / "flags.png")}}).value("ok", false));

    // Every display/view of the default config must compile.
    json oc = call(h, {{"cmd", "ocio"}});
    int views = 0;
    for (const auto& disp : oc["displays"]) {
        json c = call(h, {{"cmd", "color"}, {"display", disp}});
        for (const auto& v : c["views"]) {
            call(h, {{"cmd", "color"}, {"display", disp}, {"view", v}});
            json r = call(h, {{"cmd", "snapshot"}, {"path", u8(d.path / "view.png")}, {"width", 64}, {"height", 32}});
            if (!r.value("shaderError", "").empty())
                std::cerr << "view " << disp << " / " << v << ": " << r["shaderError"].get<std::string>() << "\n";
            EXPECT(r.value("ok", false) && r.value("shaderError", "x").empty());
            ++views;
        }
    }
    std::cout << views << " display/view combinations compiled and rendered\n";

    std::vector<unsigned char> wf(128 * 64 * 4);
    EXPECT(exrc_waveform(h, 0, 128, 64, wf.data()) == 0);
    exrc_destroy(h);
    std::cout << (failures ? "FAILED" : "OK") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
