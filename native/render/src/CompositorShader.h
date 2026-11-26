// HLSL for the compositor. The full program is assembled at runtime:
//   kCompositorHeader + OCIO_InputA + OCIO_InputB + OCIO_Display (generated) + kCompositorMain
// and compiled with D3DCompile (ps_5_0 / vs_5_0).
//
// Per pixel, for each layer: fetch -> layer mode (colour via OCIO input transform / raw / depth /
// normal / motion / ID) -> exposure, channel, gamma in scene linear -> display transform.
// Then the compare mode combines A and B: A, B, wipe (Nuke-style line + softness + mix), stack
// (B over A with opacity), difference (in linear), side-by-side / over-under (layout only).
#pragma once

namespace exrc {

inline constexpr char kCompositorHeader[] = R"HLSL(
cbuffer Params : register(b0)
{
    float4 rectA;       // viewport pixels: x0, y0, x1, y1
    float4 rectB;
    float4 wipe;        // x, y (pixels), angle (radians), softness (pixels)
    float4 misc;        // wipeMix, diffGain, exposure (linear), 1/gamma
    uint4  modes;       // compare mode, channel view, flags, unused
    uint4  layerA;      // mode, channels, point sampling, valid
    uint4  layerB;
    float4 paramsA;     // opacity, depthNear, depthFar, motionScale
    float4 paramsB;
    uint4  extraA;      // depthInvert, hasIsolate, isolateId, unused
    uint4  extraB;
    float4 background;
};

Texture2D<float4> TexA : register(t0);
Texture2D<float4> TexB : register(t1);
SamplerState LinearS : register(s0);
)HLSL";

inline constexpr char kCompositorMain[] = R"HLSL(
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const uint FLAG_NAN = 1u;
static const uint FLAG_FALSE = 2u;
static const uint FLAG_MATTE = 4u;

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    // Full-screen triangle.
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float3 Turbo(float t)
{
    // Polynomial approximation of the Turbo colour map (Google, Apache-2.0).
    const float4 kRedVec4   = float4(0.13572138, 4.61539260, -42.66032258, 132.13108234);
    const float4 kGreenVec4 = float4(0.09140261, 2.19418839, 4.84296658, -14.18503333);
    const float4 kBlueVec4  = float4(0.10667330, 12.64194608, -60.58204836, 110.36276771);
    const float2 kRedVec2   = float2(-152.94239396, 59.28637943);
    const float2 kGreenVec2 = float2(4.27729857, 2.82956604);
    const float2 kBlueVec2  = float2(-89.90310912, 27.34824973);
    t = saturate(t);
    float4 v4 = float4(1.0, t, t * t, t * t * t);
    float2 v2 = v4.zw * v4.z;
    return float3(dot(v4, kRedVec4) + dot(v2, kRedVec2),
                  dot(v4, kGreenVec4) + dot(v2, kGreenVec2),
                  dot(v4, kBlueVec4) + dot(v2, kBlueVec2));
}

float3 IdColor(uint id)
{
    uint h = id * 2654435761u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
    return float3(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u) / 255.0 * 0.85 + 0.15;
}

struct LayerOut
{
    float3 disp;      // display-referred colour
    float3 lin;       // scene-linear colour (colour/raw modes)
    bool valid;       // pixel lies inside this layer's image
    bool colorMode;   // went through the OCIO input + display transforms
};

