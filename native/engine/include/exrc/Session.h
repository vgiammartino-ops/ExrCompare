// Session: two layer slots (A and B), one timeline, one cache, one colour pipeline.
//
// The UI drives it with JSON commands/queries through call(); the render thread asks update()
// once per display refresh for everything it needs to draw. Thread-safe.
#pragma once

#include "exrc/ColorPipeline.h"
#include "exrc/CompareLayout.h"
#include "exrc/DecodeScheduler.h"
#include "exrc/PlaybackController.h"

#include <deque>
#include <mutex>

namespace exrc {

enum class CacheMode { Off = 0, LookAhead, Region };

// How a layer's pixels are turned into something viewable.
enum class LayerMode : uint32_t { Color = 0, Raw, Depth, Normal, Motion, Id };

struct SessionConfig {
    int decodeThreads = 0;  // 0 = auto
    int exrThreads = -1;    // -1 = auto
    size_t cacheBudgetBytes = size_t(4) << 30;
    std::string ocioConfig;  // "" = $OCIO or built-in
};

// Compiled into the shader: OCIO functions for both inputs and the display.
struct ShaderProgram {
    ShaderFunction inputA;    // float4 OCIO_InputA(float4)
    ShaderFunction inputB;    // float4 OCIO_InputB(float4)
    ShaderFunction display;   // float4 OCIO_Display(float4)
    uint64_t id = 0;          // changes whenever any part changes
};

struct LayerRender {
    FramePtr frame;      // null while decoding (renderer keeps its previous texture)
    bool present = false;
    bool visible = true;
    PixelRect rect{0, 0, 0, 0};
    LayerMode mode = LayerMode::Color;
    float opacity = 1.0f;
    float depthNear = 0.0f, depthFar = 1.0f;
    bool depthInvert = false;
    float motionScale = 0.05f;
    uint32_t isolateId = 0;  // cryptomatte / ID isolate (0 = none)
    bool hasIsolate = false;
};

enum RenderFlags : uint32_t {
    kFlagNanHighlight = 1u << 0,
    kFlagFalseColor = 1u << 1,
    kFlagMatteOverlay = 1u << 2,
};

struct RenderState {
    bool hasContent = false;
    LayerRender a, b;
    CompareMode compare = CompareMode::A;
    float wipeX = 0, wipeY = 0;  // viewport pixels
    float wipeAngle = 0;         // radians; 0 = vertical wipe line, B on the right
    float wipeMix = 0;           // 0..1: how much of B shows on A's side (arc handle)
    float wipeSoftness = 0;      // pixels
    float diffGain = 1.0f;
    float exposure = 1.0f;       // linear multiplier (2^stops)
    float gamma = 1.0f;
    int channel = 0;             // 0 RGB, 1 R, 2 G, 3 B, 4 A, 5 luminance
    uint32_t flags = 0;
    float background[4] = {0, 0, 0, 1};
    std::shared_ptr<const ShaderProgram> program;
    int globalFrame = 0;
};

class Session {
public:
    explicit Session(SessionConfig config = {});
    ~Session();

    // JSON in, JSON out: {"cmd": "...", ...} -> {"ok": true, ...} or {"ok": false, "error": "..."}.
    std::string call(const std::string& json);

    // Render thread, once per refresh. Viewport in physical pixels.
    RenderState update(TimePoint now, int viewportW, int viewportH);
    // A still for snapshots: fit view at the given size, current frame, no playback.
    RenderState stillState(int width, int height);
    // Full-resolution size of what is on screen (both layers for side-by-side), for snapshots.
    bool canvasSize(int& width, int& height);

    // Scopes: waveform of the given slot's current frame as RGBA8 (outW*outH*4 bytes).
    bool waveform(int slot, int outW, int outH, std::vector<uint8_t>& rgba);

    void waitForDecodes();  // tests

private:
    struct Slot;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace exrc
