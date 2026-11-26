#include "exrcore.h"

#include "Snapshot.h"
#include "exrc/Renderer.h"
#include "exrc/Session.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

using json = nlohmann::json;
using namespace exrc;

// WinUI 3 (Microsoft.UI.Xaml) ISwapChainPanelNative - declared here so the DLL builds with only
// the Windows SDK (the header ships in the Windows App SDK NuGet package).
MIDL_INTERFACE("63aad0b8-7c24-40ff-85a8-640d944cc325")
ISwapChainPanelNativeWinUI : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain* swapChain) = 0;
};

// True when two render states would draw exactly the same picture (lets the render thread idle).
static bool sameLayer(const LayerRender& a, const LayerRender& b) {
    return a.frame == b.frame && a.present == b.present && a.visible == b.visible && a.rect.x0 == b.rect.x0 &&
           a.rect.y0 == b.rect.y0 && a.rect.x1 == b.rect.x1 && a.rect.y1 == b.rect.y1 && a.mode == b.mode &&
           a.opacity == b.opacity && a.depthNear == b.depthNear && a.depthFar == b.depthFar &&
           a.depthInvert == b.depthInvert && a.motionScale == b.motionScale && a.isolateId == b.isolateId &&
           a.hasIsolate == b.hasIsolate;
}
static bool sameState(const RenderState& a, const RenderState& b) {
    return a.hasContent == b.hasContent && sameLayer(a.a, b.a) && sameLayer(a.b, b.b) && a.compare == b.compare &&
           a.wipeX == b.wipeX && a.wipeY == b.wipeY && a.wipeAngle == b.wipeAngle && a.wipeMix == b.wipeMix &&
           a.wipeSoftness == b.wipeSoftness && a.diffGain == b.diffGain && a.exposure == b.exposure &&
           a.gamma == b.gamma && a.channel == b.channel && a.flags == b.flags && a.program == b.program;
}

struct ExrcHandle {
    std::unique_ptr<Session> session;

    // Live view (render thread).
    std::unique_ptr<Renderer> renderer;
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex m;
    int width = 1, height = 1;
    float scaleX = 1, scaleY = 1;
    bool resizePending = false;
    std::string renderError;
    std::string shaderError;

    // Snapshots render on the calling thread with their own device.
    std::unique_ptr<Renderer> still;

    void renderLoop() {
        RenderState last;
        bool hasLast = false;
        while (running) {
            if (HANDLE w = renderer->frameLatencyWaitable()) WaitForSingleObjectEx(w, 100, TRUE);
            if (!running) break;
            int w, h;
            float sx, sy;
            bool resize;
            {
                std::lock_guard lock(m);
                w = width;
                h = height;
                sx = scaleX;
                sy = scaleY;
                resize = resizePending;
                resizePending = false;
            }
            try {
                if (resize) renderer->resize(UINT(w), UINT(h), sx, sy);
                RenderState rs = session->update(Clock::now(), int(renderer->width()), int(renderer->height()));
                if (!resize && hasLast && sameState(rs, last)) {
                    Sleep(2);  // nothing changed: keep the last picture, don't burn the GPU
                    continue;
                }
                renderer->render(rs);
                last = std::move(rs);
                hasLast = true;
                const std::string se = renderer->lastShaderError();
                std::lock_guard lock(m);
                shaderError = se;
                if (renderer->deviceLost()) {
                    renderError = "The graphics device was removed or reset. Restart the app.";
                    running = false;
                }
            } catch (const std::exception& e) {
                std::lock_guard lock(m);
                renderError = e.what();
                running = false;
            }
        }
    }

    json snapshot(const json& j) {
        int w = j.value("width", 0), h = j.value("height", 0);
        if (w <= 0 || h <= 0) {
            if (!session->canvasSize(w, h)) throw std::runtime_error("Nothing loaded");
        }
        w = std::clamp(w, 16, 16384);
        h = std::clamp(h, 16, 16384);
        const RenderState rs = session->stillState(w, h);
        if (!still) {
            still = std::make_unique<Renderer>();
            const char* warp = std::getenv("EXRC_FORCE_WARP");
            still->createDevice(warp && *warp == '1');
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC needs COM; harmless if already initialised
        auto tex = still->renderOffscreen(rs, UINT(w), UINT(h));
        Overlay ov;
        for (const auto& line : j.value("burnin", json::array())) {
            const std::string s = line.get<std::string>();
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
            std::wstring ws(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), ws.data(), n);
            ov.burnIn.push_back(ws);
        }
        drawOverlay(tex.Get(), ov);
        const std::vector<uint8_t> pixels = still->readback(tex.Get(), UINT(w), UINT(h));
        const std::string path = j.value("path", "");
        if (path.empty()) throw std::runtime_error("snapshot: path required");
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), int(path.size()), nullptr, 0);
        std::wstring wpath(size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), int(path.size()), wpath.data(), n);
        std::string fmt = j.value("format", "");
        if (fmt.empty()) {
            const auto dot = path.find_last_of('.');
            fmt = dot == std::string::npos ? "png" : path.substr(dot + 1);
        }
        const bool jpeg = fmt == "jpg" || fmt == "jpeg" || fmt == "JPG" || fmt == "JPEG";
        writeImage(wpath, pixels.data(), w, h, jpeg);
        // Centre pixel, for tests.
        const size_t mid = (size_t(h / 2) * w + w / 2) * 4;
        return {{"path", path}, {"width", w}, {"height", h}, {"shaderError", still->lastShaderError()},
                {"centerBGRA", {pixels[mid], pixels[mid + 1], pixels[mid + 2], pixels[mid + 3]}}};
    }

    void stopRendering() {
        running = false;
        if (thread.joinable()) thread.join();
    }
};

