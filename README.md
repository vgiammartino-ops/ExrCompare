# EXR Compare

A Windows 11 viewer for OpenEXR image sequences, built for reviewing renders and AOVs and for
comparing two sequences side by side, Nuke-viewer style.

**Download:** the latest build is on the [Releases page](../../releases/tag/latest):
- `ExrCompare-portable-x64.zip`: unzip anywhere and run `ExrCompare.exe`.
- `ExrCompare-x64.exe`: a single file; it unpacks to a temp folder on first run.

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

**Compare modes** (toolbar dropdown, next to the AOV dropdown; the mouse wheel steps one entry per notch)
- A only, B only
- **Wipe:** a Nuke-style widget. Drag the centre to move it, the long handle to rotate, and the arc handle to dissolve B into A. Edge softness is adjustable.
- **Stack:** B over A using the layer opacities.
- **Difference:** |A-B| in scene linear, with gain.
- **Side by side** and **over/under.**
- **Different resolutions:** B is fitted to A, or shown at the same pixel scale.

**Colour (OpenColorIO)**
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
- **AOV contact sheet:** every AOV of the frame in a grid; click a tile to view it.
- **Beauty rebuild check:** sum chosen AOVs and show |beauty - sum|.
- **Highlights:** NaN/Inf (magenta) and negative values (cyan), false-colour exposure map, alpha matte overlay, R/G/B/A/luminance channels.
- **Pixel Analyzer** (Nuke's pixel sampler), for A and B at once:
  - Hover reads the pixel's R, G, B and A in scene linear, unclamped (values above 1.0 show as is), plus a swatch of the displayed colour.
  - Ctrl+drag draws a red box that stays sampled after you let go; Ctrl+click pins one pixel. The box follows pan and zoom and is re-read on every frame and AOV.
  - The Pixel tab shows current, min, max, average and median per channel, with NaN/Inf counts. Esc or Clear sample removes the box.
  - A readout bar under the viewer shows `x= y= WxH  R G B A [swatch]`, like Nuke's viewer.
- Per-channel frame statistics.
- **Histogram and waveform** scopes.
- **Metadata panel:** every EXR header attribute. A data-window outline is drawn on the image.
- **Stereo:** multi-view EXR view selector.

**Review workflow**
- Per-frame notes, exported as CSV.
- Snapshots (PNG/JPEG) with burn-in text.
- Sessions (`.exrcs`) save layers, AOVs, colour settings, wipe, in/out and notes.

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
