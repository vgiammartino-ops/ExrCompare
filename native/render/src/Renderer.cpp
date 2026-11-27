#include "exrc/Renderer.h"

#include "CompositorShader.h"
#include "WaveformShader.h"
#include "exrc/Waveform.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace exrc {

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

struct alignas(16) Constants {
    float rectA[4];
    float rectB[4];
    float wipe[4];
    float misc[4];
    uint32_t modes[4];
    uint32_t layerA[4];
    uint32_t layerB[4];
    float paramsA[4];
    float paramsB[4];
    uint32_t extraA[4];
    uint32_t extraB[4];
    float background[4];
};
static_assert(sizeof(Constants) == 192, "must match the HLSL cbuffer layout");

// Used until (or if) OCIO provides a program: identity input, sRGB-ish display.
const char* kFallbackOcio = R"HLSL(
float4 OCIO_InputA(float4 c) { return c; }
float4 OCIO_InputB(float4 c) { return c; }
float4 OCIO_Display(float4 c)
{
    float3 x = saturate(c.rgb);
    return float4(lerp(12.92 * x, 1.055 * pow(x, 1.0 / 2.4) - 0.055, step(0.0031308, x)), c.a);
}
)HLSL";

// OCIO's HLSL samples its LUTs with Texture.Sample(), which needs screen-space derivatives and so
// can't sit inside dynamic branches. LUTs have a single mip level, so SampleLevel(.., 0) is
// identical and lets the compositor skip work per pixel.
std::string explicitLod(const std::string& code) {
    std::string out;
    out.reserve(code.size() + 64);
    size_t pos = 0;
    while (true) {
        const size_t hit = code.find(".Sample(", pos);
        if (hit == std::string::npos) {
            out.append(code, pos, std::string::npos);
            break;
        }
        out.append(code, pos, hit - pos);
        out += ".SampleLevel(";
        size_t i = hit + 8;
        int depth = 1;
        const size_t argsStart = i;
        while (i < code.size() && depth > 0) {
            if (code[i] == '(') ++depth;
            else if (code[i] == ')') --depth;
            if (depth > 0) ++i;
        }
        out.append(code, argsStart, i - argsStart);
        out += ", 0)";
        pos = i + 1;
    }
    return out;
}

DXGI_FORMAT formatFor(const Frame& f) {
    if (f.format == PixelFormat::Half) return f.channels == 1 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
    return f.channels == 1 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
}

void rectOf(const PixelRect& r, float out[4]) {
    out[0] = float(r.x0);
    out[1] = float(r.y0);
    out[2] = float(r.x1);
    out[3] = float(r.y1);
}

}  // namespace

Renderer::~Renderer() {
    if (context_) context_->ClearState();
}

void Renderer::createDevice(bool warp) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // Direct2D burn-ins on snapshots
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = E_FAIL;
    if (!warp)
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_,
                               nullptr, &context_);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &device_,
                               nullptr, &context_);
    check(hr, "D3D11CreateDevice");

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device_->CreateBuffer(&cb, nullptr, &constants_), "CreateBuffer(constants)");

    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    check(device_->CreateSamplerState(&sd, &linearSampler_), "CreateSamplerState");

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    check(device_->CreateRasterizerState(&rd, &raster_), "CreateRasterizerState");

    if (!ensureProgram(nullptr)) throw std::runtime_error("Fallback shader failed to compile: " + shaderError_);
}

IDXGISwapChain1* Renderer::createSwapChain(UINT width, UINT height) {
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    ComPtr<IDXGIDevice> dxgiDevice;
    check(device_.As(&dxgiDevice), "QueryInterface(IDXGIDevice)");
    ComPtr<IDXGIAdapter> adapter;
    check(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent(IDXGIFactory2)");

    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width_;
    d.Height = height_;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // not sRGB: the shader applies the display transform
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    check(factory->CreateSwapChainForComposition(device_.Get(), &d, nullptr, &sc1), "CreateSwapChainForComposition");
    check(sc1.As(&swapChain_), "QueryInterface(IDXGISwapChain2)");
    check(swapChain_->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
    waitable_ = swapChain_->GetFrameLatencyWaitableObject();
    createRenderTarget();
    return sc1.Get();
}

void Renderer::createRenderTarget() {
    ComPtr<ID3D11Texture2D> back;
    check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    check(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_), "CreateRenderTargetView");
}

void Renderer::resize(UINT width, UINT height, float scaleX, float scaleY) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (!swapChain_) return;
    if (width != width_ || height != height_) {
        rtv_.Reset();
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        context_->Flush();
        const HRESULT hr = swapChain_->ResizeBuffers(2, width, height, DXGI_FORMAT_UNKNOWN,
                                                     DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            deviceLost_ = true;
            return;
        }
        check(hr, "ResizeBuffers");
        width_ = width;
        height_ = height;
        createRenderTarget();
    }
    DXGI_MATRIX_3X2_F m{};  // buffer is in physical pixels: undo the panel's DPI scale
    m._11 = scaleX > 0 ? 1.0f / scaleX : 1.0f;
    m._22 = scaleY > 0 ? 1.0f / scaleY : 1.0f;
    swapChain_->SetMatrixTransform(&m);
}

