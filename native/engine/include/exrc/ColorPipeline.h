// OpenColorIO integration.
//
// The viewer works like Nuke's: each layer is converted from its input colour space to the
// config's scene_linear role, layers are compared/composited in linear, then one display/view
// (+ optional look) transform produces monitor values. OCIO generates the HLSL for each step;
// the renderer compiles it and uploads the LUT textures listed here.
//
// Not thread-safe: Session calls it under its own lock.
#pragma once

#include "exrc/Types.h"

namespace exrc {

struct LutTexture {
    std::string textureName;
    std::string samplerName;
    bool is3D = false;
    int width = 0, height = 1, depth = 1;  // 1D LUTs have height 1 (or are wrapped into 2D rows)
    int channels = 3;                       // 1 (red only) or 3 (RGB)
    bool linear = true;                     // interpolation: linear vs nearest
    std::vector<float> values;              // width*height*depth*channels
};

struct ShaderFunction {
    std::string code;                // HLSL: float4 <name>(float4 inPixel)
    std::vector<LutTexture> luts;
    int uniformCount = 0;            // dynamic properties (unsupported: reported, not set)
};

// Built into this app (not OCIO's registry): plain linear sRGB / Rec.709 primaries with sRGB,
// Rec.709 (camera curve), BT.1886 and gamma views and no tone mapping, like Nuke's default viewer.
inline constexpr const char* kLegacyConfigUri = "ocio://exrcompare-legacy-srgb";

struct BuiltinConfig {
    std::string name;    // "ocio://..." URI
    std::string uiName;
    bool recommended = false;
};

class ColorPipeline {
public:
    ColorPipeline();
    ~ColorPipeline();

    // pathOrUri: a config file, an .ocioz archive or an "ocio://" built-in URI.
    // Empty = $OCIO if set, otherwise the latest built-in ACES studio config.
    bool load(const std::string& pathOrUri, std::string* error);
    bool loadFromString(const std::string& yaml, std::string* error);  // tests

    std::string source() const { return source_; }
    std::string configName() const;
    std::string ocioVersion() const;
    static std::vector<BuiltinConfig> builtinConfigs();

    std::vector<std::string> displays() const;
    std::vector<std::string> views(const std::string& display) const;
    std::vector<std::string> looks() const;
    std::vector<std::string> colorSpaces() const;

    std::string display() const { return display_; }
    std::string view() const { return view_; }
    std::string look() const { return look_; }
    bool setDisplayView(const std::string& display, const std::string& view, const std::string& look, std::string* error);

    std::string sceneLinear() const;  // name of the scene_linear role's colour space
    // Default input space for a file: the config's file rules, else a linear Rec.709 space if the
    // config has one, else scene_linear.
    std::string guessInputSpace(const fs::path& file) const;
    bool hasColorSpace(const std::string& name) const;

    // HLSL for input colour space -> scene_linear, and scene_linear -> display/view/look.
    ShaderFunction inputShader(const std::string& colorSpace, const std::string& functionName,
                               const std::string& resourcePrefix) const;
    ShaderFunction displayShader(const std::string& functionName, const std::string& resourcePrefix) const;

    // CPU path for scopes: converts linear RGBA pixels of `colorSpace` to display values in place.
    void toDisplayCpu(const std::string& colorSpace, float* rgba, int width, int height) const;
    // scene_linear -> display (for values that are already scene linear).
    void linearToDisplayCpu(float* rgba, int width, int height) const;

    uint64_t generation() const { return generation_; }  // bumps on any change affecting shaders

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string source_;
    std::string display_, view_, look_;
    uint64_t generation_ = 1;
    void pickDefaults();
};

}  // namespace exrc
