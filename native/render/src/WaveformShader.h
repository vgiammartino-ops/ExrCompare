#pragma once

// Waveform monitor shaders.
//
// kWaveCompute is appended to the compositor source (header + OCIO functions + main), so it sees
// the same textures, constants and OCIO transforms as the viewer. It counts every source pixel into
// bins (layout in exrc/Waveform.h): scene-linear working-space values, or display light in nits.
// kWaveDraw is a standalone pixel shader that draws the counted bins as the overlay.

namespace exrc {

inline constexpr char kWaveCompute[] = R"HLSL(
cbuffer WaveParams : register(b1)
{
    uint4  wSize;    // cols, rows, source (0 viewer, 1 A texels, 2 B texels), units (0 scene, 1 nits)
    int4   wArea;    // x0, y0, x1, y1 (end exclusive)
    float4 wScale;   // lo, hi, log (1) / linear (0), include exposure (scene units)
    float4 wLuma;    // scene luma weights, display peak nits
    float4 wDisp;    // display luma weights, EOTF (0 sRGB, 1 gamma 2.2, 2 gamma 2.4, 3 PQ, 4 gamma 2.6)
};
RWByteAddressBuffer WaveBins : register(u0);

// Display code value -> light (0..1 of peak; PQ: absolute nits / 10000).
float WaveDecode(float e, uint eotf)
{
    e = saturate(e);
    if (eotf == 0u) return e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4);
    if (eotf == 1u) return pow(e, 2.2);
    if (eotf == 2u) return pow(e, 2.4);
    if (eotf == 4u) return pow(e, 2.6);
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(e, 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

void WaveCount(uint plane, uint col, float v)
{
    uint cols = wSize.x, rows = wSize.y;
    uint tail = 4u * (rows + 2u) * cols;
    if (isnan(v) || isinf(v))
    {
        WaveBins.InterlockedAdd((tail + plane) * 4u, 1u);
        return;
    }
    uint r = 0u;  // below the scale (or <= 0 on a log scale)
    if (wScale.z < 0.5 || v > 0.0)
    {
        float t = ((wScale.z > 0.5 ? log2(v) : v) - wScale.x) / (wScale.y - wScale.x);
        if (t >= 1.0) r = rows + 1u;
        else if (t >= 0.0) r = 1u + min(uint(t * float(rows)), rows - 1u);
    }
    WaveBins.InterlockedAdd(((plane * (rows + 2u) + r) * cols + col) * 4u, 1u);
    if (plane < 3u && v > 0.0)
    {
        uint old;
        WaveBins.InterlockedMax((tail + 4u) * 4u, asuint(v), old);  // positive floats order like uints
    }
}

bool WaveFetch(Texture2D<float4> tex, float2 p, float4 rect, uint4 L, out float4 s)
{
    s = float4(0, 0, 0, 0);
    if (L.w == 0u) return false;
    float2 uv = (p - rect.xy) / max(rect.zw - rect.xy, float2(1e-6, 1e-6));
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return false;
    uint tw, th;
    tex.GetDimensions(tw, th);
    if (L.z != 0u)
        s = tex.Load(int3(min(int2(uv * float2(tw, th)), int2(int(tw) - 1, int(th) - 1)), 0));
    else
        s = tex.SampleLevel(LinearS, uv, 0);
    return true;
}

// Working-space scene-linear value, or display light in nits.
float3 WaveValue(float4 s, uint4 L, bool isA)
{
    if (L.y == 1u) s = float4(s.rrr, 1.0);
    float3 c = s.rgb;
    if (any(isnan(c)) || any(isinf(c))) return c;  // counted as NaN / Inf per channel
    if (L.x == 0u) c = (isA ? OCIO_InputA(float4(c, 1.0)) : OCIO_InputB(float4(c, 1.0))).rgb;
    if (wSize.w == 0u) return wScale.w > 0.5 ? c * misc.z : c;
    // Display light: the viewer's signal decoded with the display's EOTF, in nits.
    c *= misc.z;
    float3 e = L.x == 0u ? OCIO_Display(float4(c, 1.0)).rgb : saturate(c);
    uint eotf = uint(wDisp.w + 0.5);
    float k = eotf == 3u ? 10000.0 : wLuma.w;
    return float3(WaveDecode(e.r, eotf), WaveDecode(e.g, eotf), WaveDecode(e.b, eotf)) * k;
}

[numthreads(16, 16, 1)]
void WaveCS(uint3 id : SV_DispatchThreadID)
{
    int2 px = int2(id.xy) + wArea.xy;
    if (px.x >= wArea.z || px.y >= wArea.w) return;
    uint cols = wSize.x;
    uint col = min(uint(px.x - wArea.x) * cols / uint(wArea.z - wArea.x), cols - 1u);
    float3 v;
    if (wSize.z == 0u)
    {
        // Viewer: per viewport pixel, the layer(s) this pixel shows.
        float2 p = float2(px) + 0.5;
        float4 sa, sb;
        bool va = WaveFetch(TexA, p, rectA, layerA, sa);
        bool vb = WaveFetch(TexB, p, rectB, layerB, sb);
        uint mode = modes.x;
        if (mode == 0u) vb = false;
        if (mode == 1u) va = false;
        if (!va && !vb) return;
        float3 a = va ? WaveValue(sa, layerA, true) : float3(0, 0, 0);
        float3 b = vb ? WaveValue(sb, layerB, false) : float3(0, 0, 0);
        if (mode == 2u && va && vb)
        {
            float2 n = float2(cos(wipe.z), sin(wipe.z));
            float wB = lerp(misc.x, 1.0, step(0.0, dot(p - wipe.xy, n)));
            v = wB >= 0.5 ? b : a;
        }
        else if (mode == 3u && va && vb) v = lerp(a, b, paramsB.x);
        else if (mode == 4u && va && vb) v = abs(a - b) * misc.y;
        else v = va ? a : b;
    }
    else
    {
        // Full frame / sample box of one layer: every texel.
        bool isA = wSize.z == 1u;
        uint4 L = isA ? layerA : layerB;
        if (L.w == 0u) return;
        float4 s = isA ? TexA.Load(int3(px, 0)) : TexB.Load(int3(px, 0));
        v = WaveValue(s, L, isA);
    }
    float3 w = wSize.w == 0u ? wLuma.xyz : wDisp.xyz;
    WaveCount(0u, col, v.r);
    WaveCount(1u, col, v.g);
    WaveCount(2u, col, v.b);
    WaveCount(3u, col, dot(v, w));
}
)HLSL";

inline constexpr char kWaveDraw[] = R"HLSL(
cbuffer WaveDraw : register(b0)
{
    float4 plot;       // trace area, render-target pixels
    float4 box;        // whole overlay incl. header
    uint4  dims;       // cols, rows, mode (0 luma, 1 parade, 2 overlay), line count
    float4 extra;      // source pixels per column, peak t (0..1 of the trace height, < 0 none)
    float4 lines[16];  // graticule: (t, strength) pairs, two per float4
};
ByteAddressBuffer Bins : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

uint Bin(uint plane, uint row, uint col)
{
    return Bins.Load(((plane * (dims.y + 2u) + row) * dims.x + col) * 4u);
}

static const float kBand = 3.0;  // out-of-range bands at the top (above) and bottom (below / <= 0)

float4 WavePS(float4 pos : SV_Position) : SV_Target
{
    float2 p = pos.xy;
    // Output is premultiplied: rgb is added on top of image * (1 - alpha).
    if (p.y < plot.y || p.x < plot.x || p.x >= plot.z || p.y >= plot.w)
    {
        bool edge = p.x < box.x + 1.0 || p.x >= box.z - 1.0 || p.y < box.y + 1.0 || p.y >= box.w - 1.0;
        return float4(edge ? float3(0.25, 0.25, 0.25) : float3(0.035, 0.035, 0.035), 0.88);
    }
    uint cols = dims.x, rows = dims.y, mode = dims.z;
    float pw = plot.z - plot.x, inner = plot.w - plot.y - 2.0 * kBand;
    float lx = (p.x - plot.x) / pw;
    float panels = mode == 1u ? 3.0 : 1.0;
    float k = floor(lx * panels);
    float px0 = lx * panels - k;
    float px1 = px0 + panels / pw;
    uint ca = min(uint(px0 * cols), cols - 1u);
    uint cb = clamp(uint(ceil(px1 * cols)), ca + 1u, cols);

    // Rows this pixel covers: top band = above, bottom band = below, else in-range rows.
    float yFromBottom = plot.w - kBand - p.y;  // pixel centre
    uint ra, rb;
    bool band = false;
    if (p.y < plot.y + kBand) { ra = rows + 1u; rb = rows + 2u; band = true; }
    else if (p.y >= plot.w - kBand) { ra = 0u; rb = 1u; band = true; }
    else
    {
        float t0 = (yFromBottom - 0.5) / inner, t1 = (yFromBottom + 0.5) / inner;
        ra = 1u + min(uint(max(t0, 0.0) * rows), rows - 1u);
        rb = clamp(1u + uint(ceil(saturate(t1) * rows)), ra + 1u, rows + 1u);
    }

    uint first = mode == 0u ? 3u : (mode == 1u ? uint(k) : 0u);
    uint last = mode == 0u ? 3u : (mode == 1u ? uint(k) : 2u);
    float3 sum = float3(0, 0, 0);
    float lum = 0.0;
    [loop] for (uint c = ca; c < cb; ++c)
    {
        [loop] for (uint r = ra; r < rb; ++r)
        {
            [loop] for (uint pl = first; pl <= last; ++pl)
            {
                float n = float(Bin(pl, r, c));
                if (pl == 0u) sum.r += n;
                else if (pl == 1u) sum.g += n;
                else if (pl == 2u) sum.b += n;
                else lum += n;
            }
        }
    }
    float span = float((cb - ca) * (rb - ra));
    float ref = max(1.0, extra.x * span / float(rows) * 24.0);
    float norm = 1.0 / log2(1.0 + ref);
    float3 I = saturate(log2(1.0 + sum) * norm);
    float IL = saturate(log2(1.0 + lum) * norm);
    I = max(I, (sum > 0.0) * 0.2);
    IL = max(IL, (lum > 0.0) * 0.2);

    float3 trace;
    if (mode == 0u) trace = IL * float3(0.9, 0.97, 0.9);
    else if (mode == 2u) trace = I;
    else
    {
        float3 tint = k < 0.5 ? float3(1.0, 0.32, 0.3) : (k < 1.5 ? float3(0.35, 1.0, 0.4) : float3(0.4, 0.58, 1.0));
        trace = tint * max(I.r, max(I.g, I.b));
    }
    if (band)
    {
        float m = mode == 0u ? IL : max(I.r, max(I.g, I.b));
        trace = (ra == 0u ? float3(0.3, 0.45, 1.0) : float3(1.0, 0.18, 0.1)) * m;
    }

    float3 grid = float3(0, 0, 0);
    [loop] for (uint i = 0u; i < dims.w; ++i)
    {
        float4 pair = lines[i / 2u];
        float2 l = (i & 1u) ? pair.zw : pair.xy;
        float yl = plot.w - kBand - l.x * inner;
        if (abs(p.y - yl) < 0.5) grid = max(grid, l.y * 0.32);
    }
    if (extra.y >= 0.0)
    {
        float yp = plot.w - kBand - extra.y * inner;
        if (abs(p.y - yp) < 0.5 && fmod(p.x, 6.0) < 3.0) grid = max(grid, float3(0.75, 0.6, 0.15));
    }
    if (mode == 1u && (abs(px0 * pw / panels) < 0.5 && k > 0.5)) grid = max(grid, 0.3);
    return float4(saturate(0.035 + grid + trace), 0.88);
}
)HLSL";

}  // namespace exrc