bool Renderer::ensureProgram(const std::shared_ptr<const ShaderProgram>& program) {
    const uint64_t id = program ? program->id : 0;
    if (ps_ && id == programId_) return true;

    std::string source = kCompositorHeader;
    if (program) {
        source += explicitLod(program->inputA.code);
        source += explicitLod(program->inputB.code);
        source += explicitLod(program->display.code);
    } else {
        source += kFallbackOcio;
    }
    source += kCompositorMain;
    source_ = source;

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& code) {
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(source.data(), source.size(), "Compositor.hlsl", nullptr, nullptr, entry, target,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            shaderError_ = std::string(entry) + ": " +
                           (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                                   : "compile failed");
            return false;
        }
        return true;
    };
    ComPtr<ID3DBlob> vsCode, psCode;
    if (!compile("VSMain", "vs_5_0", vsCode) || !compile("PSMain", "ps_5_0", psCode)) {
        programId_ = id;  // don't retry a broken program every frame
        return ps_ != nullptr;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    check(device_->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), "CreateVertexShader");
    check(device_->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), "CreatePixelShader");

    // OCIO's LUT textures and samplers have no fixed registers: find where the compiler put them.
    std::vector<LutBinding> luts;
    if (program) {
        ComPtr<ID3D11ShaderReflection> reflect;
        check(D3DReflect(psCode->GetBufferPointer(), psCode->GetBufferSize(), IID_PPV_ARGS(&reflect)), "D3DReflect");
        for (const ShaderFunction* fn : {&program->inputA, &program->inputB, &program->display}) {
            for (const LutTexture& t : fn->luts) {
                D3D11_SHADER_INPUT_BIND_DESC texBind{}, sampBind{};
                if (FAILED(reflect->GetResourceBindingDescByName(t.textureName.c_str(), &texBind))) continue;  // optimised away
                if (FAILED(reflect->GetResourceBindingDescByName(t.samplerName.c_str(), &sampBind))) continue;
                LutBinding b;
                b.textureSlot = texBind.BindPoint;
                b.samplerSlot = sampBind.BindPoint;
                b.textureName = t.textureName;
                b.samplerName = t.samplerName;
                // RGB LUTs are padded to RGBA (RGB32F textures can't always be sampled).
                const size_t texels = size_t(t.width) * t.height * t.depth;
                std::vector<float> data;
                DXGI_FORMAT fmt = DXGI_FORMAT_R32_FLOAT;
                if (t.channels == 1) {
                    data = t.values;
                } else {
                    fmt = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    data.resize(texels * 4);
                    for (size_t i = 0; i < texels; ++i) {
                        data[i * 4 + 0] = t.values[i * 3 + 0];
                        data[i * 4 + 1] = t.values[i * 3 + 1];
                        data[i * 4 + 2] = t.values[i * 3 + 2];
                        data[i * 4 + 3] = 1.0f;
                    }
                }
                const UINT texelBytes = t.channels == 1 ? 4 : 16;
                D3D11_SUBRESOURCE_DATA init{};
                init.pSysMem = data.data();
                init.SysMemPitch = UINT(t.width) * texelBytes;
                init.SysMemSlicePitch = UINT(t.width * t.height) * texelBytes;
                if (t.is3D) {
                    D3D11_TEXTURE3D_DESC td{};
                    td.Width = UINT(t.width);
                    td.Height = UINT(t.height);
                    td.Depth = UINT(t.depth);
                    td.MipLevels = 1;
                    td.Format = fmt;
                    td.Usage = D3D11_USAGE_IMMUTABLE;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ComPtr<ID3D11Texture3D> tex;
                    check(device_->CreateTexture3D(&td, &init, &tex), "CreateTexture3D(LUT)");
                    check(device_->CreateShaderResourceView(tex.Get(), nullptr, &b.srv), "CreateShaderResourceView(LUT3D)");
                } else {
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = UINT(t.width);
                    td.Height = UINT(t.height);
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = fmt;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_IMMUTABLE;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    ComPtr<ID3D11Texture2D> tex;
                    check(device_->CreateTexture2D(&td, &init, &tex), "CreateTexture2D(LUT)");
                    check(device_->CreateShaderResourceView(tex.Get(), nullptr, &b.srv), "CreateShaderResourceView(LUT)");
                }
                D3D11_SAMPLER_DESC sd{};
                sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.MaxLOD = D3D11_FLOAT32_MAX;
                sd.Filter = t.linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
                check(device_->CreateSamplerState(&sd, &b.sampler), "CreateSamplerState(LUT)");
                luts.push_back(std::move(b));
            }
        }
    }
    vs_ = vs;
    ps_ = ps;
    luts_ = std::move(luts);
    programId_ = id;
    shaderError_.clear();
    return true;
}

