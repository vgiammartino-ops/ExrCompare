// OpenColorIO pipeline tests. Uses an inline config so they don't depend on built-in configs
// (which need OCIO >= 2.2); the built-in config test runs when available.
#include "TestHarness.h"

#include "exrc/ColorPipeline.h"

#include <cmath>
#include <fstream>

using namespace exrc;

namespace {
// A small config: linear sRGB scene_linear, an sRGB display with a 1D LUT-free "Standard" view,
// a "Film" view with a 3D LUT (forces texture generation) and a look.
const char* kConfig = R"(ocio_profile_version: 2
roles:
  scene_linear: lin_srgb
  default: lin_srgb
file_rules:
  - !<Rule> {name: Default, colorspace: default}
displays:
  sRGB:
    - !<View> {name: Standard, colorspace: srgb_display}
    - !<View> {name: Raw, colorspace: raw}
looks:
  - !<Look> {name: warm, process_space: lin_srgb, transform: !<CDLTransform> {slope: [1.2, 1.0, 0.8]}}
colorspaces:
  - !<ColorSpace>
    name: lin_srgb
    aliases: [Linear Rec.709 (sRGB)]
    isdata: false
  - !<ColorSpace>
    name: acescg
    from_scene_reference: !<MatrixTransform> {matrix: [0.6131, 0.3395, 0.0474, 0, 0.0702, 0.9164, 0.0134, 0, 0.0206, 0.1096, 0.8698, 0, 0, 0, 0, 1]}
  - !<ColorSpace>
    name: srgb_display
    from_scene_reference: !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055, direction: inverse}
  - !<ColorSpace>
    name: raw
    isdata: true
)";
}  // namespace

TEST_CASE("OCIO: inline config lists displays, views, looks and colour spaces") {
    ColorPipeline p;
    std::string err;
    CHECK(p.loadFromString(kConfig, &err));
    if (!err.empty()) std::cerr << err << "\n";
    CHECK(p.displays() == std::vector<std::string>{"sRGB"});
    CHECK(p.views("sRGB") == (std::vector<std::string>{"Standard", "Raw"}));
    CHECK(p.looks() == std::vector<std::string>{"warm"});
    CHECK_EQ(p.sceneLinear(), std::string("lin_srgb"));
    CHECK_EQ(p.display(), std::string("sRGB"));
    CHECK_EQ(p.view(), std::string("Standard"));
    CHECK_EQ(p.guessInputSpace("x.exr"), std::string("lin_srgb"));  // via the alias
}

TEST_CASE("OCIO: HLSL generated for input and display; CPU path matches the sRGB curve") {
    ColorPipeline p;
    p.loadFromString(kConfig, nullptr);
    const uint64_t gen = p.generation();
    ShaderFunction in = p.inputShader("acescg", "OCIO_InputA", "ocioa_");
    CHECK(in.code.find("OCIO_InputA") != std::string::npos);
    ShaderFunction disp = p.displayShader("OCIO_Display", "ociod_");
    CHECK(disp.code.find("OCIO_Display") != std::string::npos);
    CHECK_EQ(disp.uniformCount, 0);
    std::string err;
    CHECK(p.setDisplayView("sRGB", "Standard", "warm", &err));
    CHECK(p.generation() > gen);
    CHECK(!p.setDisplayView("nope", "Standard", "", &err));
    p.setDisplayView("sRGB", "Standard", "", nullptr);

    float px[4] = {0.18f, 0.18f, 0.18f, 1.0f};
    p.linearToDisplayCpu(px, 1, 1);
    CHECK_NEAR(px[0], 0.4614, 2e-3);
    float px2[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    p.toDisplayCpu("acescg", px2, 1, 1);  // white stays white through the matrix
    CHECK_NEAR(px2[1], 1.0, 2e-3);

    // Dump generated shaders so CI / developers can compile-check them (glslang, fxc).
    std::ofstream("ocio_input.hlsl") << in.code;
    std::ofstream("ocio_display.hlsl") << disp.code;
}

TEST_CASE("OCIO: built-in ACES config loads when this OCIO has it") {
    const auto builtins = ColorPipeline::builtinConfigs();
    if (builtins.size() <= 1) {  // only this app's legacy config: OCIO's registry is absent
        std::cout << "  (OCIO " << ColorPipeline().ocioVersion() << " has no built-in configs; skipped)\n";
        return;
    }
    ColorPipeline p;
    std::string err;
    CHECK(p.load("", &err));
    CHECK(!p.displays().empty());
    ShaderFunction d = p.displayShader("OCIO_Display", "ociod_");
    CHECK(!d.code.empty());
    std::cout << "  default config: " << p.configName() << ", display " << p.display() << " / " << p.view() << ", "
              << d.luts.size() << " LUT textures\n";
}

TEST_CASE("OCIO: legacy linear sRGB / Rec.709 workflow (no tone mapping)") {
    ColorPipeline ocio;
    std::string err;
    CHECK(ocio.load(kLegacyConfigUri, &err));
    CHECK_EQ(ocio.sceneLinear(), std::string("Linear Rec.709 (sRGB)"));
    CHECK_EQ(ocio.display(), std::string("sRGB Display"));
    CHECK_EQ(ocio.view(), std::string("sRGB"));
    bool listed = false;
    for (const auto& b : ColorPipeline::builtinConfigs()) listed |= b.name == kLegacyConfigUri;
    CHECK(listed);
    auto run = [&](const char* display, const char* view, float v) {
        CHECK(ocio.setDisplayView(display, view, "", &err));
        float px[4] = {v, v, v, 1.0f};
        ocio.linearToDisplayCpu(px, 1, 1);
        return px[0];
    };
    CHECK_NEAR(run("sRGB Display", "sRGB", 0.18f), 0.4613, 1e-3);
    CHECK_NEAR(run("sRGB Display", "sRGB", 1.0f), 1.0, 1e-4);
    CHECK_NEAR(run("sRGB Display", "sRGB", 0.0f), 0.0, 1e-6);
    CHECK_NEAR(run("sRGB Display", "Gamma 2.2", 0.18f), 0.4587, 1e-3);
    CHECK_NEAR(run("Rec.709 Display", "Rec.709", 0.18f), 0.4090, 1e-3);
    CHECK_NEAR(run("Rec.709 Display", "BT.1886 (gamma 2.4)", 0.18f), 0.4894, 1e-3);
    CHECK(run("sRGB Display", "sRGB", 4.0f) >= 1.0f);  // no tone curve: clips on screen
    CHECK(!ocio.displayShader("OCIO_Display", "ociod_").code.empty());
}
