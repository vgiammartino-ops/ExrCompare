# EXR Compare

A Windows 11 viewer for OpenEXR image sequences, built for reviewing renders and AOVs and for
comparing two sequences side by side, Nuke-viewer style.

**Download:** the latest build is on the [Releases page](../../releases/tag/latest):
- `ExrCompare-portable-x64.zip`: unzip anywhere (keep the folder together) and run `ExrCompare.exe`.

Nothing needs installing. Requires 64-bit Windows 10 (1809+) or Windows 11 and a Direct3D 11 GPU.

## Features

**Two layers (A and B)**
- Open a sequence into A or B (any frame, or a folder), or drop files on the window.
- Per layer:
  - AOV dropdown (the mouse wheel switches AOVs)
  - opacity (fade), visibility and frame offset
  - input colour space and alpha channel override
- **Lock AOV:** A and B switch AOV together.
- **Versions:** step a layer up or down through `v###` versions on disk (Alt+Up / Alt+Down).

**Compare modes** (Nuke-style top bar: `A [AOV]  [on/off] [mode]  B [AOV]`; the mouse wheel steps one entry per notch)
- A only, B only (click the orange A or B). **Double-click A or B to open a sequence into that input.**
- **Wipe:** a Nuke-style widget. Drag the centre to move it, the long handle to rotate, and the arc handle to dissolve B into A.
- **Stack:** B over A using the layer opacities.
- **Difference:** |A-B| in scene linear, with gain.
- **Side by side** and **over/under.**
- **Different resolutions:** B is fitted to A, or shown at the same pixel scale.

**Colour (OpenColorIO)**
- **Workflows** (Colour menu). The app always starts in **ACES 2.0** (OCIO's CG config, view "ACES 2.0 - SDR 100 nits").
- **Linear sRGB / Rec.709 (legacy)**, built in: linear Rec.709 primaries shown through plain curves with no tone mapping or gamut mapping. 1.0 shows as white and values above 1.0 clip, like Nuke's default viewer.
  - `sRGB Display`: views `sRGB` (0.18 shows as 0.461), `Gamma 2.2` and `Raw`.
  - `Rec.709 Display`: views `Rec.709` (camera curve, as Nuke's `rec709`; 0.18 shows as 0.409), `BT.1886 (gamma 2.4)` (0.18 shows as 0.489) and `Raw`.
  - Command line: `--workflow legacy` or `--workflow aces`.
- Load any `config.ocio` / `.ocioz`, use `$OCIO`, or pick a built-in ACES config, including the ACES 2.0 CG and Studio configs.
- Display / view / look selection, per-layer input colour space, exposure and gamma.
- Layers are compared in scene linear; the view is applied last.

**AOV review**
- Data AOVs are shown sensibly by default:
  - depth: auto near/far, invert
  - normals: remapped to colour
  - motion vectors: scaled around mid grey
  - IDs and Cryptomatte: random colours; hover shows object names; Alt+click isolates an object
  - other data (P, UV): raw
  - puzzle mattes (`PuzzleMatte_ObjectID`, `PuzzleMatte_MaterialID`, any name starting `PuzzleMatte`): raw, no OCIO, so R, G and B stay pure
- **AOV contact sheet:** every AOV of the frame in a grid; click a tile to view it.
- **Beauty rebuild check:** sum chosen AOVs and show |beauty - sum|.
- **Highlights:** NaN/Inf (magenta) and negative values (cyan), false-colour exposure map, alpha matte overlay, R/G/B/A/luminance channels.
- **Pixel readout** (Nuke's pixel sampler), for A and B at once, in a fixed two-row bar under the viewer:
  - Each row: resolution, bbox, channels, `x= y= WxH`, R G B A in scene linear (unclamped; above 1.0 shows as is), a swatch of the displayed colour, H S V and L.
  - Ctrl+drag draws a red box that stays sampled (its average is shown) after you let go; Ctrl+click pins one pixel. The box follows pan and zoom and is re-read on every frame and AOV. Esc or the clear button removes it.
- **View toggles** (bottom left, after Full screen): false colour, NaN/Inf highlight, alpha matte, half-resolution proxy, waveform monitor.
- **Waveform monitor** (Ctrl+Shift+W, View → Waveform monitor, or the bottom-left button): an overlay in a viewer corner, built for HDR.
  - Every pixel is counted on the GPU from the float data, before the view transform, so values above 1.0 keep their true place.
  - Default scale: **stops** (log2) around 18% grey, ±8 stops (0.0007 to 46). Lines every stop; labels show stops and linear values; 1.0 (display white) is marked; the frame's peak is a dashed yellow line.
  - Out-of-range values are never dropped: a red band at the top counts values above the scale, a blue band at the bottom counts values below it (and zero / negative on a log scale). The header shows the peak and the NaN/Inf count; hovering shows the value under the pointer.
  - Settings (gear): Luma (weights of the working colour space, e.g. ACEScg), RGB parade or RGB overlay; source = what's on screen (follows wipe and zoom), full frame A or B, or the red sample box; ±6/8/10/14 stops or linear 0–1/0–4/0–16; units = scene linear or **display light in nits** (after the view, decoded with the display's curve: sRGB, gamma 2.2/2.4/2.6 or PQ; peak from the view name, e.g. 100 or 1000 nits); corner and size.
  - Nits are only measured after the view transform; raw EXR values have no nits of their own.
- **Exposure, gamma and saturation** on their own line under the top bar (saturation 0 = greyscale, 1 = unchanged, up to 2; applied in scene linear around Rec.709 luminance). Key 0 resets all three.
- **Side pane** (layer cards) starts closed. Open or hide it with the pane button at the right end of the transport bar, View → Side pane, or Ctrl+I.
- A data-window outline is drawn on the image.
- **Stereo:** multi-view EXR view selector.

**Review workflow**
- Snapshots (PNG/JPEG) with burn-in text.
- Sessions (`.exrcs`) save layers, AOVs, colour settings, wipe and in/out.

**Playback**
- Exact 23.976 / 29.97 timing, loop on/off, in/out points, play backward, frame stepping.
- **Cache:** RV-style look-ahead or region modes, sharing one RAM budget between A and B, with a cache bar per layer.
- **Proxy:** half-resolution (reads mip level 1 when the EXR is tiled and mipmapped).
- Neighbouring AOVs are pre-read while paused, so Page Up/Down is instant.

Help > Keyboard shortcuts lists every key.

## How it's built

```
native/                 C++20 (CMake + vcpkg: OpenEXR, OpenColorIO, nlohmann-json)
  engine/               portable engine: sequences, EXR/AOV reading, cache, decode pool,
                        playback clock, OCIO pipeline, compare layout, analysis, Session (JSON API)
  render/               Direct3D 11 compositor (HLSL assembled with OCIO-generated code), snapshots
  capi/                 exrcore.dll: C interface used by the app
  tests/                engine tests (any OS) + headless render test (Windows, WARP)
app/ExrCompare/         C# WinUI 3 app (Windows App SDK, unpackaged + self-contained)
.github/workflows/      CI: build, test, publish, release
```

Every push to `main` builds, tests and publishes a new "latest" release on a GitHub Windows
runner. CI logs are pushed to the `ci-logs` branch.

### Building locally

Install Visual Studio 2022/2026 ("Desktop development with C++"), the .NET 10 SDK and git, then run:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1            # portable folder
powershell -ExecutionPolicy Bypass -File build.ps1 -SingleFile
```

The first build compiles OpenEXR and OpenColorIO through vcpkg (about 20 minutes).
