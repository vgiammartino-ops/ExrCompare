/*
 * exrcore.dll - C interface used by the C# WinUI app (P/Invoke).
 *
 * Almost everything goes through exrc_call() as JSON: {"cmd": "...", ...} in, a JSON object out.
 * Only the hot or binary paths have their own functions (panel attach/resize, waveform pixels).
 * Strings are UTF-8. All functions are safe to call from the UI thread; rendering happens on a
 * thread owned by the DLL.
 */
#ifndef EXRCORE_H
#define EXRCORE_H

#ifdef EXRCORE_BUILDING
#define EXRC_API __declspec(dllexport)
#else
#define EXRC_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ExrcHandle ExrcHandle;

/* configJson: {"cacheBudget": bytes, "ocio": "config path or ocio:// URI", "decodeThreads": n}
 * (any field optional; NULL or "" for defaults). */
EXRC_API ExrcHandle* exrc_create(const char* configJson);
EXRC_API void exrc_destroy(ExrcHandle* h);

/* Runs a JSON command/query. Writes the JSON result (NUL-terminated) into out and returns its
 * length without the NUL. If the return value >= capacity the result was truncated: call again
 * with a larger buffer. Extra commands handled here (not by the engine):
 *   {"cmd":"snapshot","path":"C:/x.png","width":w,"height":h,"burnin":["line",...]}
 *   {"cmd":"render_status"}  -> {"error": "...", "shaderError": "...", "attached": bool} */
EXRC_API int exrc_call(ExrcHandle* h, const char* json, char* out, int capacity);

/* Binds rendering to a WinUI 3 SwapChainPanel. panel = its IUnknown/IInspectable pointer.
 * Must be called on the panel's UI thread. Sizes are physical pixels. Returns 0 on success. */
EXRC_API int exrc_attach_panel(ExrcHandle* h, void* panel, int pixelWidth, int pixelHeight, float scaleX, float scaleY);
EXRC_API void exrc_resize(ExrcHandle* h, int pixelWidth, int pixelHeight, float scaleX, float scaleY);
EXRC_API void exrc_detach(ExrcHandle* h);

/* Waveform scope of a layer's current frame: writes width*height*4 RGBA bytes. 0 on success. */
EXRC_API int exrc_waveform(ExrcHandle* h, int slot, int width, int height, unsigned char* rgba);

#ifdef __cplusplus
}
#endif

#endif