LayerOut EvalLayer(Texture2D<float4> tex, float2 p, float4 rect, uint4 L, float4 P, uint4 X, bool isA)
{
    LayerOut o;
    o.disp = float3(0, 0, 0);
    o.lin = float3(0, 0, 0);
    o.valid = false;
    o.colorMode = false;
    if (L.w == 0u) return o;
    float2 uv = (p - rect.xy) / max(rect.zw - rect.xy, float2(1e-6, 1e-6));
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x >= 1.0 || uv.y >= 1.0) return o;
    o.valid = true;

    uint tw, th;
    tex.GetDimensions(tw, th);
    float4 s;
    if (L.z != 0u)
        s = tex.Load(int3(min(int2(uv * float2(tw, th)), int2(int(tw) - 1, int(th) - 1)), 0));
    else
        s = tex.SampleLevel(LinearS, uv, 0);
    if (L.y == 1u) s = float4(s.rrr, 1.0);  // greyscale AOV

    bool bad = any(isnan(s)) || any(isinf(s));
    bool neg = any(s.rgb < 0.0);
    uint mode = L.x;
    uint ch = modes.y;
    float3 disp;

    if (mode <= 1u)  // 0 colour (OCIO), 1 raw
    {
        float4 c = bad ? float4(0, 0, 0, 1) : s;
        if (mode == 0u) c = isA ? OCIO_InputA(c) : OCIO_InputB(c);
        c.rgb *= misc.z;
        if (ch == 1u) c.rgb = c.rrr;
        else if (ch == 2u) c.rgb = c.ggg;
        else if (ch == 3u) c.rgb = c.bbb;
        else if (ch == 5u) c.rgb = dot(c.rgb, kLuma).xxx;
        if (misc.w != 1.0) c.rgb = sign(c.rgb) * pow(abs(c.rgb), misc.w);
        o.lin = c.rgb;
        if (ch == 4u)
            disp = saturate(s.aaa);  // alpha is data: shown as-is
        else if ((modes.z & FLAG_FALSE) != 0u)
            disp = Turbo((log2(max(dot(c.rgb, kLuma), 1e-6) / 0.18) + 6.0) / 12.0);  // +-6 stops around 18% grey
        else if (mode == 0u)
            disp = OCIO_Display(float4(c.rgb, 1.0)).rgb;
        else
            disp = saturate(c.rgb);
        o.colorMode = mode == 0u && ch != 4u;
    }
    else if (mode == 2u)  // depth: near/far normalised greyscale
    {
        float t = bad ? 0.0 : saturate((s.r - P.y) / max(P.z - P.y, 1e-20));
        if (X.x != 0u) t = 1.0 - t;
        disp = t.xxx;
    }
    else if (mode == 3u)  // normals: -1..1 -> 0..1
    {
        disp = bad ? float3(0, 0, 0) : saturate(s.rgb * 0.5 + 0.5);
    }
    else if (mode == 4u)  // motion vectors: scaled around mid grey
    {
        disp = bad ? float3(0.5, 0.5, 0.5) : saturate(float3(s.r * P.w * 0.5 + 0.5, s.g * P.w * 0.5 + 0.5, 0.5));
    }
    else  // 5: IDs / cryptomatte (rank pairs: id0, coverage0, id1, coverage1)
    {
        uint id0 = asuint(s.r);
        if (L.y == 1u)
        {
            disp = IdColor(id0);
            if (X.y != 0u) disp = id0 == X.z ? float3(1, 1, 1) : disp * 0.15;
        }
        else
        {
            uint id1 = asuint(s.b);
            disp = IdColor(id0) * saturate(s.g) + IdColor(id1) * saturate(s.a);
            if (X.y != 0u)
            {
                float m = (id0 == X.z ? s.g : 0.0) + (id1 == X.z ? s.a : 0.0);
                disp = saturate(m).xxx + disp * 0.15 * (1.0 - saturate(m));
            }
        }
    }

    if ((modes.z & FLAG_NAN) != 0u)
    {
        if (bad) disp = float3(1, 0, 1);                       // NaN / Inf: magenta
        else if (neg && mode <= 1u) disp = float3(0, 1, 1);   // negative: cyan
    }
    if ((modes.z & FLAG_MATTE) != 0u && mode <= 1u)
        disp = lerp(disp, float3(1, 0, 0), saturate(1.0 - s.a) * 0.5);
    o.disp = disp;
    return o;
}

float4 PSMain(float4 pos : SV_Position) : SV_Target
{
    float2 p = pos.xy;
    LayerOut A = EvalLayer(TexA, p, rectA, layerA, paramsA, extraA, true);
    LayerOut B = EvalLayer(TexB, p, rectB, layerB, paramsB, extraB, false);
    float3 bg = background.rgb;
    float3 aCol = A.valid ? lerp(bg, A.disp, paramsA.x) : bg;  // opacity fades towards the background
    float3 bCol = B.valid ? lerp(bg, B.disp, paramsB.x) : bg;
    uint mode = modes.x;
    float3 outc = bg;

    if (mode == 2u)  // wipe
    {
        float2 n = float2(cos(wipe.z), sin(wipe.z));
        float d = dot(p - wipe.xy, n);
        float t = wipe.w > 0.0 ? smoothstep(-wipe.w, wipe.w, d) : step(0.0, d);
        float wB = lerp(misc.x, 1.0, t);  // the mix (arc) handle dissolves B into A's side
        float3 left = A.valid ? aCol : bCol;
        float3 right = B.valid ? bCol : aCol;
        outc = lerp(left, right, wB);
    }
    else if (mode == 3u)  // stack: B over A
    {
        float3 under = A.valid ? aCol : bg;
        outc = B.valid ? lerp(under, B.disp, paramsB.x) : under;
    }
    else if (mode == 4u)  // difference, computed in scene linear when both layers are colour
    {
        if (A.valid && B.valid)
        {
            if (A.colorMode && B.colorMode)
                outc = OCIO_Display(float4(abs(A.lin - B.lin) * misc.y, 1.0)).rgb;
            else
                outc = saturate(abs(A.disp - B.disp) * misc.y);
        }
        else
        {
            outc = A.valid ? aCol : (B.valid ? bCol : bg);
        }
    }
    else if (mode == 1u)  // B
    {
        outc = B.valid ? bCol : (A.valid ? aCol : bg);
    }
    else  // A, side-by-side, over/under (layout puts them in different places)
    {
        outc = A.valid ? aCol : (B.valid ? bCol : bg);
    }

    // +-0.5 LSB of static noise hides banding on an 8-bit swap chain.
    float noise = frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
    return float4(outc + noise / 255.0, 1.0);
}
)HLSL";

}  // namespace exrc