extern "C" {

EXRC_API ExrcHandle* exrc_create(const char* configJson) {
    try {
        SessionConfig cfg;
        if (configJson && *configJson) {
            const json j = json::parse(configJson);
            if (j.contains("cacheBudget")) cfg.cacheBudgetBytes = j["cacheBudget"].get<size_t>();
            if (j.contains("ocio")) cfg.ocioConfig = j["ocio"].get<std::string>();
            if (j.contains("decodeThreads")) cfg.decodeThreads = j["decodeThreads"].get<int>();
        }
        auto h = new ExrcHandle();
        h->session = std::make_unique<Session>(cfg);
        return h;
    } catch (...) {
        return nullptr;
    }
}

EXRC_API void exrc_destroy(ExrcHandle* h) {
    if (!h) return;
    h->stopRendering();
    delete h;
}

EXRC_API int exrc_call(ExrcHandle* h, const char* text, char* out, int capacity) {
    std::string result;
    if (!h || !text) {
        result = R"({"ok":false,"error":"invalid handle"})";
    } else {
        try {
            const json j = json::parse(text);
            const std::string cmd = j.value("cmd", "");
            if (cmd == "snapshot") {
                json r = h->snapshot(j);
                r["ok"] = true;
                result = r.dump(-1, ' ', false, json::error_handler_t::replace);
            } else if (cmd == "render_status") {
                std::lock_guard lock(h->m);
                result = json{{"ok", true}, {"error", h->renderError}, {"shaderError", h->shaderError},
                              {"attached", h->running.load()}}
                             .dump(-1, ' ', false, json::error_handler_t::replace);
            } else {
                result = h->session->call(text);
            }
        } catch (const std::exception& e) {
            result = json{{"ok", false}, {"error", e.what()}}.dump(-1, ' ', false, json::error_handler_t::replace);
        }
    }
    if (out && capacity > 0) {
        const size_t n = std::min(result.size(), size_t(capacity - 1));
        std::memcpy(out, result.data(), n);
        out[n] = '\0';
    }
    return int(result.size());
}

EXRC_API int exrc_attach_panel(ExrcHandle* h, void* panel, int w, int ht, float sx, float sy) {
    if (!h || !panel) return -1;
    if (h->running) return 0;
    try {
        h->renderer = std::make_unique<Renderer>();
        const char* warp = std::getenv("EXRC_FORCE_WARP");
        h->renderer->createDevice(warp && *warp == '1');
        IDXGISwapChain1* sc = h->renderer->createSwapChain(UINT(std::max(1, w)), UINT(std::max(1, ht)));
        Microsoft::WRL::ComPtr<ISwapChainPanelNativeWinUI> native;
        const HRESULT hr = static_cast<IUnknown*>(panel)->QueryInterface(__uuidof(ISwapChainPanelNativeWinUI),
                                                                          reinterpret_cast<void**>(native.GetAddressOf()));
        if (FAILED(hr)) throw std::runtime_error("SwapChainPanel does not expose ISwapChainPanelNative");
        if (FAILED(native->SetSwapChain(sc))) throw std::runtime_error("SetSwapChain failed (call from the UI thread)");
    } catch (const std::exception& e) {
        std::lock_guard lock(h->m);
        h->renderError = e.what();
        h->renderer.reset();
        return -2;
    }
    {
        std::lock_guard lock(h->m);
        h->width = w;
        h->height = ht;
        h->scaleX = sx;
        h->scaleY = sy;
        h->resizePending = true;
        h->renderError.clear();
    }
    h->running = true;
    h->thread = std::thread([h] { h->renderLoop(); });
    return 0;
}

EXRC_API void exrc_resize(ExrcHandle* h, int w, int ht, float sx, float sy) {
    if (!h) return;
    std::lock_guard lock(h->m);
    h->width = std::max(1, w);
    h->height = std::max(1, ht);
    h->scaleX = sx;
    h->scaleY = sy;
    h->resizePending = true;
}

EXRC_API void exrc_detach(ExrcHandle* h) {
    if (h) h->stopRendering();
}

EXRC_API int exrc_waveform(ExrcHandle* h, int slot, int w, int ht, unsigned char* rgba) {
    if (!h || !rgba || w <= 0 || ht <= 0) return -1;
    std::vector<uint8_t> img;
    if (!h->session->waveform(slot, w, ht, img)) return -2;
    std::memcpy(rgba, img.data(), img.size());
    return 0;
}

}  // extern "C"
