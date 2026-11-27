// Windows-only: drives exrcore.dll headless on a WARP device. Checks that the compositor shader
// (with the real OCIO-generated code and LUTs) compiles and every compare mode renders.
#include "TestExr.h"
#include "exrcore.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
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

    // Waveform: the GPU counts every texel of A (raw scene-linear values); they must match the
    // engine's CPU reference. Columns and totals exactly; rows may differ by one bin at a boundary
    // (GPU log2 vs CPU log2), measured as the largest per-column CDF gap.
    call(h, {{"cmd", "color"}, {"nanHighlight", false}, {"falseColor", false}, {"matte", false}, {"exposure", 0.0}, {"gamma", 1.0}});
    call(h, {{"cmd", "aov"}, {"slot", 0}, {"index", 0}});
    call(h, {{"cmd", "layer_set"}, {"slot", 0}, {"mode", "raw"}});
    for (const char* scale : {"stops8", "linear1"}) {
        call(h, {{"cmd", "waveform"}, {"enabled", true}, {"source", "a"}, {"scale", scale}, {"units", "scene"}});
        const int cols = 64, rows = 256;
        json g = call(h, {{"cmd", "waveform_bins"}, {"cols", cols}, {"rows", rows}});
        json c = call(h, {{"cmd", "waveform_reference"}, {"slot", 0}, {"cols", cols}, {"rows", rows}});
        if (!g.value("error", "").empty()) std::cerr << "waveform: " << g["error"].get<std::string>() << "\n";
        EXPECT(g.value("ok", false) && g["bins"].size() == c["bins"].size());
        if (g["bins"].size() != c["bins"].size()) break;
        const auto gb = g["bins"].get<std::vector<uint32_t>>(), cb = c["bins"].get<std::vector<uint32_t>>();
        const size_t tail = size_t(4) * (rows + 2) * cols;
        uint64_t total = 0, maxGap = 0;
        bool columnsMatch = true;
        for (int p = 0; p < 4; ++p) {
            for (int col = 0; col < cols; ++col) {
                int64_t cg = 0, cc = 0;
                for (int r = 0; r < rows + 2; ++r) {
                    const size_t i = (size_t(p) * (rows + 2) + r) * cols + col;
                    cg += gb[i];
                    cc += cb[i];
                    maxGap = std::max<uint64_t>(maxGap, uint64_t(std::llabs(cg - cc)));
                }
                columnsMatch &= cg == cc;
                total += uint64_t(cg);
            }
        }
        for (int k = 0; k < 4; ++k) EXPECT(gb[tail + k] == cb[tail + k]);  // NaN / Inf counts
        float pg, pc;
        std::memcpy(&pg, &gb[tail + 4], 4);
        std::memcpy(&pc, &cb[tail + 4], 4);
        std::cout << "waveform " << scale << ": " << total << " samples, columns match " << columnsMatch
                  << ", max CDF gap " << maxGap << ", peak GPU " << pg << " CPU " << pc << "\n";
        EXPECT(columnsMatch && total > 0);
        EXPECT(maxGap <= 2);
        EXPECT(std::abs(pg - pc) <= 1e-6f * std::max(1.0f, pc));
    }
    // Display-light (nits) units and the viewer source go through OCIO in the compute shader.
    call(h, {{"cmd", "layer_set"}, {"slot", 0}, {"mode", "auto"}});
    call(h, {{"cmd", "compare"}, {"mode", "wipe"}});
    call(h, {{"cmd", "waveform"}, {"source", "viewer"}, {"units", "nits"}, {"scale", "stops8"}});
    json gn = call(h, {{"cmd", "waveform_bins"}, {"cols", 32}, {"rows", 128}});
    if (!gn.value("error", "").empty()) std::cerr << "waveform nits: " << gn["error"].get<std::string>() << "\n";
    EXPECT(gn.value("ok", false) && gn.value("error", "x").empty() && gn["bins"].size() > 0);
    call(h, {{"cmd", "waveform"}, {"enabled", false}, {"units", "scene"}});

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

    exrc_destroy(h);
    std::cout << (failures ? "FAILED" : "OK") << " (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