void Renderer::upload(LayerTexture& lt, const FramePtr& frame) {
    if (!frame || frame == lt.frame) return;
    lt.frame = frame;
    if (frame->missing || frame->width <= 0 || frame->height <= 0 || frame->data.empty()) {
        lt.valid = false;
        return;
    }
    const DXGI_FORMAT fmt = formatFor(*frame);
    if (!lt.texture || lt.width != frame->width || lt.height != frame->height || lt.format != fmt) {
        lt.srv.Reset();
        lt.texture.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(frame->width);
        td.Height = UINT(frame->height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device_->CreateTexture2D(&td, nullptr, &lt.texture), "CreateTexture2D(image)");
        check(device_->CreateShaderResourceView(lt.texture.Get(), nullptr, &lt.srv), "CreateShaderResourceView(image)");
        lt.width = frame->width;
        lt.height = frame->height;
        lt.format = fmt;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(lt.texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(image)");
    const size_t rowBytes = frame->rowBytes();
    const uint8_t* src = frame->data.data();
    auto* dst = static_cast<uint8_t*>(mapped.pData);
    if (mapped.RowPitch == rowBytes) {
        std::memcpy(dst, src, rowBytes * frame->height);
    } else {
        for (int y = 0; y < frame->height; ++y) std::memcpy(dst + size_t(y) * mapped.RowPitch, src + y * rowBytes, rowBytes);
    }
    context_->Unmap(lt.texture.Get(), 0);
    lt.valid = true;
}

void Renderer::draw(const RenderState& s, UINT width, UINT height, ID3D11RenderTargetView* target) {
    context_->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT vp{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    context_->RSSetViewports(1, &vp);
    context_->ClearRenderTargetView(target, s.background);
    if (!s.hasContent) return;

    ensureProgram(s.program);
    if (s.a.present) upload(texA_, s.a.frame);
    if (s.b.present) upload(texB_, s.b.frame);

    Constants c{};
    rectOf(s.a.rect, c.rectA);
    rectOf(s.b.rect, c.rectB);
    c.wipe[0] = s.wipeX;
    c.wipe[1] = s.wipeY;
    c.wipe[2] = s.wipeAngle;
    c.wipe[3] = s.wipeSoftness;
    c.misc[0] = s.wipeMix;
    c.misc[1] = s.diffGain;
    c.misc[2] = s.exposure;
    c.misc[3] = 1.0f / (s.gamma > 0.01f ? s.gamma : 1.0f);
    c.modes[0] = uint32_t(s.compare);
    c.modes[1] = uint32_t(s.channel);
    c.modes[2] = s.flags;
    std::copy(std::begin(s.background), std::end(s.background), c.background);
    c.background[3] = s.saturation;  // background.w carries viewer saturation (alpha is unused)

    auto fillLayer = [](const LayerRender& l, const LayerTexture& t, uint32_t L[4], float P[4], uint32_t X[4]) {
        const bool valid = l.present && l.visible && t.valid;
        L[0] = uint32_t(l.mode);
        L[1] = t.frame ? uint32_t(t.frame->channels) : 4u;
        const double drawnWidth = l.rect.x1 - l.rect.x0;
        // Real pixels when zoomed in, and always for IDs (filtering would invent IDs).
        L[2] = (l.mode == LayerMode::Id || (t.width > 0 && drawnWidth >= t.width * 0.999)) ? 1u : 0u;
        L[3] = valid ? 1u : 0u;
        P[0] = l.opacity;
        P[1] = l.depthNear;
        P[2] = l.depthFar;
        P[3] = l.motionScale;
        X[0] = l.depthInvert ? 1u : 0u;
        X[1] = l.hasIsolate ? 1u : 0u;
        X[2] = l.isolateId;
    };
    fillLayer(s.a, texA_, c.layerA, c.paramsA, c.extraA);
    fillLayer(s.b, texB_, c.layerB, c.paramsB, c.extraB);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);

    ID3D11Buffer* cb = constants_.Get();
    ID3D11ShaderResourceView* srvs[2] = {c.layerA[3] ? texA_.srv.Get() : nullptr, c.layerB[3] ? texB_.srv.Get() : nullptr};
    ID3D11SamplerState* samp = linearSampler_.Get();
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->RSSetState(raster_.Get());
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, &samp);
    for (const LutBinding& l : luts_) {
        ID3D11ShaderResourceView* srv = l.srv.Get();
        ID3D11SamplerState* ls = l.sampler.Get();
        context_->PSSetShaderResources(l.textureSlot, 1, &srv);
        context_->PSSetSamplers(l.samplerSlot, 1, &ls);
    }
    context_->Draw(3, 0);
}

void Renderer::render(const RenderState& state) {
    if (!rtv_ || deviceLost_) return;
    draw(state, width_, height_, rtv_.Get());
    if (state.hasContent && state.wave.enabled && ensureWaveform(state)) {
        dispatchWaveform(state);
        drawWaveform(state, rtv_.Get());
    }
    const HRESULT hr = swapChain_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) deviceLost_ = true;
}

