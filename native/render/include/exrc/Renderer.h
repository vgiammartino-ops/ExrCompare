// Direct3D 11 compositor. Draws a RenderState (layers A/B + compare mode + OCIO program) into
// either a composition swap chain (for the WinUI SwapChainPanel) or an offscreen texture.
//
// Threading: one Renderer is used by one thread. The swap chain is created on the UI thread
// (it must be handed to the panel there); every other call happens on the render thread.
#pragma once

#include "exrc/Session.h"

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

namespace exrc {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // warp: software rasteriser (tests / machines without a GPU). Falls back to WARP if no
    // hardware device can be created.
    void createDevice(bool warp = false);
    IDXGISwapChain1* createSwapChain(UINT width, UINT height);
    HANDLE frameLatencyWaitable() const { return waitable_; }
    void resize(UINT width, UINT height, float compositionScaleX, float compositionScaleY);
    UINT width() const { return width_; }
    UINT height() const { return height_; }

    void render(const RenderState& state);  // draw + present (vsync)
    // Draws into a new BGRA texture of the given size (snapshots). Returns the texture.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> renderOffscreen(const RenderState& state, UINT width, UINT height);
    // Copies a BGRA texture to CPU memory (tightly packed rows).
    std::vector<uint8_t> readback(ID3D11Texture2D* texture, UINT width, UINT height);

    ID3D11Device* device() const { return device_.Get(); }
    bool deviceLost() const { return deviceLost_; }
    std::string lastShaderError() const { return shaderError_; }

private:
    struct LayerTexture {
        FramePtr frame;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool valid = false;  // holds a real (non-missing) image
    };
    struct LutBinding {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        UINT textureSlot = 0, samplerSlot = 0;
    };

    void createRenderTarget();
    void upload(LayerTexture& lt, const FramePtr& frame);
    bool ensureProgram(const std::shared_ptr<const ShaderProgram>& program);
    void draw(const RenderState& state, UINT width, UINT height, ID3D11RenderTargetView* target);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    std::vector<LutBinding> luts_;
    uint64_t programId_ = 0;
    std::string shaderError_;

    LayerTexture texA_, texB_;
    HANDLE waitable_ = nullptr;
    UINT width_ = 1, height_ = 1;
    bool deviceLost_ = false;
};

}  // namespace exrc
