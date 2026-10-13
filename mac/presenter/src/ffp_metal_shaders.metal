/*
 * Metal presenter shaders (ffp_metal_presenter.m). video_fs matches the D3D11 presenter's pixel shader
 * (two-texture NV12 / P010 input): crop clamp, manual bilinear filter, YCbCr -> R'G'B' with Kr / Kb of
 * BT.601 / BT.709 / BT.2020, limited or full range. Pictures arrive upright and filtered by the engine.
 * No transfer function is applied: for PQ / HLG frames
 * the output is the BT.2100 signal and the CAMetalLayer's colorspace (ITU-R BT.2100 PQ / HLG, EDR) does the rest.
 * overlay_vs / overlay_fs: the caption quads, premultiplied on upload, blended ONE / ONE_MINUS_SRC_ALPHA by the
 * pipeline state.
 *
 * This file is part of FFmpeg-derived code and is licensed under the LGPL 2.1 or later.
 */

#include <metal_stdlib>
using namespace metal;

struct VideoParams {
    uint4 input;      /* luma storage w,h */
    uint4 crop;       /* x,y,w,h */
    float4 color;     /* Kr, Kb, full-range, - */
    float4 dest;      /* x,y,width,height (target pixels) */
};

struct OverlayParams {
    float4 dest;      /* x,y,width,height (target pixels) */
    float4 target;    /* width,height, hdr (0 sdr, 1 pq, 2 hlg), - */
};

vertex float4 video_vs(uint id [[vertex_id]])
{
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

static float fetchY(texture2d<float, access::read> texY, constant VideoParams &P, int2 p)
{
    p = clamp(p, int2(0, 0), int2(P.input.xy) - 1);
    return texY.read(uint2(p)).r;
}

static float2 fetchUV(texture2d<float, access::read> texUV, constant VideoParams &P, int2 p)
{
    int2 limit = int2((P.input.xy + 1u) / 2u) - 1;
    p = clamp(p, int2(0, 0), limit);
    return texUV.read(uint2(p)).rg;
}

static float sampleY(texture2d<float, access::read> texY, constant VideoParams &P, float2 s)
{
    float2 q = clamp(s - 0.5, float2(P.crop.xy), float2(P.crop.xy + P.crop.zw) - 1.0);
    int2 p = int2(floor(q));
    float2 f = fract(q);
    int2 hi = int2(P.crop.xy + P.crop.zw) - 1;
    float a = fetchY(texY, P, p), b = fetchY(texY, P, min(p + int2(1, 0), hi));
    float c = fetchY(texY, P, min(p + int2(0, 1), hi)), d = fetchY(texY, P, min(p + int2(1, 1), hi));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

static float2 sampleUV(texture2d<float, access::read> texUV, constant VideoParams &P, float2 s)
{
    int2 lo = int2(P.crop.xy / 2u);
    int2 hi = int2((P.crop.xy + P.crop.zw - 1u) / 2u);
    float2 q = clamp(s * 0.5 - 0.5, float2(lo), float2(hi));
    int2 p = int2(floor(q));
    float2 f = fract(q);
    float2 a = fetchUV(texUV, P, p), b = fetchUV(texUV, P, min(p + int2(1, 0), hi));
    float2 c = fetchUV(texUV, P, min(p + int2(0, 1), hi)), d = fetchUV(texUV, P, min(p + int2(1, 1), hi));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

fragment float4 video_fs(float4 pos [[position]],
                         constant VideoParams &P [[buffer(0)]],
                         texture2d<float, access::read> texY [[texture(0)]],
                         texture2d<float, access::read> texUV [[texture(1)]])
{
    float2 uv = (pos.xy - P.dest.xy) / P.dest.zw;
    if (any(uv < 0.0) || any(uv >= 1.0))
        return float4(0, 0, 0, 1);
    float2 s = float2(P.crop.xy) + uv * float2(P.crop.zw);
    float y = sampleY(texY, P, s);
    float2 chroma = sampleUV(texUV, P, s);
    float scale = P.color.z != 0.0 ? 1.0 : 255.0 / 219.0;
    float y0 = (y - (P.color.z != 0.0 ? 0.0 : 16.0 / 255.0)) * scale;
    float2 c = (chroma - 0.5) * (P.color.z != 0.0 ? 1.0 : 255.0 / 224.0);
    float kr = P.color.x, kb = P.color.y, kg = 1.0 - kr - kb;
    float r = y0 + 2.0 * (1.0 - kr) * c.y;
    float b = y0 + 2.0 * (1.0 - kb) * c.x;
    float g = (y0 - kr * r - kb * b) / kg;
    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}

struct OverlayOut {
    float4 pos [[position]];
    float2 uv;
};

vertex OverlayOut overlay_vs(uint id [[vertex_id]], constant OverlayParams &P [[buffer(0)]])
{
    float2 uv;
    if (id == 0u) uv = float2(0, 0); else if (id == 1u) uv = float2(1, 0);
    else if (id == 2u || id == 3u) uv = float2(0, 1);
    else if (id == 4u) uv = float2(1, 0); else uv = float2(1, 1);
    float2 p = P.dest.xy + uv * P.dest.zw;
    OverlayOut o;
    o.pos = float4(p.x / P.target.x * 2.0 - 1.0, 1.0 - p.y / P.target.y * 2.0, 0, 1);
    o.uv = uv;
    return o;
}

/* An HDR drawable holds the BT.2100 signal: the (SDR, BT.709) caption is put at the reference white of
 * ITU-R BT.2408 (203 cd/m2 for PQ, 75 % signal for HLG) in BT.2020 primaries, else a white caption would be
 * drawn at the PQ peak (10000 cd/m2). */
static float3 pq_oetf(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

static float3 hlg_oetf(float3 e)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 lo = sqrt(3.0 * e);
    float3 hi = a * log(max(12.0 * e - b, 1e-6)) + c;
    return select(hi, lo, e <= 1.0 / 12.0);
}

fragment float4 overlay_fs(OverlayOut in [[stage_in]], constant OverlayParams &P [[buffer(0)]],
                           texture2d<float> bitmap [[texture(0)]], sampler s [[sampler(0)]])
{
    float4 c = bitmap.sample(s, in.uv);
    if (P.target.z != 0.0 && c.a > 0.0) {
        const float3x3 bt709_to_bt2020 = float3x3(float3(0.6274, 0.0691, 0.0164),
                                                  float3(0.3293, 0.9195, 0.0880),
                                                  float3(0.0433, 0.0114, 0.8956));
        float3 lin = bt709_to_bt2020 * pow(c.rgb / c.a, 2.4);
        float3 sig = P.target.z == 1.0 ? pq_oetf(lin * 203.0) : hlg_oetf(lin * 0.26496256);
        c.rgb = sig * c.a;
    }
    return c;
}