ComPtr<ID3D11Texture2D> Renderer::renderOffscreen(const RenderState& state, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    check(device_->CreateTexture2D(&td, nullptr, &tex), "CreateTexture2D(offscreen)");
    ComPtr<ID3D11RenderTargetView> rtv;
    check(device_->CreateRenderTargetView(tex.Get(), nullptr, &rtv), "CreateRenderTargetView(offscreen)");
    draw(state, width, height, rtv.Get());
    context_->Flush();
    return tex;
}

std::vector<uint8_t> Renderer::readback(ID3D11Texture2D* texture, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    check(device_->CreateTexture2D(&td, nullptr, &staging), "CreateTexture2D(staging)");
    context_->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");
    std::vector<uint8_t> out(size_t(width) * height * 4);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(out.data() + size_t(y) * width * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                    size_t(width) * 4);
    context_->Unmap(staging.Get(), 0);
    return out;
}

// ------------------------------------------------------------------ waveform --

namespace {

struct alignas(16) WaveParamsCB {
    uint32_t size[4];  // cols, rows, source, units
    int32_t area[4];
    float scale[4];    // lo, hi, log, include exposure
    float luma[4];     // scene luma, peak nits
    float disp[4];     // display luma, eotf
};

struct alignas(16) WaveDrawCB {
    float plot[4];
    float box[4];
    uint32_t dims[4];  // cols, rows, mode, line count
    float extra[4];    // density, peak t
    float lines[16][4];
};

}  // namespace

