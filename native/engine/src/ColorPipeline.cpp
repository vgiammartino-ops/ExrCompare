#include "exrc/ColorPipeline.h"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace OCIO = OCIO_NAMESPACE;

namespace exrc {

struct ColorPipeline::Impl {
    OCIO::ConstConfigRcPtr config;
};

ColorPipeline::ColorPipeline() : impl_(std::make_unique<Impl>()) {
    impl_->config = OCIO::Config::CreateRaw();
    source_ = "raw";
    pickDefaults();
}

ColorPipeline::~ColorPipeline() = default;

std::string ColorPipeline::ocioVersion() const { return OCIO::GetVersion(); }

namespace {

// Legacy linear workflow: reference = linear Rec.709 / sRGB primaries. Each encoded space decodes
// to linear with an exact curve (no LUTs), so views are 1.0 -> white, above 1.0 clips, primaries pure.
OCIO::ConstConfigRcPtr makeLegacyConfig() {
    auto cfg = OCIO::Config::Create();
    cfg->setName("exrcompare-legacy-srgb");
    cfg->setDescription("Linear sRGB / Rec.709 (legacy): no tone mapping, no gamut mapping.");
    auto add = [&](const char* name, const char* family, const char* encoding, OCIO::ConstTransformRcPtr toRef, bool data) {
        auto cs = OCIO::ColorSpace::Create(OCIO::REFERENCE_SPACE_SCENE);
        cs->setName(name);
        cs->setFamily(family);
        cs->setEncoding(encoding);
        cs->setIsData(data);
        if (toRef) cs->setTransform(toRef, OCIO::COLORSPACE_DIR_TO_REFERENCE);
        cfg->addColorSpace(cs);
    };
    // Forward ExponentWithLinear / Exponent = decode (encoded -> linear).
    auto curve = [](double gamma, double offset) {
        auto t = OCIO::ExponentWithLinearTransform::Create();
        const double g[4] = {gamma, gamma, gamma, 1.0}, o[4] = {offset, offset, offset, 0.0};
        t->setGamma(g);
        t->setOffset(o);
        return OCIO::ConstTransformRcPtr(t);
    };
    auto power = [](double gamma) {
        auto t = OCIO::ExponentTransform::Create();
        const double g[4] = {gamma, gamma, gamma, 1.0};
        t->setValue(g);
        return OCIO::ConstTransformRcPtr(t);
    };
    add("Linear Rec.709 (sRGB)", "Scene-linear", "scene-linear", nullptr, false);
    add("sRGB", "Display", "sdr-video", curve(2.4, 0.055), false);
    add("Rec.709", "Display", "sdr-video", curve(1.0 / 0.45, 0.099), false);  // camera OETF (Nuke "rec709")
    add("Gamma 2.4 (BT.1886)", "Display", "sdr-video", power(2.4), false);
    add("Gamma 2.2", "Display", "sdr-video", power(2.2), false);
    add("Raw", "Utility", "", nullptr, true);
    cfg->setRole(OCIO::ROLE_REFERENCE, "Linear Rec.709 (sRGB)");
    cfg->setRole(OCIO::ROLE_SCENE_LINEAR, "Linear Rec.709 (sRGB)");
    cfg->setRole(OCIO::ROLE_DEFAULT, "Linear Rec.709 (sRGB)");
    cfg->setRole(OCIO::ROLE_DATA, "Raw");
    cfg->setRole(OCIO::ROLE_COLOR_PICKING, "sRGB");
    cfg->addDisplayView("sRGB Display", "sRGB", "sRGB", "");
    cfg->addDisplayView("sRGB Display", "Gamma 2.2", "Gamma 2.2", "");
    cfg->addDisplayView("sRGB Display", "Raw", "Raw", "");
    cfg->addDisplayView("Rec.709 Display", "Rec.709", "Rec.709", "");
    cfg->addDisplayView("Rec.709 Display", "BT.1886 (gamma 2.4)", "Gamma 2.4 (BT.1886)", "");
    cfg->addDisplayView("Rec.709 Display", "Raw", "Raw", "");
    cfg->setActiveDisplays("sRGB Display, Rec.709 Display");
    const double luma[3] = {0.2126, 0.7152, 0.0722};  // Rec.709 primaries
    cfg->setDefaultLumaCoefs(luma);
    cfg->validate();
    return cfg;
}

}  // namespace

std::vector<BuiltinConfig> ColorPipeline::builtinConfigs() {
    std::vector<BuiltinConfig> out;
    out.push_back({kLegacyConfigUri, "Linear sRGB / Rec.709 (legacy, no tone mapping)", false});
#if OCIO_VERSION_HEX >= 0x02020000
    const auto& reg = OCIO::BuiltinConfigRegistry::Get();
    for (size_t i = 0; i < reg.getNumBuiltinConfigs(); ++i) {
        BuiltinConfig b;
        b.name = std::string("ocio://") + reg.getBuiltinConfigName(i);
        b.uiName = reg.getBuiltinConfigUIName(i);
        b.recommended = reg.isBuiltinConfigRecommended(i);
        out.push_back(b);
    }
#endif
    return out;
}

bool ColorPipeline::load(const std::string& pathOrUri, std::string* error) {
    std::vector<std::string> candidates;
    if (!pathOrUri.empty()) {
        candidates.push_back(pathOrUri);
    } else {
        if (const char* env = std::getenv("OCIO"); env && *env) candidates.push_back(env);
#if OCIO_VERSION_HEX >= 0x02030000
        candidates.push_back("ocio://cg-config-latest");
#endif
#if OCIO_VERSION_HEX >= 0x02020000
        candidates.push_back("ocio://default");
#endif
    }
    std::string lastError;
    for (const auto& c : candidates) {
        try {
            auto cfg = c == kLegacyConfigUri ? makeLegacyConfig() : OCIO::Config::CreateFromFile(c.c_str());
            cfg->validate();
            impl_->config = cfg;
            source_ = c;
            pickDefaults();
            ++generation_;
            return true;
        } catch (const std::exception& e) {
            lastError = c + ": " + e.what();
            if (!pathOrUri.empty()) break;  // an explicit choice that fails is reported, not replaced
        }
    }
    if (error) *error = lastError.empty() ? "No OCIO config available" : lastError;
    if (pathOrUri.empty()) {  // nothing usable at all: raw (identity) config
        impl_->config = OCIO::Config::CreateRaw();
        source_ = "raw";
        pickDefaults();
        ++generation_;
    }
    return false;
}

bool ColorPipeline::loadFromString(const std::string& yaml, std::string* error) {
    try {
        std::istringstream in(yaml);
        auto cfg = OCIO::Config::CreateFromStream(in);
        cfg->validate();
        impl_->config = cfg;
        source_ = "<inline>";
        pickDefaults();
        ++generation_;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

std::string ColorPipeline::configName() const {
    const char* n = impl_->config->getName();
    return n && *n ? n : source_;
}

void ColorPipeline::pickDefaults() {
    const auto& cfg = impl_->config;
    display_ = cfg->getDefaultDisplay();
    view_ = cfg->getDefaultView(display_.c_str());
    look_.clear();
}

std::vector<std::string> ColorPipeline::displays() const {
    std::vector<std::string> out;
    for (int i = 0; i < impl_->config->getNumDisplays(); ++i) out.push_back(impl_->config->getDisplay(i));
    return out;
}

std::vector<std::string> ColorPipeline::views(const std::string& display) const {
    std::vector<std::string> out;
    for (int i = 0; i < impl_->config->getNumViews(display.c_str()); ++i)
        out.push_back(impl_->config->getView(display.c_str(), i));
    return out;
}

std::vector<std::string> ColorPipeline::looks() const {
    std::vector<std::string> out;
    for (int i = 0; i < impl_->config->getNumLooks(); ++i) out.push_back(impl_->config->getLookNameByIndex(i));
    return out;
}

std::vector<std::string> ColorPipeline::colorSpaces() const {
    std::vector<std::string> out;
    const auto& cfg = impl_->config;
    for (int i = 0; i < cfg->getNumColorSpaces(); ++i) out.push_back(cfg->getColorSpaceNameByIndex(i));
    return out;
}

bool ColorPipeline::hasColorSpace(const std::string& name) const {
    return !name.empty() && impl_->config->getColorSpace(name.c_str()) != nullptr;
}

bool ColorPipeline::setDisplayView(const std::string& display, const std::string& view, const std::string& look,
                                   std::string* error) {
    const auto ds = displays();
    if (std::find(ds.begin(), ds.end(), display) == ds.end()) {
        if (error) *error = "Unknown display: " + display;
        return false;
    }
    std::string v = view;
    const auto vs = views(display);
    if (std::find(vs.begin(), vs.end(), v) == vs.end()) v = impl_->config->getDefaultView(display.c_str());
    display_ = display;
    view_ = v;
    look_ = look;
    ++generation_;
    return true;
}

std::string ColorPipeline::sceneLinear() const {
    auto cs = impl_->config->getColorSpace(OCIO::ROLE_SCENE_LINEAR);
    if (cs) return cs->getName();
    return impl_->config->getNumColorSpaces() > 0 ? impl_->config->getColorSpaceNameByIndex(0) : "raw";
}

std::string ColorPipeline::guessInputSpace(const fs::path& file) const {
    const auto& cfg = impl_->config;
    try {
        size_t ruleIndex = 0;
        const std::string fromRules = cfg->getColorSpaceFromFilepath(file.generic_string().c_str(), ruleIndex);
        // The default rule (last one) is only a fallback; prefer a linear Rec.709 space then.
        const bool onlyDefault = cfg->getFileRules()->getNumEntries() == 0 ||
                                 ruleIndex + 1 >= cfg->getFileRules()->getNumEntries();
        if (!onlyDefault && hasColorSpace(fromRules)) return fromRules;
    } catch (...) {
    }
    for (const char* n : {"Linear Rec.709 (sRGB)", "lin_rec709_srgb", "lin_srgb", "Utility - Linear - sRGB", "linear"})
        if (hasColorSpace(n)) return cfg->getCanonicalName(n);
    return sceneLinear();
}

namespace {

ShaderFunction extract(const OCIO::ConstProcessorRcPtr& proc, const std::string& name, const std::string& prefix) {
    ShaderFunction out;
    auto desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    desc->setLanguage(OCIO::GPU_LANGUAGE_HLSL_DX11);
    desc->setFunctionName(name.c_str());
    desc->setResourcePrefix(prefix.c_str());
#if OCIO_VERSION_HEX >= 0x02030000
    desc->setAllowTexture1D(false);  // 1D LUTs arrive as 2D textures: one code path in the renderer
#endif
    proc->getDefaultGPUProcessor()->extractGpuShaderInfo(desc);
    out.code = desc->getShaderText();
    out.uniformCount = int(desc->getNumUniforms());

    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char* tex = nullptr;
        const char* samp = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType channel{};
        OCIO::Interpolation interp{};
#if OCIO_VERSION_HEX >= 0x02030000
        OCIO::GpuShaderDesc::TextureDimensions dims{};
        desc->getTexture(i, tex, samp, w, h, channel, dims, interp);
#else
        desc->getTexture(i, tex, samp, w, h, channel, interp);
#endif
        const float* values = nullptr;
        desc->getTextureValues(i, values);
        LutTexture t;
        t.textureName = tex;
        t.samplerName = samp;
        t.width = int(w);
        t.height = int(std::max(1u, h));
        t.channels = channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL ? 1 : 3;
        t.linear = interp != OCIO::INTERP_NEAREST;
        t.values.assign(values, values + size_t(t.width) * t.height * t.channels);
        out.luts.push_back(std::move(t));
    }
    for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
        const char* tex = nullptr;
        const char* samp = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interp{};
        desc->get3DTexture(i, tex, samp, edge, interp);
        const float* values = nullptr;
        desc->get3DTextureValues(i, values);
        LutTexture t;
        t.textureName = tex;
        t.samplerName = samp;
        t.is3D = true;
        t.width = t.height = t.depth = int(edge);
        t.channels = 3;
        t.linear = interp != OCIO::INTERP_NEAREST;
        t.values.assign(values, values + size_t(edge) * edge * edge * 3);
        out.luts.push_back(std::move(t));
    }
    return out;
}

// The scene_linear role's colour space, or the first colour space for configs without it
// (e.g. the raw fallback config).
std::string sceneLinearName(const OCIO::ConstConfigRcPtr& cfg) {
    if (auto cs = cfg->getColorSpace(OCIO::ROLE_SCENE_LINEAR)) return cs->getName();
    return cfg->getNumColorSpaces() > 0 ? cfg->getColorSpaceNameByIndex(0) : "raw";
}

OCIO::ConstProcessorRcPtr displayProcessor(const OCIO::ConstConfigRcPtr& cfg, const std::string& display,
                                           const std::string& view, const std::string& look) {
    auto dvt = OCIO::DisplayViewTransform::Create();
    dvt->setSrc(sceneLinearName(cfg).c_str());
    dvt->setDisplay(display.c_str());
    dvt->setView(view.c_str());
    if (look.empty()) return cfg->getProcessor(dvt);
    auto pipeline = OCIO::LegacyViewingPipeline::Create();
    pipeline->setDisplayViewTransform(dvt);
    pipeline->setLooksOverrideEnabled(true);
    pipeline->setLooksOverride(look.c_str());
    return pipeline->getProcessor(cfg);
}

}  // namespace

ShaderFunction ColorPipeline::inputShader(const std::string& colorSpace, const std::string& functionName,
                                          const std::string& resourcePrefix) const {
    const auto& cfg = impl_->config;
    const std::string src = hasColorSpace(colorSpace) ? colorSpace : sceneLinear();
    auto proc = cfg->getProcessor(src.c_str(), sceneLinearName(cfg).c_str());
    return extract(proc, functionName, resourcePrefix);
}

ShaderFunction ColorPipeline::displayShader(const std::string& functionName, const std::string& resourcePrefix) const {
    return extract(displayProcessor(impl_->config, display_, view_, look_), functionName, resourcePrefix);
}

void ColorPipeline::linearToDisplayCpu(float* rgba, int width, int height) const {
    if (!rgba || width <= 0 || height <= 0) return;
    auto cpu = displayProcessor(impl_->config, display_, view_, look_)->getDefaultCPUProcessor();
    OCIO::PackedImageDesc img(rgba, width, height, 4);
    cpu->apply(img);
}

void ColorPipeline::toDisplayCpu(const std::string& colorSpace, float* rgba, int width, int height) const {
    if (!rgba || width <= 0 || height <= 0) return;
    const std::string src = hasColorSpace(colorSpace) ? colorSpace : sceneLinear();
    auto toLinear = impl_->config->getProcessor(src.c_str(), sceneLinearName(impl_->config).c_str())->getDefaultCPUProcessor();
    OCIO::PackedImageDesc img(rgba, width, height, 4);
    toLinear->apply(img);
    linearToDisplayCpu(rgba, width, height);
}

std::array<float, 3> ColorPipeline::sceneLuma() const {
    std::array<float, 3> out{0.2126f, 0.7152f, 0.0722f};
    const auto& cfg = impl_->config;
    if (!cfg) return out;
    try {
        if (cfg->hasRole("cie_xyz_d65_interchange")) {
            auto cpu = cfg->getProcessor(sceneLinearName(cfg).c_str(), "cie_xyz_d65_interchange")->getDefaultCPUProcessor();
            for (int k = 0; k < 3; ++k) {
                float px[3] = {k == 0 ? 1.0f : 0.0f, k == 1 ? 1.0f : 0.0f, k == 2 ? 1.0f : 0.0f};
                cpu->applyRGB(px);
                out[size_t(k)] = px[1];
            }
            return out;
        }
        double c[3];
        cfg->getDefaultLumaCoefs(c);
        if (c[0] + c[1] + c[2] > 0.5) out = {float(c[0]), float(c[1]), float(c[2])};
    } catch (...) {
    }
    return out;
}

}  // namespace exrc
