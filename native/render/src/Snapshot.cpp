#include "Snapshot.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
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
}  // namespace

void drawOverlay(ID3D11Texture2D* texture, const Overlay& overlay) {
    if (overlay.burnIn.empty()) return;
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);

    ComPtr<ID2D1Factory> d2d;
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()), "D2D1CreateFactory");
    ComPtr<IDXGISurface> surface;
    check(texture->QueryInterface(IID_PPV_ARGS(&surface)), "QueryInterface(IDXGISurface)");
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1RenderTarget> rt;
    check(d2d->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &rt), "CreateDxgiSurfaceRenderTarget");

    rt->BeginDraw();
    // Burn-in text, bottom-left, on a translucent band.
    if (!overlay.burnIn.empty()) {
        ComPtr<IDWriteFactory> dw;
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(dw.GetAddressOf())),
              "DWriteCreateFactory");
        const float size = std::clamp(float(td.Height) / 45.0f, 11.0f, 48.0f);
        ComPtr<IDWriteTextFormat> fmt;
        check(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &fmt),
              "CreateTextFormat");
        std::wstring text;
        for (size_t i = 0; i < overlay.burnIn.size(); ++i) text += (i ? L"\n" : L"") + overlay.burnIn[i];
        ComPtr<IDWriteTextLayout> layout;
        check(dw->CreateTextLayout(text.c_str(), UINT32(text.size()), fmt.Get(), float(td.Width), float(td.Height), &layout),
              "CreateTextLayout");
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const float pad = size * 0.5f;
        const float top = float(td.Height) - m.height - pad * 2;
        ComPtr<ID2D1SolidColorBrush> band, ink;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &band);
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &ink);
        rt->FillRectangle(D2D1::RectF(0, top, m.width + pad * 2, float(td.Height)), band.Get());
        rt->DrawTextLayout(D2D1::Point2F(pad, top + pad), layout.Get(), ink.Get());
    }
    check(rt->EndDraw(), "EndDraw");
}

void writeImage(const std::wstring& path, const uint8_t* bgra, int width, int height, bool jpeg) {
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)), "CoCreateInstance(WIC)");
    ComPtr<IWICStream> stream;
    check(wic->CreateStream(&stream), "CreateStream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open snapshot file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(wic->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Encoder Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    check(encoder->CreateNewFrame(&frame, &options), "CreateNewFrame");
    if (jpeg && options) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.95f;
        options->Write(1, &opt, &v);
    }
    check(frame->Initialize(options.Get()), "Frame Initialize");
    check(frame->SetSize(UINT(width), UINT(height)), "SetSize");
    if (jpeg) {
        std::vector<uint8_t> bgr(size_t(width) * height * 3);
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            bgr[i * 3 + 0] = bgra[i * 4 + 0];
            bgr[i * 3 + 1] = bgra[i * 4 + 1];
            bgr[i * 3 + 2] = bgra[i * 4 + 2];
        }
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        check(frame->SetPixelFormat(&pf), "SetPixelFormat");
        check(frame->WritePixels(UINT(height), UINT(width) * 3, UINT(bgr.size()), bgr.data()), "WritePixels");
    } else {
        std::vector<uint8_t> opaque(bgra, bgra + size_t(width) * height * 4);
        for (size_t i = 0; i < size_t(width) * height; ++i) opaque[i * 4 + 3] = 255;
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&pf), "SetPixelFormat");
        check(frame->WritePixels(UINT(height), UINT(width) * 4, UINT(opaque.size()), opaque.data()), "WritePixels");
    }
    check(frame->Commit(), "Frame Commit");
    check(encoder->Commit(), "Encoder Commit");
}

}  // namespace exrc