bool Renderer::ensureWaveform(const RenderState& s) {
    try {
        if (!wavePs_) {
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(D3DCompile(kWaveDraw, std::strlen(kWaveDraw), "WaveDraw.hlsl", nullptr, nullptr, "WavePS", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
                waveError_ = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                                    : "WavePS compile failed";
                return false;
            }
            check(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &wavePs_), "CreatePixelShader(wave)");
            D3D11_BUFFER_DESC bd{};
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.ByteWidth = sizeof(WaveParamsCB);
            check(device_->CreateBuffer(&bd, nullptr, &waveParams_), "CreateBuffer(wave params)");
            bd.ByteWidth = sizeof(WaveDrawCB);
            check(device_->CreateBuffer(&bd, nullptr, &waveDraw_), "CreateBuffer(wave draw)");
            D3D11_BUFFER_DESC td{};
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            td.ByteWidth = 5 * 4;
            check(device_->CreateBuffer(&td, nullptr, &waveTail_), "CreateBuffer(wave tail)");
            D3D11_BLEND_DESC blend{};
            auto& rt = blend.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_ONE;  // premultiplied
            rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            check(device_->CreateBlendState(&blend, &waveBlend_), "CreateBlendState(wave)");
        }
        // The compute shader shares the compositor's OCIO code: rebuild it with the program.
        if (!waveCs_ || waveProgramId_ != programId_) {
            waveProgramId_ = programId_;
            waveCs_.Reset();
            waveLutSlots_.clear();
            const std::string src = source_ + kWaveCompute;
            ComPtr<ID3DBlob> code, errors;
            if (FAILED(D3DCompile(src.data(), src.size(), "Waveform.hlsl", nullptr, nullptr, "WaveCS", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
                waveError_ = "WaveCS: " + (errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                                                  : std::string("compile failed"));
                return false;
            }
            check(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &waveCs_), "CreateComputeShader(wave)");
            ComPtr<ID3D11ShaderReflection> reflect;
            check(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&reflect)), "D3DReflect(wave)");
            for (const LutBinding& l : luts_) {
                D3D11_SHADER_INPUT_BIND_DESC tb{}, sb{};
                if (FAILED(reflect->GetResourceBindingDescByName(l.textureName.c_str(), &tb)) ||
                    FAILED(reflect->GetResourceBindingDescByName(l.samplerName.c_str(), &sb))) {
                    waveLutSlots_.emplace_back(UINT(-1), UINT(-1));  // not used by the compute shader
                    continue;
                }
                waveLutSlots_.emplace_back(tb.BindPoint, sb.BindPoint);
            }
            waveError_.clear();
        }
        const size_t count = waveBinCount(s.wave.cols, s.wave.rows);
        if (count != waveBinCount_) {
            waveBins_.Reset();
            waveUav_.Reset();
            waveSrv_.Reset();
            D3D11_BUFFER_DESC bd{};
            bd.Usage = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
            bd.ByteWidth = UINT(count * 4);
            check(device_->CreateBuffer(&bd, nullptr, &waveBins_), "CreateBuffer(wave bins)");
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = UINT(count);
            ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
            check(device_->CreateUnorderedAccessView(waveBins_.Get(), &ud, &waveUav_), "CreateUnorderedAccessView(wave)");
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            sd.BufferEx.NumElements = UINT(count);
            sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            check(device_->CreateShaderResourceView(waveBins_.Get(), &sd, &waveSrv_), "CreateShaderResourceView(wave)");
            waveBinCount_ = count;
            waveTailPending_ = false;
        }
        return true;
    } catch (const std::exception& e) {
        waveError_ = e.what();
        return false;
    }
}

// Counts on the GPU. Needs draw() first (it uploads the textures and fills the constants).
void Renderer::dispatchWaveform(const RenderState& s) {
    const WaveformRender& w = s.wave;
    // Peak / NaN of the previous dispatch, if the copy has landed (never stalls).
    if (waveTailPending_) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context_->Map(waveTail_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            const auto* t = static_cast<const uint32_t*>(m.pData);
            waveNan_ = t[3];
            std::memcpy(&wavePeak_, &t[4], 4);
            context_->Unmap(waveTail_.Get(), 0);
            waveTailPending_ = false;
        }
    }

    WaveParamsCB p{};
    p.size[0] = uint32_t(w.cols);
    p.size[1] = uint32_t(w.rows);
    p.size[2] = uint32_t(w.source);
    p.size[3] = uint32_t(w.units);
    for (int i = 0; i < 4; ++i) p.area[i] = w.area[i];
    p.scale[0] = w.lo;
    p.scale[1] = w.hi;
    p.scale[2] = w.logScale ? 1.0f : 0.0f;
    p.scale[3] = w.includeExposure ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i) p.luma[i] = w.sceneLuma[i], p.disp[i] = w.displayLuma[i];
    p.luma[3] = w.peakNits;
    p.disp[3] = float(w.eotf);
    context_->UpdateSubresource(waveParams_.Get(), 0, nullptr, &p, 0, 0);

    const UINT zero[4] = {0, 0, 0, 0};
    context_->ClearUnorderedAccessViewUint(waveUav_.Get(), zero);

    const bool va = s.a.present && s.a.visible && texA_.valid, vb = s.b.present && s.b.visible && texB_.valid;
    ID3D11Buffer* cbs[2] = {constants_.Get(), waveParams_.Get()};
    ID3D11ShaderResourceView* srvs[2] = {va ? texA_.srv.Get() : nullptr, vb ? texB_.srv.Get() : nullptr};
    ID3D11SamplerState* samp = linearSampler_.Get();
    ID3D11UnorderedAccessView* uav = waveUav_.Get();
    context_->CSSetShader(waveCs_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 2, cbs);
    context_->CSSetShaderResources(0, 2, srvs);
    context_->CSSetSamplers(0, 1, &samp);
    for (size_t i = 0; i < luts_.size() && i < waveLutSlots_.size(); ++i) {
        if (waveLutSlots_[i].first == UINT(-1)) continue;
        ID3D11ShaderResourceView* srv = luts_[i].srv.Get();
        ID3D11SamplerState* ls = luts_[i].sampler.Get();
        context_->CSSetShaderResources(waveLutSlots_[i].first, 1, &srv);
        context_->CSSetSamplers(waveLutSlots_[i].second, 1, &ls);
    }
    context_->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    const UINT aw = UINT(std::max(0, w.area[2] - w.area[0])), ah = UINT(std::max(0, w.area[3] - w.area[1]));
    context_->Dispatch((aw + 15) / 16, (ah + 15) / 16, 1);
    ID3D11UnorderedAccessView* noUav = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ID3D11ShaderResourceView* noSrv[2] = {nullptr, nullptr};
    context_->CSSetShaderResources(0, 2, noSrv);

    if (!waveTailPending_) {
        D3D11_BOX tail{};
        tail.left = UINT((waveBinCount_ - 5) * 4);
        tail.right = UINT(waveBinCount_ * 4);
        tail.top = 0, tail.bottom = 1, tail.front = 0, tail.back = 1;
        context_->CopySubresourceRegion(waveTail_.Get(), 0, 0, 0, 0, waveBins_.Get(), 0, &tail);
        waveTailPending_ = true;
    }
}

void Renderer::drawWaveform(const RenderState& s, ID3D11RenderTargetView* target) {
    const WaveformRender& w = s.wave;
    WaveDrawCB d{};
    d.plot[0] = float(w.plot.x0), d.plot[1] = float(w.plot.y0), d.plot[2] = float(w.plot.x1), d.plot[3] = float(w.plot.y1);
    d.box[0] = float(w.box.x0), d.box[1] = float(w.box.y0), d.box[2] = float(w.box.x1), d.box[3] = float(w.box.y1);
    d.dims[0] = uint32_t(w.cols);
    d.dims[1] = uint32_t(w.rows);
    d.dims[2] = uint32_t(w.mode);
    const size_t lines = std::min<size_t>(w.lines.size(), 32);
    d.dims[3] = uint32_t(lines);
    for (size_t i = 0; i < lines; ++i) {
        d.lines[i / 2][(i & 1) * 2 + 0] = w.lines[i].first;
        d.lines[i / 2][(i & 1) * 2 + 1] = w.lines[i].second;
    }
    d.extra[0] = w.density;
    d.extra[1] = -1.0f;
    if (wavePeak_ > 0.0f && std::isfinite(wavePeak_)) {
        const float v = w.logScale ? std::log2(wavePeak_) : wavePeak_;
        const float t = (v - w.lo) / (w.hi - w.lo);
        if (t >= 0.0f && t <= 1.0f) d.extra[1] = t;
    }
    context_->UpdateSubresource(waveDraw_.Get(), 0, nullptr, &d, 0, 0);

    D3D11_VIEWPORT vp{float(w.box.x0), float(w.box.y0), float(w.box.x1 - w.box.x0), float(w.box.y1 - w.box.y0), 0.0f, 1.0f};
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &vp);
    const float factor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(waveBlend_.Get(), factor, 0xffffffff);
    ID3D11Buffer* cb = waveDraw_.Get();
    ID3D11ShaderResourceView* srv = waveSrv_.Get();
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(wavePs_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context_->PSSetShaderResources(0, 1, &none);
    context_->OMSetBlendState(nullptr, factor, 0xffffffff);
}

std::vector<uint32_t> Renderer::computeWaveform(const RenderState& state, UINT width, UINT height) {
    std::vector<uint32_t> out;
    if (!state.hasContent || !state.wave.enabled) return out;
    renderOffscreen(state, width, height);  // uploads the frames and fills the constants
    if (!ensureWaveform(state)) return out;
    dispatchWaveform(state);
    D3D11_BUFFER_DESC bd{};
    waveBins_->GetDesc(&bd);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.BindFlags = 0;
    bd.MiscFlags = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    check(device_->CreateBuffer(&bd, nullptr, &staging), "CreateBuffer(wave readback)");
    context_->CopyResource(staging.Get(), waveBins_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    check(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m), "Map(wave readback)");
    out.resize(waveBinCount_);
    std::memcpy(out.data(), m.pData, waveBinCount_ * 4);
    context_->Unmap(staging.Get(), 0);
    return out;
}

}  // namespace exrc
