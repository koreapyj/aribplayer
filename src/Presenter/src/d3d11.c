/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * D3D11 presenter: a flip-model DXGI swap chain on a child window of the host view; every frame uses the video
 * processor, or a pixel shader where the video processor cannot take the frame (P010 without VP P010 input, forced
 * route). The decoder shares the presenter's ID3D11Device (an AVHWDeviceContext wraps it), so decoded
 * D3D11 frames are shown without any copy. System frames are uploaded once.
 */

#include <math.h>
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/avassert.h>
#include <libavutil/bprint.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include "presenter_core.h"

static const struct {
    const char *name;
    unsigned bits;
} pr_force_keys[] = {
    { "nv12_srv",    PR_FORCE_NV12_SRV },
    { "p010_srv",    PR_FORCE_P010_SRV },
    { "decoder_srv", PR_FORCE_NV12_SRV | PR_FORCE_P010_SRV },
    { "p010",        PR_FORCE_P010 },
    { "shader",      PR_FORCE_SHADER },
};

/* one side of a LUID: 1..8 hex digits up to the end of s or `stop` */
static int pr_parse_hex32(const char *s, char stop, uint32_t *out, const char **end)
{
    uint32_t v = 0;
    int n = 0;

    for (; *s && *s != stop; s++, n++) {
        int c = *s;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0 || n >= 8)
            return -1;
        v = v << 4 | d;
    }
    if (!n)
        return -1;
    *out = v;
    *end = s;
    return 0;
}

int pr_parse_luid(const char *s, PrConfig *cfg)
{
    uint32_t high, low;
    const char *end;

    if (!s || pr_parse_hex32(s, ':', &high, &end) < 0 || *end != ':' || pr_parse_hex32(end + 1, 0, &low, &end) < 0)
        return -1;
    cfg->adapter_set = 1;
    cfg->luid_high = high;
    cfg->luid_low = low;
    return 0;
}

int pr_parse_force(const char *s, unsigned *mask, char *err, size_t err_size)
{
    char *copy = av_strdup(s ? s : ""), *tok, *save = NULL;
    int ret = 0;

    if (!copy) {
        snprintf(err, err_size, "out of memory");
        return -1;
    }
    for (tok = av_strtok(copy, ",", &save); tok; tok = av_strtok(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        size_t i;

        while (*tok == ' ')
            tok++;
        if (!*tok)
            continue;
        if (!eq || strcmp(eq + 1, "0")) {
            snprintf(err, err_size, "d3d11 force caps: '%s' is not key=0", tok);
            ret = -1;
            break;
        }
        *eq = 0;
        for (i = 0; i < FF_ARRAY_ELEMS(pr_force_keys); i++)
            if (!strcmp(pr_force_keys[i].name, tok))
                break;
        if (i == FF_ARRAY_ELEMS(pr_force_keys)) {
            snprintf(err, err_size, "d3d11 force caps: unknown capability '%s' (nv12_srv, p010_srv, decoder_srv, p010, "
                     "shader)", tok);
            ret = -1;
            break;
        }
        *mask |= pr_force_keys[i].bits;
    }
    av_free(copy);
    return ret;
}

const char *pr_force_name(unsigned mask, int n)
{
    static const char *const names[] = { "nv12_srv=0", "p010_srv=0", "p010=0", "shader=0" };
    static const unsigned bits[] = { PR_FORCE_NV12_SRV, PR_FORCE_P010_SRV, PR_FORCE_P010, PR_FORCE_SHADER };
    size_t i;

    for (i = 0; i < FF_ARRAY_ELEMS(bits); i++)
        if ((mask & bits[i]) && n-- == 0)
            return names[i];
    return NULL;
}

void pr_force_names_json(AVBPrint *bp, unsigned mask)
{
    const char *name;
    int i;

    for (i = 0; (name = pr_force_name(mask, i)); i++)
        av_bprintf(bp, "%s\"%s\"", i ? "," : "", name);
}

#define PR_REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

typedef struct PrShader {
    ID3D11Device *device;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *constants;
    ID3D11RasterizerState *raster;
    ID3D11SamplerState *sampler;
} PrShader;

typedef struct PrShaderInput {
    /* Exactly one layout: rgb, or the plane arrays ay + auv (NV12 R8_UNORM / R8G8_UNORM; P010 R16_UNORM / R16G16_UNORM). */
    ID3D11ShaderResourceView *ay, *auv, *rgb;
    unsigned slice;
    int p010;
    unsigned storage_width, storage_height;
} PrShaderInput;

typedef struct PrShaderGeometry {
    unsigned target_width, target_height;
    /* Optional destination rectangle; if NULL, letterbox the visible size with the AVFrame SAR. */
    const RECT *destination;
} PrShaderGeometry;

typedef struct PrOverlayRenderer {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *constants;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;
    ID3D11SamplerState *sampler;
} PrOverlayRenderer;

typedef struct PrAcrylicRenderer {
    ID3D11VertexShader *vs;
    ID3D11PixelShader *downsample_ps;
    ID3D11PixelShader *blur_ps;
    ID3D11PixelShader *composite_ps;
    ID3D11Buffer *constants;
    ID3D11RasterizerState *raster;
    ID3D11RasterizerState *scissor_raster;
    ID3D11SamplerState *sampler;
} PrAcrylicRenderer;

typedef struct PrAcrylicCB {
    float target[4];             /* width, height, inverse width, inverse height */
    float source[4];             /* source width, height, inverse width, inverse height */
    float region[4];             /* client-pixel x, y, width, height */
    float tint[4];               /* RGB, opacity */
    float luminosity[4];         /* RGB, opacity */
    float params[4];             /* radius, noise enabled, blur axis x, blur axis y */
} PrAcrylicCB;

typedef struct PrAcrylicTimer {
    ID3D11Query *disjoint, *begin, *end;
    int pending;
} PrAcrylicTimer;

typedef struct PrFrameGpuTimer {
    ID3D11Query *disjoint, *begin, *end;
    int pending;
} PrFrameGpuTimer;

typedef struct PrOverlayTexture {
    ID3D11Texture2D *texture;
    ID3D11ShaderResourceView *srv;
    int x, y, width, height;
} PrOverlayTexture;

typedef struct PrOverlayCache {
    uint64_t generation;
    int valid, visible;
    int canvas_width, canvas_height;
    int rect_count;
    PrOverlayTexture *rects;
} PrOverlayCache;

typedef struct PrOverlayCB {
    float dest[4];
    float target[4];
} PrOverlayCB;

typedef struct PrShaderCB {
    unsigned input[4];   /* storage w,h, unused, unused */
    unsigned crop[4];    /* x,y,w,h */
    unsigned flags[4];   /* layout (0 plane arrays, 1 rgb), unused, array slice, unused */
    float color[4];      /* Kr, Kb, full-range, unused */
    float dest[4];       /* x,y,width,height */
} PrShaderCB;

static const char cs_vs_source[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "}\n";

static const char cs_ps_source[] =
    "Texture2DArray<float> arrayY : register(t0);\n"
    "Texture2DArray<float2> arrayUV : register(t1);\n"
    "Texture2DArray<float4> texRGB : register(t2);\n"
    "SamplerState linearClamp : register(s0);\n"
    "cbuffer Parameters : register(b0) {\n"
    " uint4 input; uint4 crop; uint4 flags; float4 color; float4 dest;\n"
    "};\n"
    "float2 fetchUV(int2 p) {\n"
    " p = clamp(p, int2(0,0), int2((input.xy + 1u) / 2u) - 1);\n"
    " return arrayUV.Load(int4(p,flags.z,0));\n"
    "}\n"
    "float fetchY(int2 p) {\n"
    " p = clamp(p, int2(0,0), int2(input.xy)-1);\n"
    " return arrayY.Load(int4(p,flags.z,0));\n"
    "}\n"
    "float sampleY(float2 s) {\n"
    " float2 q = clamp(s - 0.5, float2(crop.xy), float2(crop.xy+crop.zw)-1.0);\n"
    " int2 p = int2(floor(q)); float2 f = frac(q);\n"
    " int2 hi = int2(crop.xy+crop.zw)-1;\n"
    " float a=fetchY(p), b=fetchY(min(p+int2(1,0),hi));\n"
    " float c=fetchY(min(p+int2(0,1),hi)), d=fetchY(min(p+int2(1,1),hi));\n"
    " return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);\n"
    "}\n"
    "float2 sampleUV(float2 s) {\n"
    " int2 lo = int2(crop.xy/2u);\n"
    " int2 hi = int2((crop.xy+crop.zw-1u)/2u);\n"
    " float2 q = clamp(s*0.5-0.5, float2(lo), float2(hi));\n"
    " int2 p=int2(floor(q)); float2 f=frac(q);\n"
    " float2 a=fetchUV(p), b=fetchUV(min(p+int2(1,0),hi));\n"
    " float2 c=fetchUV(min(p+int2(0,1),hi)), d=fetchUV(min(p+int2(1,1),hi));\n"
    " return lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);\n"
    "}\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    " float2 uv=(pos.xy-dest.xy)/dest.zw;\n"
    " if (any(uv < 0.0) || any(uv >= 1.0)) return float4(0,0,0,1);\n"
    " float2 s=float2(crop.xy)+uv*float2(crop.zw);\n"
    " if (flags.x == 1u)\n"
    "  return float4(texRGB.SampleLevel(linearClamp,float3(s/input.xy,0),0).rgb,1.0);\n"
    " float y=sampleY(s); float2 chroma=sampleUV(s);\n"
    " float scale = color.z != 0.0 ? 1.0 : 255.0/219.0;\n"
    " float y0=(y-(color.z != 0.0 ? 0.0 : 16.0/255.0))*scale;\n"
    " float2 c=(chroma-0.5)*(color.z != 0.0 ? 1.0 : 255.0/224.0);\n"
    " float kr=color.x, kb=color.y, kg=1.0-kr-kb;\n"
    " float r=y0+2.0*(1.0-kr)*c.y;\n"
    " float b=y0+2.0*(1.0-kb)*c.x;\n"
    " float g=(y0-kr*r-kb*b)/kg;\n"
    " return float4(saturate(r),saturate(g),saturate(b),1.0);\n"
    "}\n";

static void cs_error(char *err, size_t n, const char *what, HRESULT hr)
{
    if (err && n)
        snprintf(err, n, "%s (0x%08lx)", what, (unsigned long)hr);
}

static void pr_shader_free(PrShader *s)
{
    if (!s) return;
    PR_REL(s->sampler);
    PR_REL(s->raster);
    PR_REL(s->constants);
    PR_REL(s->ps);
    PR_REL(s->vs);
    PR_REL(s->device);
    HeapFree(GetProcessHeap(), 0, s);
}

typedef HRESULT (WINAPI *CSCompileFn)(LPCVOID, SIZE_T, LPCSTR,
                                      const D3D_SHADER_MACRO *, ID3DInclude *,
                                      LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static HRESULT cs_compile(CSCompileFn fn, const char *source, const char *profile,
                          ID3DBlob **blob, char *err, size_t n)
{
    ID3DBlob *messages = NULL;
    HRESULT hr = fn(source, strlen(source), "pr_shader", NULL, NULL,
                    "main", profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, blob, &messages);
    if (FAILED(hr) && err && n) {
        if (messages)
            snprintf(err, n, "D3DCompile %s: %.*s", profile,
                     (int)(messages->lpVtbl->GetBufferSize(messages) < 500 ?
                           messages->lpVtbl->GetBufferSize(messages) : 500),
                     (const char *)messages->lpVtbl->GetBufferPointer(messages));
        else cs_error(err, n, "D3DCompile", hr);
    }
    PR_REL(messages);
    return hr;
}

static const char pr_overlay_vs_source[] =
    "cbuffer Overlay : register(b0) { float4 dest; float4 target; };\n"
    "struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VOut main(uint id : SV_VertexID) {\n"
    " float2 uv;\n"
    " if (id == 0u) uv=float2(0,0); else if (id == 1u) uv=float2(1,0);\n"
    " else if (id == 2u || id == 3u) uv=float2(0,1);\n"
    " else if (id == 4u) uv=float2(1,0); else uv=float2(1,1);\n"
    " float2 p=dest.xy+uv*dest.zw;\n"
    " VOut o; o.pos=float4(p.x/target.x*2.0-1.0,1.0-p.y/target.y*2.0,0,1); o.uv=uv; return o;\n"
    "}\n";
static const char pr_overlay_ps_source[] =
    "Texture2D<float4> bitmap : register(t0); SamplerState linearClamp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    " return bitmap.Sample(linearClamp,uv);\n"
    "}\n";

static const char pr_acrylic_vs_source[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    " float2 p=float2((id<<1)&2,id&2);\n"
    " return float4(p*float2(2,-2)+float2(-1,1),0,1);\n"
    "}\n";

static const char pr_acrylic_downsample_ps_source[] =
    "Texture2D<float4> image : register(t0); SamplerState linearClamp : register(s0);\n"
    "cbuffer Acrylic : register(b0) { float4 target; float4 source; float4 region; float4 tint; float4 luminosity; float4 params; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target { return image.Sample(linearClamp,pos.xy*target.zw); }\n";

static const char pr_acrylic_blur_ps_source[] =
    "Texture2D<float4> image : register(t0); SamplerState linearClamp : register(s0);\n"
    "cbuffer Acrylic : register(b0) { float4 target; float4 source; float4 region; float4 tint; float4 luminosity; float4 params; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    " float2 uv=pos.xy*target.zw; float2 d=source.zw*params.zw;\n"
    " float4 c=image.Sample(linearClamp,uv)*0.13460;\n"
    " c+=(image.Sample(linearClamp,uv+d*1.0)+image.Sample(linearClamp,uv-d*1.0))*0.12730;\n"
    " c+=(image.Sample(linearClamp,uv+d*2.0)+image.Sample(linearClamp,uv-d*2.0))*0.10780;\n"
    " c+=(image.Sample(linearClamp,uv+d*3.0)+image.Sample(linearClamp,uv-d*3.0))*0.08160;\n"
    " c+=(image.Sample(linearClamp,uv+d*4.0)+image.Sample(linearClamp,uv-d*4.0))*0.05530;\n"
    " c+=(image.Sample(linearClamp,uv+d*5.0)+image.Sample(linearClamp,uv-d*5.0))*0.03360;\n"
    " c+=(image.Sample(linearClamp,uv+d*6.0)+image.Sample(linearClamp,uv-d*6.0))*0.01820;\n"
    " c+=(image.Sample(linearClamp,uv+d*7.0)+image.Sample(linearClamp,uv-d*7.0))*0.00885;\n"
    " return c;\n"
    "}\n";

static const char pr_acrylic_composite_ps_source[] =
    "Texture2D<float4> image : register(t0); Texture2D<float4> noiseTex : register(t1);\n"
    "SamplerState linearClamp : register(s0);\n"
    "cbuffer Acrylic : register(b0) { float4 target; float4 source; float4 region; float4 tint; float4 luminosity; float4 params; };\n"
    "float lum(float3 c) { return dot(c,float3(0.3,0.59,0.11)); }\n"
    "float3 setLum(float3 c,float l) {\n"
    " float d=l-lum(c); c+=d; float n=min(c.r,min(c.g,c.b)); float x=max(c.r,max(c.g,c.b));\n"
    " if(n<0.0) c=l+((c-l)*l)/(l-n);\n"
    " if(x>1.0) c=l+((c-l)*(1.0-l))/(x-l);\n"
    " return saturate(c);\n"
    "}\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    " float2 halfSize=region.zw*0.5; float radius=clamp(params.x,0.0,min(halfSize.x,halfSize.y));\n"
    " float2 q=abs(pos.xy-(region.xy+halfSize))-(halfSize-radius);\n"
    " float distance=length(max(q,0.0))+min(max(q.x,q.y),0.0)-radius; if(distance>0.0) discard;\n"
    " float3 c=image.Sample(linearClamp,pos.xy*target.zw).rgb;\n"
    " c=lerp(c,setLum(c,lum(luminosity.rgb)),saturate(luminosity.a));\n"
    " c=lerp(c,tint.rgb,saturate(tint.a));\n"
    " if(params.y>0.5) { float n=noiseTex.Sample(linearClamp,frac(pos.xy/256.0)).r-0.5; c=saturate(c+n*0.04); }\n"
    " return float4(c,1.0);\n"
    "}\n";

static void pr_overlay_renderer_free(PrOverlayRenderer *r)
{
    if (!r) return;
    PR_REL(r->sampler);
    PR_REL(r->raster);
    PR_REL(r->blend);
    PR_REL(r->constants);
    PR_REL(r->ps);
    PR_REL(r->vs);
    memset(r, 0, sizeof(*r));
}

static int pr_overlay_renderer_create(ID3D11Device *dev, PrOverlayRenderer *r,
                                       char *err, size_t n)
{
    static const WCHAR *dlls[] = { L"d3dcompiler_47.dll", L"d3dcompiler_43.dll" };
    HMODULE module = NULL;
    CSCompileFn compile = NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC blend;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC sm;
    D3D11_RENDER_TARGET_BLEND_DESC *bt;
    HRESULT hr;
    unsigned i;

    for (i = 0; i < sizeof(dlls)/sizeof(dlls[0]); ++i) {
        module = LoadLibraryW(dlls[i]);
        if (module) {
            compile = (CSCompileFn)(void *)GetProcAddress(module, "D3DCompile");
            if (compile) break;
            FreeLibrary(module);
            module = NULL;
        }
    }
    if (!compile) { cs_error(err, n, "D3DCompile unavailable for bitmap overlays", E_FAIL); goto fail; }
    if (FAILED(cs_compile(compile, pr_overlay_vs_source, "vs_5_0", &vsb, err, n)) ||
        FAILED(cs_compile(compile, pr_overlay_ps_source, "ps_5_0", &psb, err, n))) goto fail;
    hr = dev->lpVtbl->CreateVertexShader(dev, vsb->lpVtbl->GetBufferPointer(vsb),
                                          vsb->lpVtbl->GetBufferSize(vsb), NULL, &r->vs);
    if (FAILED(hr)) { cs_error(err, n, "CreateVertexShader(overlay)", hr); goto fail; }
    hr = dev->lpVtbl->CreatePixelShader(dev, psb->lpVtbl->GetBufferPointer(psb),
                                         psb->lpVtbl->GetBufferSize(psb), NULL, &r->ps);
    if (FAILED(hr)) { cs_error(err, n, "CreatePixelShader(overlay)", hr); goto fail; }
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(PrOverlayCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = dev->lpVtbl->CreateBuffer(dev, &bd, NULL, &r->constants);
    if (FAILED(hr)) { cs_error(err, n, "CreateBuffer(overlay constants)", hr); goto fail; }
    memset(&blend, 0, sizeof(blend));
    bt = &blend.RenderTarget[0];
    bt->BlendEnable = TRUE;
    bt->SrcBlend = D3D11_BLEND_ONE; /* overlay textures are premultiplied on upload */
    bt->DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bt->BlendOp = D3D11_BLEND_OP_ADD;
    bt->SrcBlendAlpha = D3D11_BLEND_ONE;
    bt->DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bt->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bt->RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = dev->lpVtbl->CreateBlendState(dev, &blend, &r->blend);
    if (FAILED(hr)) { cs_error(err, n, "CreateBlendState(overlay)", hr); goto fail; }
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = dev->lpVtbl->CreateRasterizerState(dev, &rd, &r->raster);
    if (FAILED(hr)) { cs_error(err, n, "CreateRasterizerState(overlay)", hr); goto fail; }
    memset(&sm, 0, sizeof(sm));
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dev->lpVtbl->CreateSamplerState(dev, &sm, &r->sampler);
    if (FAILED(hr)) { cs_error(err, n, "CreateSamplerState(overlay)", hr); goto fail; }
    PR_REL(vsb); PR_REL(psb);
    FreeLibrary(module);
    return 0;
fail:
    PR_REL(vsb); PR_REL(psb);
    if (module) FreeLibrary(module);
    pr_overlay_renderer_free(r);
    return -1;
}

static void pr_acrylic_renderer_free(PrAcrylicRenderer *r)
{
    if (!r) return;
    PR_REL(r->sampler);
    PR_REL(r->scissor_raster);
    PR_REL(r->raster);
    PR_REL(r->constants);
    PR_REL(r->composite_ps);
    PR_REL(r->blur_ps);
    PR_REL(r->downsample_ps);
    PR_REL(r->vs);
    memset(r, 0, sizeof(*r));
}

static int pr_acrylic_renderer_create(ID3D11Device *dev, PrAcrylicRenderer *r,
                                       char *err, size_t n)
{
    static const WCHAR *dlls[] = { L"d3dcompiler_47.dll", L"d3dcompiler_43.dll" };
    HMODULE module = NULL;
    CSCompileFn compile = NULL;
    ID3DBlob *vsb = NULL, *dsb = NULL, *bsb = NULL, *csb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC sm;
    HRESULT hr;
    unsigned i;

    for (i = 0; i < sizeof(dlls)/sizeof(dlls[0]); ++i) {
        module = LoadLibraryW(dlls[i]);
        if (module) {
            compile = (CSCompileFn)(void *)GetProcAddress(module, "D3DCompile");
            if (compile) break;
            FreeLibrary(module);
            module = NULL;
        }
    }
    if (!compile) { cs_error(err, n, "D3DCompile unavailable for Acrylic", E_FAIL); goto fail; }
    if (FAILED(cs_compile(compile, pr_acrylic_vs_source, "vs_5_0", &vsb, err, n)) ||
        FAILED(cs_compile(compile, pr_acrylic_downsample_ps_source, "ps_5_0", &dsb, err, n)) ||
        FAILED(cs_compile(compile, pr_acrylic_blur_ps_source, "ps_5_0", &bsb, err, n)) ||
        FAILED(cs_compile(compile, pr_acrylic_composite_ps_source, "ps_5_0", &csb, err, n)))
        goto fail;
    hr = dev->lpVtbl->CreateVertexShader(dev, vsb->lpVtbl->GetBufferPointer(vsb),
                                          vsb->lpVtbl->GetBufferSize(vsb), NULL, &r->vs);
    if (FAILED(hr)) { cs_error(err, n, "CreateVertexShader(Acrylic)", hr); goto fail; }
    hr = dev->lpVtbl->CreatePixelShader(dev, dsb->lpVtbl->GetBufferPointer(dsb),
                                         dsb->lpVtbl->GetBufferSize(dsb), NULL, &r->downsample_ps);
    if (FAILED(hr)) { cs_error(err, n, "CreatePixelShader(Acrylic downsample)", hr); goto fail; }
    hr = dev->lpVtbl->CreatePixelShader(dev, bsb->lpVtbl->GetBufferPointer(bsb),
                                         bsb->lpVtbl->GetBufferSize(bsb), NULL, &r->blur_ps);
    if (FAILED(hr)) { cs_error(err, n, "CreatePixelShader(Acrylic blur)", hr); goto fail; }
    hr = dev->lpVtbl->CreatePixelShader(dev, csb->lpVtbl->GetBufferPointer(csb),
                                         csb->lpVtbl->GetBufferSize(csb), NULL, &r->composite_ps);
    if (FAILED(hr)) { cs_error(err, n, "CreatePixelShader(Acrylic composite)", hr); goto fail; }
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(PrAcrylicCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = dev->lpVtbl->CreateBuffer(dev, &bd, NULL, &r->constants);
    if (FAILED(hr)) { cs_error(err, n, "CreateBuffer(Acrylic constants)", hr); goto fail; }
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = dev->lpVtbl->CreateRasterizerState(dev, &rd, &r->raster);
    if (FAILED(hr)) { cs_error(err, n, "CreateRasterizerState(Acrylic)", hr); goto fail; }
    rd.ScissorEnable = TRUE;
    hr = dev->lpVtbl->CreateRasterizerState(dev, &rd, &r->scissor_raster);
    if (FAILED(hr)) { cs_error(err, n, "CreateRasterizerState(Acrylic scissor)", hr); goto fail; }
    memset(&sm, 0, sizeof(sm));
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dev->lpVtbl->CreateSamplerState(dev, &sm, &r->sampler);
    if (FAILED(hr)) { cs_error(err, n, "CreateSamplerState(Acrylic)", hr); goto fail; }
    PR_REL(vsb); PR_REL(dsb); PR_REL(bsb); PR_REL(csb);
    FreeLibrary(module);
    return 0;
fail:
    PR_REL(vsb); PR_REL(dsb); PR_REL(bsb); PR_REL(csb);
    if (module) FreeLibrary(module);
    pr_acrylic_renderer_free(r);
    return -1;
}

/* Caller owns returned handle; no import library dependency on d3dcompiler.
 * Not thread-safe against concurrent use of the same immediate context. */
static PrShader *pr_shader_create(ID3D11Device *dev, char *err, size_t n)
{
    static const WCHAR *dlls[] = { L"d3dcompiler_47.dll", L"d3dcompiler_43.dll" };
    HMODULE module = NULL;
    CSCompileFn compile = NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC sm;
    PrShader *s = NULL;
    HRESULT hr;
    unsigned i;

    if (!dev) { cs_error(err, n, "null D3D11 device", E_INVALIDARG); return NULL; }
    for (i = 0; i < sizeof(dlls)/sizeof(dlls[0]); ++i) {
        module = LoadLibraryW(dlls[i]);
        if (module) {
            compile = (CSCompileFn)(void *)GetProcAddress(module, "D3DCompile");
            if (compile) break;
            FreeLibrary(module);
            module = NULL;
        }
    }
    if (!compile) { cs_error(err, n, "D3DCompile unavailable", E_FAIL); return NULL; }
    s = (PrShader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s) { cs_error(err,n,"out of memory",E_OUTOFMEMORY); goto fail; }
    s->device = dev;
    dev->lpVtbl->AddRef(dev);
    if (FAILED(cs_compile(compile, cs_vs_source, "vs_5_0", &vsb, err, n)) ||
        FAILED(cs_compile(compile, cs_ps_source, "ps_5_0", &psb, err, n))) goto fail;
    hr = dev->lpVtbl->CreateVertexShader(dev, vsb->lpVtbl->GetBufferPointer(vsb),
                                          vsb->lpVtbl->GetBufferSize(vsb), NULL, &s->vs);
    if (FAILED(hr)) { cs_error(err,n,"CreateVertexShader",hr); goto fail; }
    hr = dev->lpVtbl->CreatePixelShader(dev, psb->lpVtbl->GetBufferPointer(psb),
                                         psb->lpVtbl->GetBufferSize(psb), NULL, &s->ps);
    if (FAILED(hr)) { cs_error(err,n,"CreatePixelShader",hr); goto fail; }
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(PrShaderCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = dev->lpVtbl->CreateBuffer(dev, &bd, NULL, &s->constants);
    if (FAILED(hr)) { cs_error(err,n,"CreateBuffer(constants)",hr); goto fail; }
    memset(&rd, 0, sizeof(rd));
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = dev->lpVtbl->CreateRasterizerState(dev, &rd, &s->raster);
    if (FAILED(hr)) { cs_error(err,n,"CreateRasterizerState",hr); goto fail; }
    memset(&sm, 0, sizeof(sm));
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    hr = dev->lpVtbl->CreateSamplerState(dev, &sm, &s->sampler);
    if (FAILED(hr)) { cs_error(err,n,"CreateSamplerState",hr); goto fail; }
    PR_REL(vsb); PR_REL(psb); FreeLibrary(module);
    return s;
fail:
    PR_REL(vsb); PR_REL(psb);
    if (module) FreeLibrary(module);
    pr_shader_free(s);
    return NULL;
}

/* Check physical plane size too: a matching view format does not prove that
 * an SRV has enough texels, and Texture2D.Load outside its range returns zero. */
static int cs_plane_extent(ID3D11ShaderResourceView *srv, unsigned width, unsigned height)
{
    static const GUID iid_texture2d = { 0x6f15aaf2, 0xd208, 0x4e89,
                                         { 0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c } };
    ID3D11Resource *resource = NULL;
    ID3D11Texture2D *texture = NULL;
    D3D11_TEXTURE2D_DESC desc;
    int ok = 0;
    srv->lpVtbl->GetResource(srv, &resource);
    if (resource && SUCCEEDED(resource->lpVtbl->QueryInterface(resource,
                               &iid_texture2d, (void **)&texture))) {
        texture->lpVtbl->GetDesc(texture, &desc);
        ok = desc.Width >= width && desc.Height >= height &&
             desc.SampleDesc.Count == 1;
    }
    PR_REL(texture);
    PR_REL(resource);
    return ok;
}

static void cs_letterbox(RECT *dst, unsigned tw, unsigned th, unsigned w, unsigned h, const AVFrame *frame)
{
    double ar = (double)w / h;
    double dw, dh;
    int sar_num = frame->sample_aspect_ratio.num;
    int sar_den = frame->sample_aspect_ratio.den;
    if (sar_num > 0 && sar_den > 0)
        ar *= (double)sar_num / sar_den;
    dw = (double)th * ar;
    dh = th;
    if (dw > tw) { dw = tw; dh = dw / ar; }
    /* rounded to even pixel extents before centering */
    dst->right = (LONG)fmax(1.0, floor(dw + 0.5) - (double)((unsigned)floor(dw + 0.5) & 1u));
    dst->bottom = (LONG)fmax(1.0, floor(dh + 0.5) - (double)((unsigned)floor(dh + 0.5) & 1u));
    dst->left = ((LONG)tw - dst->right) / 2;
    dst->top = ((LONG)th - dst->bottom) / 2;
    dst->right += dst->left;
    dst->bottom += dst->top;
}

/* 0 on success, -1 with err filled. RTV must be BGRA8 UNORM, single sample,
 * same device, and cover geometry.target_width x target_height. The caller
 * serializes all use of ctx (and any FFmpeg hwctx lock) and owns presentation.
 * This function intentionally changes IA/VS/PS/RS/OM state: caller must restore
 * its own pipeline state if it uses the same context for other drawing.
 */
static int pr_shader_render(PrShader *s, ID3D11DeviceContext *ctx,
                           ID3D11RenderTargetView *rtv, const PrShaderInput *in,
                           const AVFrame *frame, const PrShaderGeometry *geom, char *err, size_t n)
{
    PrShaderCB cb;
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *views[3] = { 0 };
    ID3D11ShaderResourceView *nullviews[3] = { 0 };
    ID3D11Buffer *nullcb = NULL;
    ID3D11RenderTargetView *nullrtv = NULL;
    RECT dst;
    unsigned x, y, w, h;
    double kr, kb;
    static const float black[4] = { 0, 0, 0, 1 };

    if (!s || !ctx || !rtv || !in || !frame || !geom ||
        !geom->target_width || !geom->target_height ||
        geom->target_width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        geom->target_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        !in->storage_width || !in->storage_height ||
        in->storage_width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        in->storage_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        ((!!in->rgb + !!in->ay) != 1) || (in->ay && !in->auv) ||
        frame->width <= 0 || frame->height <= 0 ||
        frame->crop_left >= (size_t)frame->width || frame->crop_top >= (size_t)frame->height) {
        cs_error(err,n,"invalid shader arguments",E_INVALIDARG); return -1;
    }
    /* The shader produces SDR BGRA. PQ/HLG and constant-luminance BT.2020
     * require a transfer/tone mapping or nonlinear conversion not provided here. */
    if (frame->color_trc == AVCOL_TRC_SMPTE2084 ||
        frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        frame->colorspace == AVCOL_SPC_BT2020_CL) {
        cs_error(err,n,"HDR/BT.2020-CL conversion not implemented",E_INVALIDARG);
        return -1;
    }
    x = (unsigned)frame->crop_left; y = (unsigned)frame->crop_top;
    w = (unsigned)frame->width - x; h = (unsigned)frame->height - y;
    if (x >= in->storage_width || y >= in->storage_height ||
        w > in->storage_width-x || h > in->storage_height-y) {
        cs_error(err,n,"visible rectangle outside source",E_INVALIDARG); return -1;
    }
    if (in->ay) {
        D3D11_SHADER_RESOURCE_VIEW_DESC yd, uvd;
        in->ay->lpVtbl->GetDesc(in->ay, &yd);
        in->auv->lpVtbl->GetDesc(in->auv, &uvd);
        if (yd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY || uvd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DARRAY ||
            yd.Format != (in->p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM) ||
            uvd.Format != (in->p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM) ||
            !cs_plane_extent(in->ay, in->storage_width, in->storage_height) ||
            !cs_plane_extent(in->auv, (in->storage_width + 1u) / 2u, (in->storage_height + 1u) / 2u)) {
            cs_error(err,n,"invalid texture plane SRVs",E_INVALIDARG); return -1;
        }
    }
    if (geom->destination) dst = *geom->destination;
    else cs_letterbox(&dst, geom->target_width, geom->target_height, w, h, frame);
    if (dst.left < 0 || dst.top < 0 || dst.right <= dst.left || dst.bottom <= dst.top ||
        dst.right > (LONG)geom->target_width || dst.bottom > (LONG)geom->target_height) {
        cs_error(err,n,"destination outside target",E_INVALIDARG); return -1;
    }
    memset(&cb,0,sizeof(cb));
    cb.input[0]=in->storage_width; cb.input[1]=in->storage_height;
    cb.crop[0]=x; cb.crop[1]=y; cb.crop[2]=w; cb.crop[3]=h;
    cb.flags[0]=in->rgb ? 1 : 0;
    cb.flags[2]=in->slice;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: kr=0.299; kb=0.114; break;
    case AVCOL_SPC_BT2020_NCL: kr=0.2627; kb=0.0593; break;
    case AVCOL_SPC_BT709: kr=0.2126; kb=0.0722; break;
    default:
        if (frame->height > 576) { kr=0.2126; kb=0.0722; }
        else { kr=0.299; kb=0.114; }
        break;
    }
    cb.color[0]=(float)kr; cb.color[1]=(float)kb;
    cb.color[2]=frame->color_range == AVCOL_RANGE_JPEG ? 1.0f : 0.0f;
    cb.dest[0]=(float)dst.left; cb.dest[1]=(float)dst.top;
    cb.dest[2]=(float)(dst.right-dst.left); cb.dest[3]=(float)(dst.bottom-dst.top);
    vp.TopLeftX=0; vp.TopLeftY=0;
    vp.Width=(FLOAT)geom->target_width; vp.Height=(FLOAT)geom->target_height;
    vp.MinDepth=0; vp.MaxDepth=1;
    views[0]=in->ay; views[1]=in->auv; views[2]=in->rgb;
    ctx->lpVtbl->UpdateSubresource(ctx,(ID3D11Resource *)s->constants,0,NULL,&cb,0,0);
    ctx->lpVtbl->RSSetState(ctx,s->raster);
    ctx->lpVtbl->RSSetViewports(ctx,1,&vp);
    ctx->lpVtbl->IASetInputLayout(ctx,NULL);
    ctx->lpVtbl->IASetPrimitiveTopology(ctx,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->lpVtbl->VSSetShader(ctx,s->vs,NULL,0);
    ctx->lpVtbl->GSSetShader(ctx,NULL,NULL,0);
    ctx->lpVtbl->HSSetShader(ctx,NULL,NULL,0);
    ctx->lpVtbl->DSSetShader(ctx,NULL,NULL,0);
    ctx->lpVtbl->OMSetBlendState(ctx,NULL,NULL,0xffffffffu);
    ctx->lpVtbl->OMSetDepthStencilState(ctx,NULL,0);
    ctx->lpVtbl->PSSetShader(ctx,s->ps,NULL,0);
    ctx->lpVtbl->PSSetConstantBuffers(ctx,0,1,&s->constants);
    ctx->lpVtbl->PSSetShaderResources(ctx,0,3,views);
    ctx->lpVtbl->PSSetSamplers(ctx,0,1,&s->sampler);
    ctx->lpVtbl->OMSetRenderTargets(ctx,1,&rtv,NULL);
    ctx->lpVtbl->ClearRenderTargetView(ctx,rtv,black);
    ctx->lpVtbl->Draw(ctx,3,0);
    ctx->lpVtbl->PSSetShaderResources(ctx,0,3,nullviews);
    ctx->lpVtbl->PSSetConstantBuffers(ctx,0,1,&nullcb);
    ctx->lpVtbl->OMSetRenderTargets(ctx,0,&nullrtv,NULL);
    return 0;
}

/* IIDs spelled out: no dxguid */
static const GUID pr_IID_IDXGIDevice         = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID pr_IID_IDXGIFactory2       = { 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };
static const GUID pr_IID_ID3D11Texture2D     = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID pr_IID_ID3D11VideoDevice   = { 0x10ec4d5b, 0x975a, 0x4689, { 0xb9, 0xe4, 0xd0, 0xaa, 0xc3, 0x0f, 0xe3, 0x33 } };
static const GUID pr_IID_ID3D11VideoContext  = { 0x61f21c45, 0x3c0e, 0x4a74, { 0x9c, 0xea, 0x67, 0x10, 0x0d, 0x9a, 0xd5, 0xe4 } };
static const GUID pr_IID_ID3D11VideoContext1 = { 0xa7f026da, 0xa5f8, 0x4487, { 0xa5, 0x64, 0x15, 0xe3, 0x43, 0x57, 0x65, 0x1e } };
static const GUID pr_IID_ID3D10Multithread   = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };

#include "d3d11_device.c"

#define PR_MAX_FALLBACKS 16

/* input views of the decoder pool: one per slice (texture array) or per texture (array of textures, AMD); a pool has up
 * to ~33 surfaces, the least recently used view makes room when the cache is full */
#define PR_VIEW_CACHE 64

typedef struct PrViewEntry {
    ID3D11Texture2D *tex;
    int slice;
    ID3D11VideoProcessorInputView *view;
    uint64_t used;
} PrViewEntry;

typedef struct PrVP {
    ID3D11VideoProcessorEnumerator *en;
    ID3D11VideoProcessor *vp;
    int in_w, in_h, out_w, out_h;
    PrViewEntry views[PR_VIEW_CACHE];
    int nb_views;
    uint64_t tick;
} PrVP;

typedef struct PrInput {
    ID3D11Texture2D *tex;
    int slice;
    int direct;
    int rgb;
    int vis_x, vis_y, vis_w, vis_h;
    int tex_w, tex_h;
} PrInput;

/* what the D3D11 VideoProcessor of an adapter can do (probed once per process and adapter with a real device) */
typedef struct PrLimits {
    int ok;
    char error[192];
    unsigned proc_caps;             /* D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS.ProcessorCaps, all indexes ORed */
    unsigned telecine_caps;         /* ITelecineCaps, ORed */
    int nv12, p010;
    int nv12_srv, p010_srv;         /* a decoder pool of that format takes DECODER|SHADER_RESOURCE (probed with the misc
                                       flags of a real pool) */
    unsigned forced;                /* PR_FORCE_* bits that were masked off in this copy */
} PrLimits;

static void pr_record_fallback(PrD3D11 *p, const char *name);

typedef struct PrVideoRect {
    int x, y, width, height;        /* letterboxed video area, back-buffer pixels */
} PrVideoRect;

struct PrD3D11 {
    HWND host, child;
    HANDLE wthread, wready;         /* window thread: owns the child window and pumps its messages */
    DWORD wtid;
    int wres, wcw, wch;
    int fail_mode;                  /* PR_FAIL_*, cleared when it fired */
    int class_registered;
    int shown;
    volatile LONG closing;          /* pr_d3d11_destroy runs: the window thread skips redraws */
    PrRedrawFn redraw;
    void *redraw_opaque;

    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VideoDevice *vdev;
    ID3D11VideoContext *vctx;
    ID3D11VideoContext1 *vctx1;
    IDXGIFactory2 *factory;
    AVBufferRef *hwdev;
    PrShader *shader;
    LONG frames_shader, frames_d3d11_copied;
    volatile LONG route;            /* 0 none yet, 1 video processor, 2 shader */
    AVD3D11VADeviceContext *hwctx;
    PrLimits lim;
    PrConfig cfg;
    PrAdapter adapter;              /* the adapter of dev */
    PrDebug *dbg;                   /* debug layer messages of dev (owned by hwdev), NULL = layer off */
    int forced_lost;                /* PR_FAIL_DEVICE_LOST fired */
    volatile LONG failed_hr;        /* HRESULT of the latest failed present / readback */
    CRITICAL_SECTION fb_cs;         /* the fallback list: read by the statistics while present holds cs */
    char fallbacks[PR_MAX_FALLBACKS][64];
    int nb_fallbacks;
    int shader_failed;
    AVFrame *held;                  /* the frame shown last, for redraws */
    AVFrame *dl;                    /* D3D11 frame downloaded to system memory (no route takes it as is) */

    IDXGISwapChain1 *sc;
    int sc_w, sc_h;

    PrVP disp, snap;
    PrOverlayRenderer overlay_renderer;
    int overlay_renderer_ready;
    PrOverlayCache overlay[FFP_OVERLAY_LAYER_COUNT];
    PrVideoRect overlay_video_rect;
    LONG64 overlay_cue_uploads, overlay_rects_uploaded;
    LONG64 overlay_cues_shown, overlay_rects_drawn, overlay_draw_failures;
    LONG64 overlay_readback_failures;
    LONG64 overlay_readback_seq;            /* successful captures (written under cs) */
    LONG64 overlay_readback_rects;          /* overlay rects drawn into the latest captured frame (under cs) */
    int overlay_frame_rects;                /* rects drawn by the current present call (under cs) */
    int overlay_readback_enabled;
    int overlay_readback_valid, overlay_readback_w, overlay_readback_h, overlay_readback_stride;
    PrVideoRect overlay_readback_rect;      /* the video rectangle of the captured frame */
    int overlay_readback_stage_w, overlay_readback_stage_h;
    size_t overlay_readback_capacity;
    ID3D11Texture2D *overlay_readback_staging;
    uint8_t *overlay_readback_data;

    PrAcrylicRenderer acrylic_renderer;
    int acrylic_renderer_ready, acrylic_renderer_unavailable;
    int acrylic_w, acrylic_h, acrylic_small_w, acrylic_small_h;
    ID3D11Texture2D *acrylic_source, *acrylic_small[2], *acrylic_noise;
    ID3D11ShaderResourceView *acrylic_source_srv, *acrylic_small_srv[2], *acrylic_noise_srv;
    ID3D11RenderTargetView *acrylic_small_rtv[2];
    PrBackdropRegion acrylic_regions[PR_BACKDROP_REGION_MAX];
    int acrylic_region_count;
    PrAcrylicTimer acrylic_timers[4];
    unsigned acrylic_timer_next;
    int acrylic_timer_unavailable;
    LONG64 acrylic_frames_blurred, acrylic_failures, acrylic_last_blur_us;
    PrFrameGpuTimer frame_gpu_timers[4];
    unsigned frame_gpu_timer_next;
    int frame_gpu_timer_unavailable;
    LONG64 frame_gpu_total_us, frame_gpu_count, frame_gpu_last_us;

    CRITICAL_SECTION sc_cs;         /* the swap chain: from its creation / resize to the end of Present (entered before cs) */
    CRITICAL_SECTION cs;            /* present / readback / redraw */
    AVBufferRef *up_frames;
    int up_fmt, up_w, up_h;
    AVFrame *up_frame;              /* last uploaded surface (kept alive until the next upload) */
    AVFrame *swf;                   /* system frame padded to even dimensions / converted to NV12 */
};

/* the HRESULT of the latest failed call of this thread: tells a device loss */
static __declspec(thread) HRESULT pr_last_hr;

static void pr_set_err(char *err, size_t size, const char *what, HRESULT hr)
{
    if (FAILED(hr))
        pr_last_hr = hr;
    if (err && size) {
        if (FAILED(hr))
            snprintf(err, size, "%s failed (0x%08lx)", what, (unsigned long)hr);
        else
            snprintf(err, size, "%s", what);
    }
}

#define PR_OVERLAY_MAX_RECTS 1024
#define PR_OVERLAY_MAX_BYTES (64u * 1024u * 1024u)

static void pr_overlay_cache_free(PrOverlayCache *c)
{
    int i;
    if (!c) return;
    for (i = 0; i < c->rect_count; i++) {
        PR_REL(c->rects[i].srv);
        PR_REL(c->rects[i].texture);
    }
    av_freep(&c->rects);
    c->rect_count = 0;
}

static int pr_overlay_cache_update(PrD3D11 *p, PrOverlayCache *cache,
                                    const FFPOverlayLayer *layer, char *err, size_t n)
{
    PrOverlayTexture *rects = NULL;
    uint64_t total_bytes = 0;
    int i;

    if (!layer || layer->rect_count < 0 || layer->rect_count > PR_OVERLAY_MAX_RECTS ||
        (layer->rect_count && (!layer->rects || layer->canvas_width <= 0 || layer->canvas_height <= 0 ||
         layer->canvas_width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
         layer->canvas_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))) {
        pr_set_err(err, n, "invalid bitmap overlay layer", E_INVALIDARG);
        return -1;
    }
    if (cache->valid && cache->generation == layer->generation &&
        cache->canvas_width == layer->canvas_width && cache->canvas_height == layer->canvas_height &&
        cache->rect_count == layer->rect_count) {
        cache->visible = !!layer->visible;
        return 0;
    }
    if (layer->rect_count) {
        rects = av_calloc(layer->rect_count, sizeof(*rects));
        if (!rects) {
            pr_set_err(err, n, "bitmap overlay allocation failed", E_OUTOFMEMORY);
            return -1;
        }
    }
    for (i = 0; i < layer->rect_count; i++) {
        const FFPOverlayRect *src = &layer->rects[i];
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA sd;
        uint8_t *pixels;
        int x, y;
        HRESULT hr;
        if (!src->rgba || src->width <= 0 || src->height <= 0 || src->stride <= 0 ||
            (int64_t)src->stride < (int64_t)src->width * 4 ||
            src->width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            src->height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            (uint64_t)src->width * (uint64_t)src->height * 4u > PR_OVERLAY_MAX_BYTES - total_bytes) {
            pr_set_err(err, n, "invalid RGBA bitmap overlay rect", E_INVALIDARG);
            goto fail;
        }
        total_bytes += (uint64_t)src->width * (uint64_t)src->height * 4u;
        memset(&td, 0, sizeof(td));
        td.Width = (UINT)src->width;
        td.Height = (UINT)src->height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        pixels = av_malloc((size_t)src->width * src->height * 4u);
        if (!pixels) {
            pr_set_err(err, n, "bitmap overlay pixel allocation failed", E_OUTOFMEMORY);
            goto fail;
        }
        for (y = 0; y < src->height; y++) {
            const uint8_t *in = src->rgba + (size_t)y * src->stride;
            uint8_t *out = pixels + (size_t)y * src->width * 4u;
            for (x = 0; x < src->width; x++) {
                unsigned a = in[x * 4 + 3];
                out[x * 4 + 0] = (uint8_t)((in[x * 4 + 0] * a + 127u) / 255u);
                out[x * 4 + 1] = (uint8_t)((in[x * 4 + 1] * a + 127u) / 255u);
                out[x * 4 + 2] = (uint8_t)((in[x * 4 + 2] * a + 127u) / 255u);
                out[x * 4 + 3] = (uint8_t)a;
            }
        }
        sd.pSysMem = pixels;
        sd.SysMemPitch = (UINT)src->width * 4u;
        sd.SysMemSlicePitch = 0;
        hr = p->dev->lpVtbl->CreateTexture2D(p->dev, &td, &sd, &rects[i].texture);
        av_free(pixels);
        if (FAILED(hr)) {
            pr_set_err(err, n, "CreateTexture2D(bitmap overlay)", hr);
            goto fail;
        }
        hr = p->dev->lpVtbl->CreateShaderResourceView(p->dev, (ID3D11Resource *)rects[i].texture,
                                                       NULL, &rects[i].srv);
        if (FAILED(hr)) {
            pr_set_err(err, n, "CreateShaderResourceView(bitmap overlay)", hr);
            goto fail;
        }
        rects[i].x = src->x;
        rects[i].y = src->y;
        rects[i].width = src->width;
        rects[i].height = src->height;
    }
    pr_overlay_cache_free(cache);
    cache->rects = rects;
    cache->rect_count = layer->rect_count;
    cache->canvas_width = layer->canvas_width;
    cache->canvas_height = layer->canvas_height;
    cache->generation = layer->generation;
    cache->visible = !!layer->visible;
    cache->valid = 1;
    InterlockedIncrement64(&p->overlay_cue_uploads);
    InterlockedExchangeAdd64(&p->overlay_rects_uploaded, layer->rect_count);
    return 0;
fail:
    if (rects) {
        for (i = 0; i < layer->rect_count; i++) {
            PR_REL(rects[i].srv);
            PR_REL(rects[i].texture);
        }
        av_free(rects);
    }
    return -1;
}

static int pr_overlay_draw(PrD3D11 *p, ID3D11RenderTargetView *rtv,
                            const RECT *video_rect, int tw, int th, char *err, size_t n)
{
    static const float blend_factor[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11Buffer *null_cb = NULL;
    D3D11_VIEWPORT vp;
    int l, i, active = 0;

    p->overlay_frame_rects = 0;
    for (l = 0; l < FFP_OVERLAY_LAYER_COUNT; l++)
        if (p->overlay[l].visible && p->overlay[l].rect_count &&
            p->overlay[l].canvas_width > 0 && p->overlay[l].canvas_height > 0)
            active = 1;
    if (!active)
        return 0;
    if (!p->overlay_renderer_ready) {
        if (pr_overlay_renderer_create(p->dev, &p->overlay_renderer, err, n) < 0)
            return -1;
        p->overlay_renderer_ready = 1;
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (FLOAT)tw; vp.Height = (FLOAT)th;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    p->ctx->lpVtbl->RSSetState(p->ctx, p->overlay_renderer.raster);
    p->ctx->lpVtbl->RSSetViewports(p->ctx, 1, &vp);
    p->ctx->lpVtbl->IASetInputLayout(p->ctx, NULL);
    p->ctx->lpVtbl->IASetPrimitiveTopology(p->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->ctx->lpVtbl->VSSetShader(p->ctx, p->overlay_renderer.vs, NULL, 0);
    p->ctx->lpVtbl->GSSetShader(p->ctx, NULL, NULL, 0);
    p->ctx->lpVtbl->HSSetShader(p->ctx, NULL, NULL, 0);
    p->ctx->lpVtbl->DSSetShader(p->ctx, NULL, NULL, 0);
    p->ctx->lpVtbl->PSSetShader(p->ctx, p->overlay_renderer.ps, NULL, 0);
    p->ctx->lpVtbl->PSSetSamplers(p->ctx, 0, 1, &p->overlay_renderer.sampler);
    /* dest/target are read by the vertex shader; without a VS binding the quads get NaN positions and are culled */
    p->ctx->lpVtbl->VSSetConstantBuffers(p->ctx, 0, 1, &p->overlay_renderer.constants);
    p->ctx->lpVtbl->PSSetConstantBuffers(p->ctx, 0, 1, &p->overlay_renderer.constants);
    p->ctx->lpVtbl->OMSetBlendState(p->ctx, p->overlay_renderer.blend, blend_factor, 0xffffffffu);
    p->ctx->lpVtbl->OMSetRenderTargets(p->ctx, 1, &rtv, NULL);
    for (l = 0; l < FFP_OVERLAY_LAYER_COUNT; l++) {
        PrOverlayCache *c = &p->overlay[l];
        if (!c->visible || !c->rect_count || c->canvas_width <= 0 || c->canvas_height <= 0)
            continue;
        for (i = 0; i < c->rect_count; i++) {
            PrOverlayTexture *r = &c->rects[i];
            PrOverlayCB cb;
            ID3D11ShaderResourceView *srv = r->srv;
            cb.dest[0] = (float)video_rect->left + (float)r->x * (float)(video_rect->right-video_rect->left) / c->canvas_width;
            cb.dest[1] = (float)video_rect->top + (float)r->y * (float)(video_rect->bottom-video_rect->top) / c->canvas_height;
            cb.dest[2] = (float)r->width * (float)(video_rect->right-video_rect->left) / c->canvas_width;
            cb.dest[3] = (float)r->height * (float)(video_rect->bottom-video_rect->top) / c->canvas_height;
            cb.target[0] = (float)tw; cb.target[1] = (float)th; cb.target[2] = cb.target[3] = 0;
            p->ctx->lpVtbl->UpdateSubresource(p->ctx, (ID3D11Resource *)p->overlay_renderer.constants, 0, NULL, &cb, 0, 0);
            p->ctx->lpVtbl->PSSetShaderResources(p->ctx, 0, 1, &srv);
            p->ctx->lpVtbl->Draw(p->ctx, 6, 0);
            InterlockedIncrement64(&p->overlay_rects_drawn);
            p->overlay_frame_rects++;
        }
        InterlockedIncrement64(&p->overlay_cues_shown);
    }
    p->ctx->lpVtbl->PSSetShaderResources(p->ctx, 0, 1, &null_srv);
    p->ctx->lpVtbl->VSSetConstantBuffers(p->ctx, 0, 1, &null_cb);
    p->ctx->lpVtbl->PSSetConstantBuffers(p->ctx, 0, 1, &null_cb);
    p->ctx->lpVtbl->OMSetRenderTargets(p->ctx, 0, NULL, NULL);
    return 0;
}

/* Test-only synchronous copy, called on the immediate-context owner while the
 * presenter lock is held. It is disabled in normal playback. */
static void pr_overlay_capture_readback(PrD3D11 *p, ID3D11Texture2D *bb, int w, int h)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE mapped;
    size_t bytes;
    int y;
    if (!p->overlay_readback_enabled || !bb)
        return;
    if (!p->overlay_readback_staging || p->overlay_readback_stage_w != w || p->overlay_readback_stage_h != h) {
        PR_REL(p->overlay_readback_staging);
        memset(&td, 0, sizeof(td));
        td.Width = (UINT)w; td.Height = (UINT)h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(p->dev->lpVtbl->CreateTexture2D(p->dev, &td, NULL, &p->overlay_readback_staging)))
            goto fail;
        p->overlay_readback_stage_w = w;
        p->overlay_readback_stage_h = h;
    }
    p->ctx->lpVtbl->CopyResource(p->ctx, (ID3D11Resource *)p->overlay_readback_staging, (ID3D11Resource *)bb);
    if (FAILED(p->ctx->lpVtbl->Map(p->ctx, (ID3D11Resource *)p->overlay_readback_staging, 0, D3D11_MAP_READ, 0, &mapped)))
        goto fail;
    bytes = (size_t)w * 4u * (size_t)h;
    if (bytes / 4u / (size_t)h != (size_t)w) {
        p->ctx->lpVtbl->Unmap(p->ctx, (ID3D11Resource *)p->overlay_readback_staging, 0);
        goto fail;
    }
    if (p->overlay_readback_capacity < bytes) {
        uint8_t *new_data = av_realloc(p->overlay_readback_data, bytes);
        if (!new_data) {
            p->ctx->lpVtbl->Unmap(p->ctx, (ID3D11Resource *)p->overlay_readback_staging, 0);
            goto fail;
        }
        p->overlay_readback_data = new_data;
        p->overlay_readback_capacity = bytes;
    }
    for (y = 0; y < h; y++)
        memcpy(p->overlay_readback_data + (size_t)y * w * 4,
               (const uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch, (size_t)w * 4);
    p->overlay_readback_stride = w * 4;
    p->ctx->lpVtbl->Unmap(p->ctx, (ID3D11Resource *)p->overlay_readback_staging, 0);
    p->overlay_readback_w = w;
    p->overlay_readback_h = h;
    p->overlay_readback_valid = 1;
    p->overlay_readback_rects = p->overlay_frame_rects;     /* seq and rects change together under cs */
    p->overlay_readback_rect = p->overlay_video_rect;
    p->overlay_readback_seq++;
    return;
fail:
    p->overlay_readback_valid = 0;
    InterlockedIncrement64(&p->overlay_readback_failures);
}

void pr_d3d11_readback_test_enable(PrD3D11 *p, int enable)
{
    if (!p) return;
    EnterCriticalSection(&p->cs);
    p->overlay_readback_enabled = !!enable;
    if (!enable) p->overlay_readback_valid = 0;
    LeaveCriticalSection(&p->cs);
}

int pr_d3d11_readback_last(PrD3D11 *p, uint8_t **bgra, int *w, int *h, int *stride, int *video_x, int *video_y,
                           int *video_width, int *video_height, char *err, size_t err_size)
{
    uint8_t *copy;
    if (!p || !bgra || !w || !h || !stride || !video_x || !video_y || !video_width || !video_height) {
        pr_set_err(err, err_size, "invalid D3D11 readback arguments", E_INVALIDARG);
        return -1;
    }
    *bgra = NULL;
    EnterCriticalSection(&p->cs);
    if (!p->overlay_readback_valid || !p->overlay_readback_data) {
        LeaveCriticalSection(&p->cs);
        pr_set_err(err, err_size, "no composed back-buffer readback available", E_FAIL);
        return -1;
    }
    copy = av_memdup(p->overlay_readback_data,
                     (size_t)p->overlay_readback_stride * p->overlay_readback_h);
    if (!copy) {
        LeaveCriticalSection(&p->cs);
        pr_set_err(err, err_size, "readback allocation failed", E_OUTOFMEMORY);
        return -1;
    }
    *bgra = copy;
    *w = p->overlay_readback_w;
    *h = p->overlay_readback_h;
    *stride = p->overlay_readback_stride;
    *video_x = p->overlay_readback_rect.x;
    *video_y = p->overlay_readback_rect.y;
    *video_width = p->overlay_readback_rect.width;
    *video_height = p->overlay_readback_rect.height;
    LeaveCriticalSection(&p->cs);
    return 0;
}

#define CHECK_HR(expr, what) do {                       \
    HRESULT hr_ = (expr);                               \
    if (FAILED(hr_)) {                                  \
        pr_set_err(err, err_size, what, hr_);          \
        goto fail;                                      \
    }                                                   \
} while (0)

static void pr_lock_dev(PrD3D11 *p)
{
    if (p->hwctx && p->hwctx->lock)
        p->hwctx->lock(p->hwctx->lock_ctx);
}

static void pr_unlock_dev(PrD3D11 *p)
{
    if (p->hwctx && p->hwctx->unlock)
        p->hwctx->unlock(p->hwctx->lock_ctx);
}

/* same rules as ffplay.c calculate_display_rect() */
static RECT pr_letterbox(int bw, int bh, int pw, int ph, AVRational sar)
{
    AVRational ar = sar;
    int64_t w, h, x, y;
    RECT r;

    if (av_cmp_q(ar, av_make_q(0, 1)) <= 0)
        ar = av_make_q(1, 1);
    ar = av_mul_q(ar, av_make_q(pw, ph));
    h = bh;
    w = av_rescale(h, ar.num, ar.den) & ~1;
    if (w > bw) {
        w = bw;
        h = av_rescale(w, ar.den, ar.num) & ~1;
    }
    w = FFMAX(w, 1);
    h = FFMAX(h, 1);
    x = (bw - w) / 2;
    y = (bh - h) / 2;
    r.left = (LONG)x;
    r.top = (LONG)y;
    r.right = (LONG)(x + w);
    r.bottom = (LONG)(y + h);
    return r;
}


static DXGI_COLOR_SPACE_TYPE pr_input_colorspace(const AVFrame *f, int rgb)
{
    int full = f->color_range == AVCOL_RANGE_JPEG;
    enum AVColorSpace cs = f->colorspace;

    if (rgb)
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P)
        full = 1;
    switch (cs) {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        if (f->color_trc == AVCOL_TRC_SMPTE2084)
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    case AVCOL_SPC_BT709:
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    default:
        /* Decoder tags untagged frames; retain this fallback for direct callers. */
        return f->height > 576 ?
            (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709) :
            (full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601);
    }
}


static void pr_vp_free_views(PrVP *v)
{
    int i;
    for (i = 0; i < v->nb_views; i++) {
        PR_REL(v->views[i].view);
        PR_REL(v->views[i].tex);
    }
    v->nb_views = 0;
}

static void pr_vp_free(PrVP *v)
{
    pr_vp_free_views(v);
    PR_REL(v->vp);
    PR_REL(v->en);
    v->in_w = v->in_h = v->out_w = v->out_h = 0;
}

static int pr_vp_ensure(ID3D11VideoDevice *vdev, PrVP *v, int iw, int ih, int ow, int oh, char *err, size_t err_size)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd;

    if (v->vp && v->in_w == iw && v->in_h == ih && v->out_w == ow && v->out_h == oh)
        return 0;
    pr_vp_free(v);
    memset(&cd, 0, sizeof(cd));
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = iw;
    cd.InputHeight = ih;
    cd.OutputWidth = ow;
    cd.OutputHeight = oh;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    CHECK_HR(vdev->lpVtbl->CreateVideoProcessorEnumerator(vdev, &cd, &v->en), "CreateVideoProcessorEnumerator");
    CHECK_HR(vdev->lpVtbl->CreateVideoProcessor(vdev, v->en, 0, &v->vp), "CreateVideoProcessor");
    v->in_w = iw;
    v->in_h = ih;
    v->out_w = ow;
    v->out_h = oh;
    return 0;
fail:
    pr_vp_free(v);
    return -1;
}

static ID3D11VideoProcessorInputView *pr_vp_input_view(ID3D11VideoDevice *vdev, PrVP *v, ID3D11Texture2D *tex,
                                                        int slice, char *err, size_t err_size)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d;
    ID3D11VideoProcessorInputView *view = NULL;
    int i;

    for (i = 0; i < v->nb_views; i++)
        if (v->views[i].tex == tex && v->views[i].slice == slice) {
            v->views[i].used = ++v->tick;
            return v->views[i].view;
        }
    memset(&d, 0, sizeof(d));
    d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    d.Texture2D.ArraySlice = slice;
    CHECK_HR(vdev->lpVtbl->CreateVideoProcessorInputView(vdev, (ID3D11Resource *)tex, v->en, &d, &view),
             "CreateVideoProcessorInputView");
    if (v->nb_views >= PR_VIEW_CACHE) {
        int lru = 0;

        for (i = 1; i < v->nb_views; i++)
            if (v->views[i].used < v->views[lru].used)
                lru = i;
        PR_REL(v->views[lru].view);
        PR_REL(v->views[lru].tex);
        v->views[lru] = v->views[--v->nb_views];
    }
    tex->lpVtbl->AddRef(tex);
    v->views[v->nb_views].tex = tex;
    v->views[v->nb_views].slice = slice;
    v->views[v->nb_views].view = view;
    v->views[v->nb_views].used = ++v->tick;
    v->nb_views++;
    return view;
fail:
    return NULL;
}

static void pr_vp_setup(ID3D11VideoContext *vc, ID3D11VideoContext1 *vc1, ID3D11VideoProcessor *vp,
                        DXGI_COLOR_SPACE_TYPE in_cs, const RECT *src, const RECT *dst, const RECT *target)
{
    D3D11_VIDEO_COLOR bg;

    memset(&bg, 0, sizeof(bg));
    bg.RGBA.A = 1.0f;
    vc->lpVtbl->VideoProcessorSetOutputBackgroundColor(vc, vp, FALSE, &bg);
    vc->lpVtbl->VideoProcessorSetOutputTargetRect(vc, vp, TRUE, target);
    vc->lpVtbl->VideoProcessorSetStreamFrameFormat(vc, vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vc->lpVtbl->VideoProcessorSetStreamAutoProcessingMode(vc, vp, 0, FALSE);
    vc->lpVtbl->VideoProcessorSetStreamSourceRect(vc, vp, 0, TRUE, src);
    vc->lpVtbl->VideoProcessorSetStreamDestRect(vc, vp, 0, TRUE, dst);
    if (vc1) {
        vc1->lpVtbl->VideoProcessorSetStreamColorSpace1(vc1, vp, 0, in_cs);
        vc1->lpVtbl->VideoProcessorSetOutputColorSpace1(vc1, vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs;
        memset(&cs, 0, sizeof(cs));
        if (in_cs == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
            cs.RGB_Range = 0;
        } else {
            cs.YCbCr_Matrix = (in_cs == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 ||
                               in_cs == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601) ? 0 : 1;
            cs.Nominal_Range = (in_cs == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 ||
                                in_cs == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 ||
                                in_cs == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020) ? 2 : 1;
        }
        vc->lpVtbl->VideoProcessorSetStreamColorSpace(vc, vp, 0, &cs);
        memset(&cs, 0, sizeof(cs));
        vc->lpVtbl->VideoProcessorSetOutputColorSpace(vc, vp, &cs);
    }
}

static ID3D11Texture2D *pr_make_tex(ID3D11Device *dev, int w, int h, DXGI_FORMAT fmt, D3D11_USAGE usage, UINT bind,
                                     UINT cpu, const void *data, UINT pitch)
{
    D3D11_TEXTURE2D_DESC t;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *tx = NULL;

    memset(&t, 0, sizeof(t));
    t.Width = w;
    t.Height = h;
    t.MipLevels = 1;
    t.ArraySize = 1;
    t.Format = fmt;
    t.SampleDesc.Count = 1;
    t.Usage = usage;
    t.BindFlags = bind;
    t.CPUAccessFlags = cpu;
    sd.pSysMem = data;
    sd.SysMemPitch = pitch;
    sd.SysMemSlicePitch = 0;
    if (FAILED(dev->lpVtbl->CreateTexture2D(dev, &t, data ? &sd : NULL, &tx)))
        return NULL;
    return tx;
}

static void pr_acrylic_surface_free(PrD3D11 *p)
{
    int i;
    PR_REL(p->acrylic_source_srv);
    PR_REL(p->acrylic_source);
    for (i = 0; i < 2; i++) {
        PR_REL(p->acrylic_small_srv[i]);
        PR_REL(p->acrylic_small_rtv[i]);
        PR_REL(p->acrylic_small[i]);
    }
    p->acrylic_w = p->acrylic_h = p->acrylic_small_w = p->acrylic_small_h = 0;
}

static int pr_acrylic_surface_ensure(PrD3D11 *p, int w, int h, char *err, size_t n)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    uint8_t *noise = NULL;
    int i;
    HRESULT hr;

    if (p->acrylic_w == w && p->acrylic_h == h && p->acrylic_source && p->acrylic_source_srv &&
        p->acrylic_small[0] && p->acrylic_small_srv[0] && p->acrylic_small_rtv[0] &&
        p->acrylic_small[1] && p->acrylic_small_srv[1] && p->acrylic_small_rtv[1] &&
        p->acrylic_noise && p->acrylic_noise_srv)
        return 0;
    pr_acrylic_surface_free(p);
    p->acrylic_small_w = FFMAX(1, (w + 3) / 4);
    p->acrylic_small_h = FFMAX(1, (h + 3) / 4);
    p->acrylic_source = pr_make_tex(p->dev, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, NULL, 0);
    if (!p->acrylic_source) { pr_set_err(err, n, "Acrylic source texture creation failed", E_OUTOFMEMORY); goto fail; }
    memset(&sd, 0, sizeof(sd));
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = p->dev->lpVtbl->CreateShaderResourceView(p->dev, (ID3D11Resource *)p->acrylic_source, &sd,
                                                   &p->acrylic_source_srv);
    if (FAILED(hr)) { pr_set_err(err, n, "CreateShaderResourceView(Acrylic source)", hr); goto fail; }
    memset(&rd, 0, sizeof(rd));
    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    for (i = 0; i < 2; i++) {
        p->acrylic_small[i] = pr_make_tex(p->dev, p->acrylic_small_w, p->acrylic_small_h,
                                            DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT,
                                            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL, 0);
        if (!p->acrylic_small[i]) { pr_set_err(err, n, "Acrylic blur texture creation failed", E_OUTOFMEMORY); goto fail; }
        hr = p->dev->lpVtbl->CreateRenderTargetView(p->dev, (ID3D11Resource *)p->acrylic_small[i], &rd,
                                                    &p->acrylic_small_rtv[i]);
        if (FAILED(hr)) { pr_set_err(err, n, "CreateRenderTargetView(Acrylic blur)", hr); goto fail; }
        hr = p->dev->lpVtbl->CreateShaderResourceView(p->dev, (ID3D11Resource *)p->acrylic_small[i], &sd,
                                                       &p->acrylic_small_srv[i]);
        if (FAILED(hr)) { pr_set_err(err, n, "CreateShaderResourceView(Acrylic blur)", hr); goto fail; }
    }
    if (!p->acrylic_noise) {
        uint32_t state = 0x9e3779b9u;
        noise = av_malloc(256u * 256u);
        if (!noise) { pr_set_err(err, n, "Acrylic noise allocation failed", E_OUTOFMEMORY); goto fail; }
        for (i = 0; i < 256 * 256; i++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            noise[i] = (uint8_t)(state >> 24);
        }
        p->acrylic_noise = pr_make_tex(p->dev, 256, 256, DXGI_FORMAT_R8_UNORM,
                                         D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, noise, 256);
        av_freep(&noise);
        if (!p->acrylic_noise) { pr_set_err(err, n, "Acrylic noise texture creation failed", E_OUTOFMEMORY); goto fail; }
    }
    if (!p->acrylic_noise_srv) {
        memset(&sd, 0, sizeof(sd));
        sd.Format = DXGI_FORMAT_R8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = p->dev->lpVtbl->CreateShaderResourceView(p->dev, (ID3D11Resource *)p->acrylic_noise, &sd,
                                                       &p->acrylic_noise_srv);
        if (FAILED(hr)) { pr_set_err(err, n, "CreateShaderResourceView(Acrylic noise)", hr); goto fail; }
    }
    p->acrylic_w = w;
    p->acrylic_h = h;
    return 0;
fail:
    av_free(noise);
    pr_acrylic_surface_free(p);
    return -1;
}

static void pr_acrylic_draw(PrD3D11 *p, ID3D11RenderTargetView *rtv, ID3D11PixelShader *ps,
                             ID3D11ShaderResourceView **srvs, UINT srv_count, int w, int h,
                             const PrAcrylicCB *cb, const RECT *scissor)
{
    static const FLOAT blend_factor[4] = { 0, 0, 0, 0 };
    ID3D11ShaderResourceView *null_srvs[2] = { NULL, NULL };
    ID3D11Buffer *null_cb = NULL;
    D3D11_VIEWPORT vp;

    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    p->ctx->lpVtbl->RSSetState(p->ctx, scissor ? p->acrylic_renderer.scissor_raster : p->acrylic_renderer.raster);
    p->ctx->lpVtbl->RSSetViewports(p->ctx, 1, &vp);
    if (scissor)
        p->ctx->lpVtbl->RSSetScissorRects(p->ctx, 1, scissor);
    p->ctx->lpVtbl->IASetInputLayout(p->ctx, NULL);
    p->ctx->lpVtbl->IASetPrimitiveTopology(p->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p->ctx->lpVtbl->VSSetShader(p->ctx, p->acrylic_renderer.vs, NULL, 0);
    p->ctx->lpVtbl->GSSetShader(p->ctx, NULL, NULL, 0);
    p->ctx->lpVtbl->HSSetShader(p->ctx, NULL, NULL, 0);
    p->ctx->lpVtbl->DSSetShader(p->ctx, NULL, NULL, 0);
    p->ctx->lpVtbl->PSSetShader(p->ctx, ps, NULL, 0);
    p->ctx->lpVtbl->PSSetSamplers(p->ctx, 0, 1, &p->acrylic_renderer.sampler);
    p->ctx->lpVtbl->PSSetConstantBuffers(p->ctx, 0, 1, &p->acrylic_renderer.constants);
    p->ctx->lpVtbl->PSSetShaderResources(p->ctx, 0, srv_count, srvs);
    p->ctx->lpVtbl->OMSetBlendState(p->ctx, NULL, blend_factor, 0xffffffffu);
    p->ctx->lpVtbl->OMSetRenderTargets(p->ctx, 1, &rtv, NULL);
    p->ctx->lpVtbl->UpdateSubresource(p->ctx, (ID3D11Resource *)p->acrylic_renderer.constants, 0, NULL, cb, 0, 0);
    p->ctx->lpVtbl->Draw(p->ctx, 3, 0);
    p->ctx->lpVtbl->PSSetShaderResources(p->ctx, 0, 2, null_srvs);
    p->ctx->lpVtbl->PSSetConstantBuffers(p->ctx, 0, 1, &null_cb);
    p->ctx->lpVtbl->OMSetRenderTargets(p->ctx, 0, NULL, NULL);
}

static void pr_acrylic_timer_free(PrD3D11 *p)
{
    int i;
    for (i = 0; i < (int)(sizeof(p->acrylic_timers) / sizeof(p->acrylic_timers[0])); i++) {
        PR_REL(p->acrylic_timers[i].disjoint);
        PR_REL(p->acrylic_timers[i].begin);
        PR_REL(p->acrylic_timers[i].end);
        p->acrylic_timers[i].pending = 0;
    }
}

static int pr_acrylic_timer_ensure(PrD3D11 *p)
{
    D3D11_QUERY_DESC qd;
    int i;
    if (p->acrylic_timer_unavailable)
        return -1;
    for (i = 0; i < (int)(sizeof(p->acrylic_timers) / sizeof(p->acrylic_timers[0])); i++) {
        PrAcrylicTimer *t = &p->acrylic_timers[i];
        HRESULT hr;
        if (t->disjoint && t->begin && t->end)
            continue;
        memset(&qd, 0, sizeof(qd));
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        hr = p->dev->lpVtbl->CreateQuery(p->dev, &qd, &t->disjoint);
        if (FAILED(hr)) goto fail;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        hr = p->dev->lpVtbl->CreateQuery(p->dev, &qd, &t->begin);
        if (FAILED(hr)) goto fail;
        hr = p->dev->lpVtbl->CreateQuery(p->dev, &qd, &t->end);
        if (FAILED(hr)) goto fail;
    }
    return 0;
fail:
    pr_acrylic_timer_free(p);
    p->acrylic_timer_unavailable = 1;
    return -1;
}

static void pr_acrylic_timer_poll(PrD3D11 *p)
{
    int i;
    for (i = 0; i < (int)(sizeof(p->acrylic_timers) / sizeof(p->acrylic_timers[0])); i++) {
        PrAcrylicTimer *t = &p->acrylic_timers[i];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        UINT64 begin, end;
        if (t->pending != 2)
            continue;
        if (p->ctx->lpVtbl->GetData(p->ctx, (ID3D11Asynchronous *)t->disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            p->ctx->lpVtbl->GetData(p->ctx, (ID3D11Asynchronous *)t->begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            p->ctx->lpVtbl->GetData(p->ctx, (ID3D11Asynchronous *)t->end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (!disjoint.Disjoint && disjoint.Frequency) {
            double us = (double)(end - begin) * 1000000.0 / (double)disjoint.Frequency;
            InterlockedExchange64(&p->acrylic_last_blur_us, (LONG64)llrint(us));
        }
        t->pending = 0;
    }
}

static int pr_acrylic_timer_begin(PrD3D11 *p)
{
    unsigned k, count = (unsigned)(sizeof(p->acrylic_timers) / sizeof(p->acrylic_timers[0]));
    if (pr_acrylic_timer_ensure(p) < 0)
        return -1;
    pr_acrylic_timer_poll(p);
    for (k = 0; k < count; k++) {
        unsigned i = (p->acrylic_timer_next + k) % count;
        PrAcrylicTimer *t = &p->acrylic_timers[i];
        if (t->pending)
            continue;
        p->ctx->lpVtbl->Begin(p->ctx, (ID3D11Asynchronous *)t->disjoint);
        p->ctx->lpVtbl->End(p->ctx, (ID3D11Asynchronous *)t->begin);
        t->pending = 1;
        p->acrylic_timer_next = (i + 1) % count;
        return (int)i;
    }
    return -1;
}

static void pr_acrylic_timer_end(PrD3D11 *p, int index)
{
    PrAcrylicTimer *t;
    if (index < 0 || index >= (int)(sizeof(p->acrylic_timers) / sizeof(p->acrylic_timers[0])))
        return;
    t = &p->acrylic_timers[index];
    p->ctx->lpVtbl->End(p->ctx, (ID3D11Asynchronous *)t->end);
    p->ctx->lpVtbl->End(p->ctx, (ID3D11Asynchronous *)t->disjoint);
    t->pending = 2;
}

static void pr_frame_gpu_timer_free(PrD3D11 *p)
{
    int i;
    for (i = 0; i < (int)(sizeof(p->frame_gpu_timers) / sizeof(p->frame_gpu_timers[0])); i++) {
        PR_REL(p->frame_gpu_timers[i].disjoint);
        PR_REL(p->frame_gpu_timers[i].begin);
        PR_REL(p->frame_gpu_timers[i].end);
        p->frame_gpu_timers[i].pending = 0;
    }
}

static int pr_frame_gpu_timer_ensure(PrD3D11 *p)
{
    D3D11_QUERY_DESC qd;
    int i;
    if (p->frame_gpu_timer_unavailable)
        return -1;
    for (i = 0; i < (int)(sizeof(p->frame_gpu_timers) / sizeof(p->frame_gpu_timers[0])); i++) {
        PrFrameGpuTimer *t = &p->frame_gpu_timers[i];
        HRESULT hr;
        if (t->disjoint && t->begin && t->end)
            continue;
        memset(&qd, 0, sizeof(qd));
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        hr = p->dev->lpVtbl->CreateQuery(p->dev, &qd, &t->disjoint);
        if (FAILED(hr)) goto fail;
        qd.Query = D3D11_QUERY_TIMESTAMP;
        hr = p->dev->lpVtbl->CreateQuery(p->dev, &qd, &t->begin);
        if (FAILED(hr)) goto fail;
        hr = p->dev->lpVtbl->CreateQuery(p->dev, &qd, &t->end);
        if (FAILED(hr)) goto fail;
    }
    return 0;
fail:
    pr_frame_gpu_timer_free(p);
    p->frame_gpu_timer_unavailable = 1;
    return -1;
}

static void pr_frame_gpu_timer_poll(PrD3D11 *p)
{
    int i;
    for (i = 0; i < (int)(sizeof(p->frame_gpu_timers) / sizeof(p->frame_gpu_timers[0])); i++) {
        PrFrameGpuTimer *t = &p->frame_gpu_timers[i];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        UINT64 begin, end;
        if (t->pending < 2)
            continue;
        if (p->ctx->lpVtbl->GetData(p->ctx, (ID3D11Asynchronous *)t->disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            p->ctx->lpVtbl->GetData(p->ctx, (ID3D11Asynchronous *)t->begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            p->ctx->lpVtbl->GetData(p->ctx, (ID3D11Asynchronous *)t->end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (t->pending == 2 && !disjoint.Disjoint && disjoint.Frequency && end >= begin) {
            LONG64 us = (LONG64)llrint((double)(end - begin) * 1000000.0 / (double)disjoint.Frequency);
            InterlockedExchange64(&p->frame_gpu_last_us, us);
            InterlockedExchange64(&p->frame_gpu_total_us,
                InterlockedCompareExchange64(&p->frame_gpu_total_us, 0, 0) + us);
            InterlockedIncrement64(&p->frame_gpu_count);
        }
        t->pending = 0;
    }
}

static int pr_frame_gpu_timer_begin(PrD3D11 *p)
{
    unsigned k, count = (unsigned)(sizeof(p->frame_gpu_timers) / sizeof(p->frame_gpu_timers[0]));
    if (pr_frame_gpu_timer_ensure(p) < 0)
        return -1;
    pr_frame_gpu_timer_poll(p);
    for (k = 0; k < count; k++) {
        unsigned i = (p->frame_gpu_timer_next + k) % count;
        PrFrameGpuTimer *t = &p->frame_gpu_timers[i];
        if (t->pending)
            continue;
        p->ctx->lpVtbl->Begin(p->ctx, (ID3D11Asynchronous *)t->disjoint);
        p->ctx->lpVtbl->End(p->ctx, (ID3D11Asynchronous *)t->begin);
        t->pending = 1;
        p->frame_gpu_timer_next = (i + 1) % count;
        return (int)i;
    }
    return -1;
}

static void pr_frame_gpu_timer_end(PrD3D11 *p, int index, int discard)
{
    PrFrameGpuTimer *t;
    if (index < 0 || index >= (int)(sizeof(p->frame_gpu_timers) / sizeof(p->frame_gpu_timers[0])))
        return;
    t = &p->frame_gpu_timers[index];
    if (t->pending != 1)
        return;
    p->ctx->lpVtbl->End(p->ctx, (ID3D11Asynchronous *)t->end);
    p->ctx->lpVtbl->End(p->ctx, (ID3D11Asynchronous *)t->disjoint);
    t->pending = discard ? 3 : 2;
}

static int pr_acrylic_apply(PrD3D11 *p, ID3D11Texture2D *bb, ID3D11RenderTargetView *rtv,
                             int w, int h, char *err, size_t n)
{
    PrAcrylicCB cb;
    ID3D11ShaderResourceView *srvs[2];
    int i, timer = -1;

    if (!p->acrylic_region_count)
        return 0;
    if (!p->acrylic_renderer_ready) {
        if (p->acrylic_renderer_unavailable ||
            pr_acrylic_renderer_create(p->dev, &p->acrylic_renderer, err, n) < 0) {
            p->acrylic_renderer_unavailable = 1;
            goto fail;
        }
        p->acrylic_renderer_ready = 1;
    }
    if (pr_acrylic_surface_ensure(p, w, h, err, n) < 0)
        goto fail;
    timer = pr_acrylic_timer_begin(p);
    p->ctx->lpVtbl->CopyResource(p->ctx, (ID3D11Resource *)p->acrylic_source, (ID3D11Resource *)bb);
    memset(&cb, 0, sizeof(cb));
    cb.target[0] = (float)p->acrylic_small_w;
    cb.target[1] = (float)p->acrylic_small_h;
    cb.target[2] = 1.0f / cb.target[0];
    cb.target[3] = 1.0f / cb.target[1];
    cb.source[0] = (float)w;
    cb.source[1] = (float)h;
    cb.source[2] = 1.0f / (float)w;
    cb.source[3] = 1.0f / (float)h;
    srvs[0] = p->acrylic_source_srv;
    pr_acrylic_draw(p, p->acrylic_small_rtv[0], p->acrylic_renderer.downsample_ps,
                     srvs, 1, p->acrylic_small_w, p->acrylic_small_h, &cb, NULL);

    cb.target[0] = cb.source[0] = (float)p->acrylic_small_w;
    cb.target[1] = cb.source[1] = (float)p->acrylic_small_h;
    cb.target[2] = cb.source[2] = 1.0f / cb.target[0];
    cb.target[3] = cb.source[3] = 1.0f / cb.target[1];
    cb.params[2] = 1.0f;
    cb.params[3] = 0.0f;
    srvs[0] = p->acrylic_small_srv[0];
    pr_acrylic_draw(p, p->acrylic_small_rtv[1], p->acrylic_renderer.blur_ps,
                     srvs, 1, p->acrylic_small_w, p->acrylic_small_h, &cb, NULL);
    cb.params[2] = 0.0f;
    cb.params[3] = 1.0f;
    srvs[0] = p->acrylic_small_srv[1];
    pr_acrylic_draw(p, p->acrylic_small_rtv[0], p->acrylic_renderer.blur_ps,
                     srvs, 1, p->acrylic_small_w, p->acrylic_small_h, &cb, NULL);

    cb.target[0] = (float)w;
    cb.target[1] = (float)h;
    cb.target[2] = 1.0f / (float)w;
    cb.target[3] = 1.0f / (float)h;
    cb.source[0] = (float)p->acrylic_small_w;
    cb.source[1] = (float)p->acrylic_small_h;
    cb.source[2] = 1.0f / cb.source[0];
    cb.source[3] = 1.0f / cb.source[1];
    srvs[0] = p->acrylic_small_srv[0];
    srvs[1] = p->acrylic_noise_srv;
    for (i = 0; i < p->acrylic_region_count; i++) {
        const PrBackdropRegion *r = &p->acrylic_regions[i];
        int64_t right = (int64_t)r->x + r->width;
        int64_t bottom = (int64_t)r->y + r->height;
        RECT clip;
        clip.left = (LONG)av_clip(r->x, 0, w);
        clip.top = (LONG)av_clip(r->y, 0, h);
        clip.right = (LONG)FFMIN((int64_t)w, FFMAX((int64_t)0, right));
        clip.bottom = (LONG)FFMIN((int64_t)h, FFMAX((int64_t)0, bottom));
        if (clip.right <= clip.left || clip.bottom <= clip.top)
            continue;
        cb.region[0] = (float)r->x;
        cb.region[1] = (float)r->y;
        cb.region[2] = (float)r->width;
        cb.region[3] = (float)r->height;
        cb.tint[0] = ((r->tint_color >> 16) & 255) / 255.0f;
        cb.tint[1] = ((r->tint_color >> 8) & 255) / 255.0f;
        cb.tint[2] = (r->tint_color & 255) / 255.0f;
        cb.tint[3] = r->tint_opacity;
        cb.luminosity[0] = ((r->luminosity_color >> 16) & 255) / 255.0f;
        cb.luminosity[1] = ((r->luminosity_color >> 8) & 255) / 255.0f;
        cb.luminosity[2] = (r->luminosity_color & 255) / 255.0f;
        cb.luminosity[3] = r->luminosity_opacity;
        cb.params[0] = r->corner_radius;
        cb.params[1] = r->noise ? 1.0f : 0.0f;
        cb.params[2] = cb.params[3] = 0.0f;
        pr_acrylic_draw(p, rtv, p->acrylic_renderer.composite_ps,
                         srvs, 2, w, h, &cb, &clip);
    }
    pr_acrylic_timer_end(p, timer);
    InterlockedIncrement64(&p->acrylic_frames_blurred);
    return 0;
fail:
    InterlockedIncrement64(&p->acrylic_failures);
    return -1;
}

static void pr_post_redraw(PrD3D11 *p);

static int pr_acrylic_region_valid(const PrBackdropRegion *r)
{
    if (!r || r->width <= 0 || r->height <= 0 ||
        (int64_t)r->x + r->width < INT_MIN || (int64_t)r->x + r->width > INT_MAX ||
        (int64_t)r->y + r->height < INT_MIN || (int64_t)r->y + r->height > INT_MAX ||
        !isfinite(r->corner_radius) || r->corner_radius < 0.0f ||
        !isfinite(r->tint_opacity) || r->tint_opacity < 0.0f || r->tint_opacity > 1.0f ||
        !isfinite(r->luminosity_opacity) || r->luminosity_opacity < 0.0f || r->luminosity_opacity > 1.0f ||
        (r->noise != 0 && r->noise != 1))
        return 0;
    return 1;
}

static int pr_backdrop_compare(const PrBackdropRegion *a, const PrBackdropRegion *b)
{
    return a->z < b->z ? -1 : a->z > b->z ? 1 : 0;
}

int pr_d3d11_set_backdrop_regions(PrD3D11 *p, const PrBackdropRegion *regions, int count,
                                   char *err, size_t err_size)
{
    int i, j;
    PrBackdropRegion key;
    if (!p || count < 0 || count > PR_BACKDROP_REGION_MAX || (count && !regions)) {
        pr_set_err(err, err_size, "invalid Acrylic region list", E_INVALIDARG);
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!pr_acrylic_region_valid(&regions[i])) {
            pr_set_err(err, err_size, "invalid Acrylic region geometry or opacity", E_INVALIDARG);
            return -1;
        }
    }
    EnterCriticalSection(&p->cs);
    for (i = 0; i < count; i++) {
        key = regions[i];
        j = i;
        while (j > 0 && pr_backdrop_compare(&key, &p->acrylic_regions[j - 1]) < 0) {
            p->acrylic_regions[j] = p->acrylic_regions[j - 1];
            j--;
        }
        p->acrylic_regions[j] = key;
    }
    p->acrylic_region_count = count;
    LeaveCriticalSection(&p->cs);
    pr_post_redraw(p);
    return 0;
}

/* Can a decoder pool of this format have DECODER|SHADER_RESOURCE? Probed with a texture array that has the misc flags
 * of a real pool (hwcontext_d3d11va creates its pools SHARED) and the plane formats the shader reads. */
static int pr_decoder_srv_supported(ID3D11Device *dev, DXGI_FORMAT fmt)
{
    const DXGI_FORMAT planes[2][2] = { { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM },
                                       { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM } };
    const DXGI_FORMAT *pl = planes[fmt == DXGI_FORMAT_P010];
    D3D11_TEXTURE2D_DESC td = { 0 };
    ID3D11Texture2D *tex = NULL;
    UINT support;
    int i, ok;

    for (i = 0; i < 2; i++)
        if (FAILED(dev->lpVtbl->CheckFormatSupport(dev, pl[i], &support)) ||
            !(support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE))
            return 0;
    td.Width = 1920;
    td.Height = 1088;
    td.ArraySize = 8;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ok = SUCCEEDED(dev->lpVtbl->CreateTexture2D(dev, &td, NULL, &tex));
    PR_REL(tex);
    return ok;
}

static int pr_probe_limits_dev(ID3D11Device *dev, ID3D11DeviceContext *ctx, PrLimits *l)
{
    ID3D11VideoDevice *vd = NULL;
    ID3D11VideoProcessorEnumerator *en = NULL;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd;
    D3D11_VIDEO_PROCESSOR_CAPS caps;
    UINT i, fm;
    HRESULT hr;

    (void)ctx;
    if (FAILED(hr = dev->lpVtbl->QueryInterface(dev, &pr_IID_ID3D11VideoDevice, (void **)&vd))) {
        snprintf(l->error, sizeof(l->error), "no video device (0x%08lx)", (unsigned long)hr);
        goto end;
    }
    memset(&cd, 0, sizeof(cd));
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = 64;
    cd.InputHeight = 32;
    cd.OutputWidth = 64;
    cd.OutputHeight = 64;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(hr = vd->lpVtbl->CreateVideoProcessorEnumerator(vd, &cd, &en)) ||
        FAILED(hr = en->lpVtbl->GetVideoProcessorCaps(en, &caps))) {
        snprintf(l->error, sizeof(l->error), "video processor enumerator failed (0x%08lx)", (unsigned long)hr);
        goto end;
    }
    for (i = 0; i < caps.RateConversionCapsCount; i++) {
        D3D11_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rc;
        if (SUCCEEDED(en->lpVtbl->GetVideoProcessorRateConversionCaps(en, i, &rc))) {
            l->proc_caps |= rc.ProcessorCaps;
            l->telecine_caps |= rc.ITelecineCaps;
        }
    }
    if (SUCCEEDED(en->lpVtbl->CheckVideoProcessorFormat(en, DXGI_FORMAT_NV12, &fm)))
        l->nv12 = (fm & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;
    if (SUCCEEDED(en->lpVtbl->CheckVideoProcessorFormat(en, DXGI_FORMAT_P010, &fm)))
        l->p010 = (fm & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) != 0;
    if (!l->nv12) {
        snprintf(l->error, sizeof(l->error), "the video processor does not take NV12 input");
        goto end;
    }
    l->nv12_srv = pr_decoder_srv_supported(dev, DXGI_FORMAT_NV12);
    l->p010_srv = pr_decoder_srv_supported(dev, DXGI_FORMAT_P010);
    l->ok = 1;
end:
    PR_REL(en);
    PR_REL(vd);
    return l->ok ? 0 : -1;
}

#define PR_LIMITS_CACHE 4

static SRWLOCK g_limits_lock = SRWLOCK_INIT;
static struct {
    int used, adapter_set;
    uint32_t high, low;
    PrLimits l;               /* as probed: no capability masked */
} g_limits_cache[PR_LIMITS_CACHE];

/* the force caps bits that belong to the video processor limits (the others are applied by the presenter) */
static void pr_apply_force(PrLimits *l, unsigned mask)
{
    l->forced = mask;
    if (mask & PR_FORCE_NV12_SRV)
        l->nv12_srv = 0;
    if (mask & PR_FORCE_P010_SRV)
        l->p010_srv = 0;
    if (mask & PR_FORCE_P010)
        l->p010 = 0;
}

/* the video processor limits of the adapter of cfg (cached per LUID), with cfg->force applied to the copy */
static int pr_get_limits(const PrConfig *cfg, PrLimits *out)
{
    int set = cfg->adapter_set, i;

    AcquireSRWLockExclusive(&g_limits_lock);
    for (i = 0; i < PR_LIMITS_CACHE; i++)
        if (g_limits_cache[i].used && g_limits_cache[i].adapter_set == set &&
            (!set || (g_limits_cache[i].high == cfg->luid_high && g_limits_cache[i].low == cfg->luid_low)))
            break;
    if (i == PR_LIMITS_CACHE) {
        PrLimits l;
        PrConfig dc;
        ID3D11Device *dev = NULL;
        ID3D11DeviceContext *ctx = NULL;
        PrDebug *dbg = NULL;
        HRESULT hr;

        memset(&l, 0, sizeof(l));
        memset(&dc, 0, sizeof(dc));
        if (set) {
            dc.adapter_set = 1;
            dc.luid_high = cfg->luid_high;
            dc.luid_low = cfg->luid_low;
        }
        dc.debug = cfg->debug;
        hr = pr_create_device(&dc, &dev, &ctx, &dbg);
        if (FAILED(hr)) {
            pr_create_device_error(&dc, hr, l.error, sizeof(l.error));
        } else {
            pr_probe_limits_dev(dev, ctx, &l);
            pr_dbg_drain(dbg, "probe", 0);
        }
        pr_dbg_free(dbg);
        PR_REL(ctx);
        PR_REL(dev);
        if (FAILED(hr)) {           /* not cached: the GPU may show up (or recover) later */
            *out = l;
            ReleaseSRWLockExclusive(&g_limits_lock);
            return 0;
        }
        for (i = 0; i < PR_LIMITS_CACHE && g_limits_cache[i].used; i++)
            ;
        if (i == PR_LIMITS_CACHE) {
            memmove(g_limits_cache, g_limits_cache + 1, (PR_LIMITS_CACHE - 1) * sizeof(g_limits_cache[0]));
            i = PR_LIMITS_CACHE - 1;
        }
        g_limits_cache[i].used = 1;
        g_limits_cache[i].adapter_set = set;
        g_limits_cache[i].high = set ? cfg->luid_high : 0;
        g_limits_cache[i].low = set ? cfg->luid_low : 0;
        g_limits_cache[i].l = l;
    }
    *out = g_limits_cache[i].l;
    pr_apply_force(out, cfg->force);
    ReleaseSRWLockExclusive(&g_limits_lock);
    return out->ok ? 1 : 0;
}

static int pr_shader_avail(const PrD3D11 *p)
{
    return !(p->cfg.force & PR_FORCE_SHADER) && !p->shader_failed;
}

static int pr_input_p010(const PrInput *in)
{
    D3D11_TEXTURE2D_DESC td;

    in->tex->lpVtbl->GetDesc(in->tex, &td);
    return td.Format == DXGI_FORMAT_P010;
}

enum { PR_ROUTE_PICK_VP = 0, PR_ROUTE_PICK_SHADER = 1, PR_ROUTE_PICK_DOWNLOAD = 2 };

/* The route of a picture: the VideoProcessor, the pixel shader, or (P010 that the video processor cannot take, with no
 * shader) a download that is uploaded as NV12. display: the picture is presented (the forced route applies and is
 * recorded); a readback only needs a route that works. */
static int pr_pick_route(PrD3D11 *p, const PrInput *in, int display)
{
    int p010 = !p->lim.p010 && pr_input_p010(in);
    int avail = pr_shader_avail(p);
    int route;

    route = p010 && avail ? PR_ROUTE_PICK_SHADER : p010 ? PR_ROUTE_PICK_DOWNLOAD : PR_ROUTE_PICK_VP;
    if (display && p->cfg.route) {
        int vp = p->cfg.route == PR_ROUTE_VP;
        int possible = vp ? !p010 : avail;
        char name[48];

        snprintf(name, sizeof(name), "route_forced%s:%s", possible ? "" : "_unavailable", vp ? "vp" : "shader");
        pr_record_fallback(p, name);
        if (possible)
            route = vp ? PR_ROUTE_PICK_VP : PR_ROUTE_PICK_SHADER;
    }
    return route;
}

static int pr_upload(PrD3D11 *p, const AVFrame *f, PrInput *in, char *err, size_t err_size);

/* A D3D11 frame that no route takes as is (P010 without video processor P010 input and without the shader route):
 * downloaded, then it takes the path of a system frame. */
static int pr_download_input(PrD3D11 *p, const AVFrame *f, PrInput *in, char *err, size_t err_size)
{
    int ret;

    if (!p->dl && !(p->dl = av_frame_alloc())) {
        pr_set_err(err, err_size, "out of memory", S_OK);
        return -1;
    }
    av_frame_unref(p->dl);
    ret = av_hwframe_transfer_data(p->dl, f, 0);
    if (ret >= 0)
        ret = av_frame_copy_props(p->dl, f);
    if (ret < 0) {
        snprintf(err, err_size, "download of a D3D11 frame failed (%s)", av_err2str(ret));
        return -1;
    }
    return pr_upload(p, p->dl, in, err, err_size);
}

#include "d3d11_shader.c"

void pr_probe_presenter(const PrConfig *cfg, PrPresenterCaps *out)
{
    PrLimits l;

    memset(out, 0, sizeof(*out));
    out->ok = pr_get_limits(cfg, &l);
    av_strlcpy(out->error, l.error, sizeof(out->error));
    out->p010 = l.p010;
    out->nv12_srv = l.nv12_srv;
    out->p010_srv = l.p010_srv;
    out->shader = out->ok && !(cfg->force & PR_FORCE_SHADER) && pr_probe_d3d11_shader(cfg);
    out->deint_blend = !!(l.proc_caps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND);
    out->deint_bob = !!(l.proc_caps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB);
    out->deint_adaptive = !!(l.proc_caps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE);
    out->deint_motion = !!(l.proc_caps & D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION);
    out->telecine = l.telecine_caps != 0;
    out->forced = cfg->force;
}

static LONG g_class_refs;
static const WCHAR pr_wnd_class[] = L"wvp.presenter.d3d11";

#define PR_WM_SHOW   (WM_APP + 1)
#define PR_WM_SIZE   (WM_APP + 2)   /* wp = width, lp = height */
#define PR_WM_QUIT   (WM_APP + 3)
#define PR_WM_REDRAW (WM_APP + 4)   /* wp = 1: the host view was resized */

static void pr_redraw_held(PrD3D11 *p, int host_resized);

static LRESULT CALLBACK pr_child_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case PR_WM_SHOW:
        ShowWindow(h, SW_SHOWNA);
        return 0;
    case PR_WM_SIZE:
        SetWindowPos(h, NULL, 0, 0, (int)wp, (int)lp, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        return 0;
    case PR_WM_REDRAW: {
        PrD3D11 *p = (PrD3D11 *)GetWindowLongPtrW(h, GWLP_USERDATA);
        MSG pending;

        /* a burst of resizes (a window drag) redraws once for the last one */
        while (PeekMessageW(&pending, h, PR_WM_REDRAW, PR_WM_REDRAW, PM_REMOVE))
            wp |= pending.wParam;
        if (p)
            pr_redraw_held(p, (int)wp);
        return 0;
    }
    case PR_WM_QUIT:
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        DefWindowProcW(h, msg, wp, lp);
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        /* never takes the pointer (also covered by WS_DISABLED): the host view underneath gets hit-testing, mouse
         * input and cursor handling directly on its own thread, so its own hit-test rules (popups, bars) apply */
        return HTTRANSPARENT;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HMODULE pr_this_module(void)
{
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)pr_child_proc, &m);
    return m;
}

/* The child window lives on its own thread with a real message loop: a window owned by a thread that does not pump
 * deadlocks every cross-thread SendMessage of the host (SetWindowPos / ShowWindow / SetParent on the video view).
 * Everything the other threads do to the window is posted; the window thread never sends to the host
 * (WS_EX_NOPARENTNOTIFY, mouse input is posted), so pr_d3d11_destroy cannot deadlock with a blocked host. The thread
 * also redraws the held picture when the host view is resized or the backdrop changes. */
static DWORD WINAPI pr_window_thread(LPVOID arg)
{
    PrD3D11 *p = arg;
    MSG m;

    p->child = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE, pr_wnd_class, L"",
                               WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, 0, 0, p->wcw, p->wch, p->host, NULL,
                               pr_this_module(), p);
    p->wres = p->child ? 0 : (int)GetLastError();
    SetEvent(p->wready);
    if (!p->child)
        return 0;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

static int pr_start_window(PrD3D11 *p, int w, int h)
{
    p->wcw = w;
    p->wch = h;
    p->wready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!p->wready)
        return -1;
    p->wthread = CreateThread(NULL, 0, pr_window_thread, p, 0, &p->wtid);
    if (!p->wthread) {
        CloseHandle(p->wready);
        p->wready = NULL;
        return -1;
    }
    WaitForSingleObject(p->wready, INFINITE);       /* the thread needs nothing from the caller or the host */
    if (p->wres) {
        WaitForSingleObject(p->wthread, INFINITE);
        CloseHandle(p->wthread);
        CloseHandle(p->wready);
        p->wthread = p->wready = NULL;
        p->child = NULL;
        return -1;
    }
    return 0;
}

static void pr_stop_window(PrD3D11 *p)
{
    if (!p->wthread)
        return;
    if (IsWindow(p->child))
        PostMessageW(p->child, PR_WM_QUIT, 0, 0);
    else
        PostThreadMessageW(p->wtid, WM_QUIT, 0, 0);
    WaitForSingleObject(p->wthread, 5000);
    CloseHandle(p->wthread);
    CloseHandle(p->wready);
    p->wthread = p->wready = NULL;
    p->child = NULL;
}

static int pr_register_class(void)
{
    WNDCLASSEXW wc;

    if (InterlockedIncrement(&g_class_refs) > 1)
        return 0;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = pr_child_proc;
    wc.hInstance = pr_this_module();
    wc.lpszClassName = pr_wnd_class;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        InterlockedDecrement(&g_class_refs);
        return -1;
    }
    return 0;
}

static void pr_unregister_class(void)
{
    if (InterlockedDecrement(&g_class_refs) == 0)
        UnregisterClassW(pr_wnd_class, pr_this_module());
}

static void pr_post_redraw(PrD3D11 *p)
{
    HWND child = p->child;

    if (child)
        PostMessageW(child, PR_WM_REDRAW, 0, 0);
}

void pr_d3d11_host_resized(PrD3D11 *p)
{
    HWND child = p ? p->child : NULL;

    if (child)
        PostMessageW(child, PR_WM_REDRAW, 1, 0);
}

static void pr_record_fallback(PrD3D11 *p, const char *name)
{
    int i;

    if (!p || !name)
        return;
    EnterCriticalSection(&p->fb_cs);
    for (i = 0; i < p->nb_fallbacks; i++)
        if (!strcmp(p->fallbacks[i], name))
            break;
    if (i == p->nb_fallbacks && i < PR_MAX_FALLBACKS) {
        av_strlcpy(p->fallbacks[i], name, sizeof(p->fallbacks[i]));
        p->nb_fallbacks++;
        av_log(NULL, AV_LOG_VERBOSE, "d3d11 presenter fallback: %s\n", name);
    }
    LeaveCriticalSection(&p->fb_cs);
}

static void pr_json_str(AVBPrint *bp, const char *s)
{
    av_bprintf(bp, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            av_bprintf(bp, "\\%c", *s);
        else if ((unsigned char)*s >= 0x20)
            av_bprintf(bp, "%c", *s);
    }
    av_bprintf(bp, "\"");
}

static int pr_hr_lost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

int pr_d3d11_device_lost(PrD3D11 *p, unsigned *reason)
{
    HRESULT r = S_OK, last;

    if (!p)
        return 0;
    if (p->forced_lost) {
        *reason = (unsigned)DXGI_ERROR_DEVICE_REMOVED;
        return 1;
    }
    if (p->dev && (r = p->dev->lpVtbl->GetDeviceRemovedReason(p->dev)) != S_OK) {
        *reason = (unsigned)r;
        return 1;
    }
    last = (HRESULT)InterlockedCompareExchange(&p->failed_hr, 0, 0);
    if (pr_hr_lost(last)) {
        *reason = (unsigned)last;
        return 1;
    }
    return 0;
}

PrD3D11 *pr_d3d11_create(void *host_hwnd, const PrConfig *cfg, int fail_mode, PrRedrawFn redraw, void *redraw_opaque,
                         char *err, size_t err_size)
{
    PrD3D11 *p;
    IDXGIDevice *dxdev = NULL;
    IDXGIAdapter *ad = NULL;
    PrDebug *dbg = NULL;
    DXGI_ADAPTER_DESC adesc;
    HRESULT hr;
    int ret;
    size_t i;

    if (!host_hwnd || !IsWindow((HWND)host_hwnd)) {
        pr_set_err(err, err_size, "the d3d11 presenter needs a host window", S_OK);
        return NULL;
    }
    p = av_mallocz(sizeof(*p));
    if (!p) {
        pr_set_err(err, err_size, "out of memory", S_OK);
        return NULL;
    }
    InitializeCriticalSection(&p->sc_cs);
    InitializeCriticalSection(&p->cs);
    InitializeCriticalSection(&p->fb_cs);
    p->host = (HWND)host_hwnd;
    p->fail_mode = fail_mode;
    p->redraw = redraw;
    p->redraw_opaque = redraw_opaque;
    p->cfg = *cfg;
    if (!pr_get_limits(&p->cfg, &p->lim)) {
        snprintf(err, err_size, "%s", p->lim.error[0] ? p->lim.error : "video processor unavailable");
        goto fail;
    }
    hr = pr_create_device(&p->cfg, &p->dev, &p->ctx, &dbg);
    if (FAILED(hr)) {
        pr_create_device_error(&p->cfg, hr, err, err_size);
        goto fail;
    }
    CHECK_HR(p->dev->lpVtbl->QueryInterface(p->dev, &pr_IID_ID3D11VideoDevice, (void **)&p->vdev), "ID3D11VideoDevice");
    CHECK_HR(p->ctx->lpVtbl->QueryInterface(p->ctx, &pr_IID_ID3D11VideoContext, (void **)&p->vctx), "ID3D11VideoContext");
    p->ctx->lpVtbl->QueryInterface(p->ctx, &pr_IID_ID3D11VideoContext1, (void **)&p->vctx1);
    CHECK_HR(p->dev->lpVtbl->QueryInterface(p->dev, &pr_IID_IDXGIDevice, (void **)&dxdev), "IDXGIDevice");
    CHECK_HR(dxdev->lpVtbl->GetAdapter(dxdev, &ad), "IDXGIDevice::GetAdapter");
    if (SUCCEEDED(ad->lpVtbl->GetDesc(ad, &adesc)))
        pr_adapter_from_desc(&adesc, &p->adapter);
    CHECK_HR(ad->lpVtbl->GetParent(ad, &pr_IID_IDXGIFactory2, (void **)&p->factory), "IDXGIFactory2 (flip-model swap chain unavailable)");

    /* wraps (and owns a reference to) our device; patch 1007's decoder_bind_flags make decoder pools shader resources */
    ret = pr_wrap_hwdevice(p->dev, dbg, p->lim.nv12_srv ? D3D11_BIND_SHADER_RESOURCE : 0,
                            p->lim.p010_srv ? D3D11_BIND_SHADER_RESOURCE : 0, &p->hwdev, err, err_size);
    if (ret < 0)
        goto fail;
    p->dbg = dbg;
    dbg = NULL;
    p->hwctx = ((AVHWDeviceContext *)p->hwdev->data)->hwctx;
    p->hwctx->decoder_bind_test_fail = p->cfg.srv_fail;
    pr_dbg_drain(p->dbg, "init", 0);
    for (i = 0; i < FF_ARRAY_ELEMS(pr_force_keys); i++)
        if (pr_force_keys[i].bits != (PR_FORCE_NV12_SRV | PR_FORCE_P010_SRV) &&
            (p->cfg.force & pr_force_keys[i].bits)) {
            char name[48];
            snprintf(name, sizeof(name), "force_caps:%s=0", pr_force_keys[i].name);
            pr_record_fallback(p, name);
        }
    PR_REL(ad);
    PR_REL(dxdev);
    if (pr_register_class() < 0) {
        pr_set_err(err, err_size, "RegisterClassEx failed", S_OK);
        goto fail;
    }
    p->class_registered = 1;
    return p;
fail:
    PR_REL(ad);
    PR_REL(dxdev);
    pr_dbg_free(dbg);
    pr_d3d11_destroy(p);
    return NULL;
}

static void pr_destroy_swapchain(PrD3D11 *p)
{
    pr_vp_free(&p->disp);
    PR_REL(p->sc);
    pr_stop_window(p);
    p->sc_w = p->sc_h = 0;
    p->shown = 0;
}

void pr_d3d11_destroy(PrD3D11 *p)
{
    if (!p)
        return;
    InterlockedExchange(&p->closing, 1);
    EnterCriticalSection(&p->cs);
    av_frame_free(&p->held);
    pr_destroy_swapchain(p);
    pr_vp_free(&p->snap);
    pr_overlay_cache_free(&p->overlay[FFP_OVERLAY_CAPTION]);
    pr_overlay_cache_free(&p->overlay[FFP_OVERLAY_SUPERIMPOSE]);
    pr_overlay_renderer_free(&p->overlay_renderer);
    pr_acrylic_surface_free(p);
    PR_REL(p->acrylic_noise_srv);
    PR_REL(p->acrylic_noise);
    pr_acrylic_renderer_free(&p->acrylic_renderer);
    pr_acrylic_timer_free(p);
    pr_frame_gpu_timer_free(p);
    PR_REL(p->overlay_readback_staging);
    av_freep(&p->overlay_readback_data);
    av_frame_free(&p->up_frame);
    av_frame_free(&p->swf);
    av_frame_free(&p->dl);
    av_buffer_unref(&p->up_frames);
    if (p->ctx)
        p->ctx->lpVtbl->Flush(p->ctx);
    PR_REL(p->factory);
    PR_REL(p->vctx1);
    PR_REL(p->vctx);
    PR_REL(p->vdev);
    PR_REL(p->ctx);
    PR_REL(p->dev);
    pr_dbg_drain(p->dbg, "close", 0);
    p->dbg = NULL;
    av_buffer_unref(&p->hwdev);      /* releases its own reference to the device, and the debug layer state (last drain) */
    pr_shader_free(p->shader);
    LeaveCriticalSection(&p->cs);
    if (p->class_registered)
        pr_unregister_class();
    DeleteCriticalSection(&p->fb_cs);
    DeleteCriticalSection(&p->cs);
    DeleteCriticalSection(&p->sc_cs);
    av_free(p);
}

AVBufferRef *pr_d3d11_device(PrD3D11 *p)
{
    return p->hwdev;
}

int pr_d3d11_p010_ok(PrD3D11 *p)
{
    return p->lim.p010 || pr_shader_avail(p);
}

/* is this a D3D11 NV12/P010 frame of the device `dev`? */
static int pr_frame_ok(const void *dev, const AVFrame *f)
{
    AVHWFramesContext *fc;
    AVHWDeviceContext *dc;

    if (!dev || !f || f->format != AV_PIX_FMT_D3D11 || !f->hw_frames_ctx)
        return 0;
    fc = (AVHWFramesContext *)f->hw_frames_ctx->data;
    if (fc->sw_format != AV_PIX_FMT_NV12 && fc->sw_format != AV_PIX_FMT_P010)
        return 0;
    dc = fc->device_ctx;
    return dc && dc->type == AV_HWDEVICE_TYPE_D3D11VA &&
           ((AVD3D11VADeviceContext *)dc->hwctx)->device == (ID3D11Device *)dev;
}

int pr_d3d11_accepts(PrD3D11 *p, const AVFrame *f)
{
    if (f->hw_frames_ctx && ((AVHWFramesContext *)f->hw_frames_ctx->data)->sw_format == AV_PIX_FMT_P010 &&
        !pr_d3d11_p010_ok(p))
        return 0;
    return pr_frame_ok(p->dev, f);
}

/* DXGI frame statistics of the last successful present (1 when available, 0 when disjoint / unsupported) and the GPU
 * time of the presented frames */
static int pr_frame_stats(PrD3D11 *p, uint32_t *present_count, uint32_t *present_refresh_count,
                          uint32_t *sync_refresh_count, uint64_t *sync_qpc_time, uint64_t *sync_gpu_time,
                          uint64_t *gpu_total_us, uint64_t *gpu_frames, uint64_t *gpu_last_us)
{
    DXGI_FRAME_STATISTICS stats;
    HRESULT hr;

    pr_frame_gpu_timer_poll(p);
    *gpu_total_us = (uint64_t)InterlockedCompareExchange64(&p->frame_gpu_total_us, 0, 0);
    *gpu_frames = (uint64_t)InterlockedCompareExchange64(&p->frame_gpu_count, 0, 0);
    *gpu_last_us = (uint64_t)InterlockedCompareExchange64(&p->frame_gpu_last_us, 0, 0);
    if (!p->sc)
        return 0;
    memset(&stats, 0, sizeof(stats));
    hr = p->sc->lpVtbl->GetFrameStatistics(p->sc, &stats);
    if (FAILED(hr))
        return 0;
    *present_count = stats.PresentCount;
    *present_refresh_count = stats.PresentRefreshCount;
    *sync_refresh_count = stats.SyncRefreshCount;
    *sync_qpc_time = (uint64_t)stats.SyncQPCTime.QuadPart;
    *sync_gpu_time = (uint64_t)stats.SyncGPUTime.QuadPart;
    return 1;
}

/* src (NV12 / P010) copied into dst of even dimensions (NV12 / P010 textures), the last column / row repeated into the
 * padding (only the luma plane can need a column); P010 -> NV12 keeps the 8 most significant bits (when the presenter
 * has no route for P010). */
static void pr_pad_planes(const AVFrame *src, AVFrame *dst)
{
    int in16 = src->format == AV_PIX_FMT_P010LE, out16 = dst->format == AV_PIX_FMT_P010LE;
    int plane;

    for (plane = 0; plane < 2; plane++) {
        int w = plane ? (src->width + 1) / 2 * 2 : src->width;          /* samples per row (UV interleaved) */
        int h = plane ? (src->height + 1) / 2 : src->height;
        int pw = dst->width, ph = plane ? dst->height / 2 : dst->height;
        int x, y;

        for (y = 0; y < ph; y++) {
            const uint8_t *s = src->data[plane] + (size_t)FFMIN(y, h - 1) * src->linesize[plane];
            uint8_t *d = dst->data[plane] + (size_t)y * dst->linesize[plane];

            if (in16 == out16) {
                memcpy(d, s, (size_t)w << out16);
                for (x = w; x < pw; x++)
                    memcpy(d + ((size_t)x << out16), d + ((size_t)(x - 1) << out16), (size_t)1 << out16);
            } else {
                const uint16_t *s16 = (const uint16_t *)s;
                for (x = 0; x < pw; x++)
                    d[x] = (uint8_t)(s16[FFMIN(x, w - 1)] >> 8);
            }
        }
    }
}

static int pr_upload(PrD3D11 *p, const AVFrame *f, PrInput *in, char *err, size_t err_size)
{
    enum AVPixelFormat tgt;
    const AVFrame *src = f;
    AVFrame *hw = NULL;
    int w = f->width, h = f->height, pw, ph, ret;

    if (w <= 0 || h <= 0 ||
        (f->format != AV_PIX_FMT_NV12 && f->format != AV_PIX_FMT_P010LE && f->format != AV_PIX_FMT_BGRA)) {
        pr_set_err(err, err_size, "unsupported system frame format", S_OK);
        return -1;
    }
    tgt = f->format;
    if (tgt == AV_PIX_FMT_P010LE && !p->lim.p010 && !pr_shader_avail(p)) {
        tgt = AV_PIX_FMT_NV12;
        pr_record_fallback(p, "p010_no_route:nv12");
    }
    pw = tgt == AV_PIX_FMT_BGRA ? w : (w + 1) & ~1;
    ph = tgt == AV_PIX_FMT_BGRA ? h : (h + 1) & ~1;

    if (!p->up_frames || p->up_fmt != (int)tgt || p->up_w != pw || p->up_h != ph) {
        AVHWFramesContext *fc;
        av_buffer_unref(&p->up_frames);
        p->up_frames = av_hwframe_ctx_alloc(p->hwdev);
        if (!p->up_frames) {
            pr_set_err(err, err_size, "av_hwframe_ctx_alloc failed", S_OK);
            return -1;
        }
        fc = (AVHWFramesContext *)p->up_frames->data;
        fc->format = AV_PIX_FMT_D3D11;
        fc->sw_format = tgt;
        fc->width = pw;
        fc->height = ph;
        fc->initial_pool_size = 4;
        ((AVD3D11VAFramesContext *)fc->hwctx)->BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ret = av_hwframe_ctx_init(p->up_frames);
        if (ret < 0) {
            snprintf(err, err_size, "upload surface pool failed (%s)", av_err2str(ret));
            av_buffer_unref(&p->up_frames);
            return -1;
        }
        p->up_fmt = tgt;
        p->up_w = pw;
        p->up_h = ph;
    }

    if ((int)tgt != f->format || pw != w || ph != h) {
        if (!p->swf || p->swf->format != (int)tgt || p->swf->width != pw || p->swf->height != ph) {
            av_frame_free(&p->swf);
            p->swf = av_frame_alloc();
            if (!p->swf)
                goto nomem;
            p->swf->format = tgt;
            p->swf->width = pw;
            p->swf->height = ph;
            if (av_frame_get_buffer(p->swf, 0) < 0) {
                av_frame_free(&p->swf);
                goto nomem;
            }
        }
        if (av_frame_make_writable(p->swf) < 0)
            goto nomem;
        pr_pad_planes(f, p->swf);
        src = p->swf;
    }

    hw = av_frame_alloc();
    if (!hw)
        goto nomem;
    ret = av_hwframe_get_buffer(p->up_frames, hw, 0);
    if (ret >= 0)
        ret = av_hwframe_transfer_data(hw, src, 0);
    if (ret < 0) {
        snprintf(err, err_size, "upload failed (%s)", av_err2str(ret));
        av_frame_free(&hw);
        return -1;
    }
    av_frame_free(&p->up_frame);
    p->up_frame = hw;
    in->tex = (ID3D11Texture2D *)hw->data[0];
    in->slice = (int)(intptr_t)hw->data[1];
    in->direct = 0;
    in->rgb = tgt == AV_PIX_FMT_BGRA;
    in->vis_x = in->vis_y = 0;
    in->vis_w = w;
    in->vis_h = h;
    in->tex_w = pw;
    in->tex_h = ph;
    return 0;
nomem:
    pr_set_err(err, err_size, "out of memory", S_OK);
    return -1;
}

static int pr_get_input(PrD3D11 *p, const AVFrame *f, PrInput *in, char *err, size_t err_size)
{
    memset(in, 0, sizeof(*in));
    if (pr_frame_ok(p->dev, f)) {
        D3D11_TEXTURE2D_DESC td;

        in->tex = (ID3D11Texture2D *)f->data[0];
        in->slice = (int)(intptr_t)f->data[1];
        in->tex->lpVtbl->GetDesc(in->tex, &td);
        in->tex_w = td.Width;
        in->tex_h = td.Height;
        in->direct = 1;
        /* decoders crop the right / bottom edge from width / height, the left / top edge stays in crop_left / crop_top */
        in->vis_x = (int)f->crop_left;
        in->vis_y = (int)f->crop_top;
        in->vis_w = FFMAX(f->width - (int)f->crop_left, 1);
        in->vis_h = FFMAX(f->height - (int)f->crop_top, 1);
        return 0;
    }
    if (f->hw_frames_ctx) {
        pr_set_err(err, err_size, "hardware frame of another device / format", S_OK);
        return -1;
    }
    return pr_upload(p, f, in, err, err_size);
}

static int pr_ensure_swapchain(PrD3D11 *p, char *err, size_t err_size)
{
    RECT rc;
    int w, h;

    if (!GetClientRect(p->host, &rc)) {
        pr_set_err(err, err_size, "GetClientRect failed", S_OK);
        return -1;
    }
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        if (p->sc)
            return 0;
        w = 2;
        h = 2;
    }
    if (!p->child) {
        if (pr_start_window(p, w, h) < 0) {
            snprintf(err, err_size, "CreateWindowEx(child) failed (%d)", p->wres);
            return -1;
        }
    }
    if (!p->sc) {
        DXGI_SWAP_CHAIN_DESC1 d;

        memset(&d, 0, sizeof(d));
        d.Width = w;
        d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 3;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        CHECK_HR(p->factory->lpVtbl->CreateSwapChainForHwnd(p->factory, (IUnknown *)p->dev, p->child, &d, NULL, NULL, &p->sc),
                 "CreateSwapChainForHwnd (flip model)");
        p->factory->lpVtbl->MakeWindowAssociation(p->factory, p->child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        p->sc_w = w;
        p->sc_h = h;
        return 0;
    }
    if (w != p->sc_w || h != p->sc_h) {
        PostMessageW(p->child, PR_WM_SIZE, (WPARAM)w, (LPARAM)h);     /* the window thread resizes it */
        CHECK_HR(p->sc->lpVtbl->ResizeBuffers(p->sc, 0, w, h, DXGI_FORMAT_UNKNOWN, 0), "IDXGISwapChain::ResizeBuffers");
        p->sc_w = w;
        p->sc_h = h;
    }
    return 0;
fail:
    return -1;
}

static int pr_blt(PrD3D11 *p, PrVP *v, ID3D11VideoProcessorOutputView *ov, const PrInput *in, const AVFrame *f,
                  const RECT *dst, const RECT *target, char *err, size_t err_size)
{
    ID3D11VideoProcessorInputView *iv;
    D3D11_VIDEO_PROCESSOR_STREAM st;
    RECT src;
    HRESULT hr;

    iv = pr_vp_input_view(p->vdev, v, in->tex, in->slice, err, err_size);
    if (!iv)
        return -1;
    src.left = in->vis_x;
    src.top = in->vis_y;
    src.right = in->vis_x + in->vis_w;
    src.bottom = in->vis_y + in->vis_h;
    pr_vp_setup(p->vctx, p->vctx1, v->vp, pr_input_colorspace(f, in->rgb), &src, dst, target);
    memset(&st, 0, sizeof(st));
    st.Enable = TRUE;
    st.pInputSurface = iv;
    hr = p->vctx->lpVtbl->VideoProcessorBlt(p->vctx, v->vp, ov, 0, 1, &st);
    if (FAILED(hr)) {
        pr_set_err(err, err_size, "VideoProcessorBlt", hr);
        return -1;
    }
    return 0;
}


static void pr_note_video_rect(PrD3D11 *p, const RECT *dst, int *video_width, int *video_height)
{
    p->overlay_video_rect.x = dst->left;
    p->overlay_video_rect.y = dst->top;
    p->overlay_video_rect.width = dst->right - dst->left;
    p->overlay_video_rect.height = dst->bottom - dst->top;
    *video_width = p->overlay_video_rect.width;
    *video_height = p->overlay_video_rect.height;
}

/* The overlays, the backdrop and the test capture on top of the rendered back buffer; cs and the device lock held. */
static void pr_compose(PrD3D11 *p, ID3D11Texture2D *bb, ID3D11RenderTargetView *rtv, const RECT *dst, char *err,
                       size_t err_size)
{
    if (pr_overlay_draw(p, rtv, dst, p->sc_w, p->sc_h, err, err_size) < 0) {
        InterlockedIncrement64(&p->overlay_draw_failures);
        av_log(NULL, AV_LOG_WARNING, "D3D11 bitmap overlay draw failed: %s\n", err);
    }
    (void)pr_acrylic_apply(p, bb, rtv, p->sc_w, p->sc_h, err, err_size);
    pr_overlay_capture_readback(p, bb, p->sc_w, p->sc_h);
}

/* Shows f (NULL: the held frame again) with the overlay layers (NULL: the cached ones). sc_cs and cs are entered here
 * unless locked; both are left before it returns, cs before Present. 0 shown, 1 nothing held to redraw, -1 failed with
 * err. */
static int pr_display(PrD3D11 *p, const AVFrame *f, const FFPOverlayLayer *layers, int locked, int *copies,
                      int *video_width, int *video_height, char *err, size_t err_size)
{
    PrInput in;
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11VideoProcessorOutputView *ov = NULL;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od;
    RECT dst, target;
    HRESULT hr;
    int ret = -1, copied = 0, shader = 0, l;
    int gpu_timer = -1, gpu_timer_ended = 0;

    if (!locked) {
        EnterCriticalSection(&p->sc_cs);
        EnterCriticalSection(&p->cs);
    }
    pr_last_hr = S_OK;
    *copies = 0;
    *video_width = *video_height = 0;
    if (f) {
        if (!p->held && !(p->held = av_frame_alloc())) {
            pr_set_err(err, err_size, "out of memory", S_OK);
            goto done;
        }
        av_frame_unref(p->held);
        if (av_frame_ref(p->held, f) < 0) {
            pr_set_err(err, err_size, "out of memory", S_OK);
            goto done;
        }
    } else if (!p->held || !p->held->buf[0]) {
        LeaveCriticalSection(&p->cs);
        LeaveCriticalSection(&p->sc_cs);
        return 1;
    }
    f = p->held;
    for (l = 0; layers && l < FFP_OVERLAY_LAYER_COUNT; l++) {
        if (pr_overlay_cache_update(p, &p->overlay[l], &layers[l], err, err_size) < 0)
            av_log(NULL, AV_LOG_WARNING, "D3D11 %s overlay update failed: %s\n",
                   l == FFP_OVERLAY_CAPTION ? "caption" : "superimpose", err);
    }
    err[0] = 0;
    if (p->fail_mode == PR_FAIL_DISPLAY) {
        p->fail_mode = PR_FAIL_NONE;
        pr_set_err(err, err_size, "forced failure (presenter test option)", S_OK);
        goto done;
    }
    if (p->fail_mode == PR_FAIL_DEVICE_LOST) {
        p->fail_mode = PR_FAIL_NONE;
        p->forced_lost = 1;
        pr_set_err(err, err_size, "forced device loss (presenter test option)", S_OK);
        goto done;
    }
    if (pr_ensure_swapchain(p, err, err_size) < 0)
        goto done;
    if (p->hwctx && InterlockedCompareExchange((volatile LONG *)&p->hwctx->decoder_bind_fallbacks, 0, 0))
        pr_record_fallback(p, "decoder_pool_no_srv");     /* a decoder pool was created again without SHADER_RESOURCE */
    gpu_timer = pr_frame_gpu_timer_begin(p);
    if (pr_get_input(p, f, &in, err, err_size) < 0)
        goto done;
    shader = pr_pick_route(p, &in, 1);
    if (shader == PR_ROUTE_PICK_DOWNLOAD) {
        if (pr_download_input(p, f, &in, err, err_size) < 0)
            goto done;
        shader = PR_ROUTE_PICK_VP;
    }
    dst = pr_letterbox(p->sc_w, p->sc_h, in.vis_w, in.vis_h, f->sample_aspect_ratio);
    pr_note_video_rect(p, &dst, video_width, video_height);
    if (shader) {
        int p010_no_vp = !p->lim.p010 && pr_input_p010(&in);

        CHECK_HR(p->sc->lpVtbl->GetBuffer(p->sc, 0, &pr_IID_ID3D11Texture2D, (void **)&bb), "shader back buffer");
        CHECK_HR(p->dev->lpVtbl->CreateRenderTargetView(p->dev, (ID3D11Resource *)bb, NULL, &rtv), "shader render target");
        pr_lock_dev(p);
        ret = pr_d3d11_shader_render(p, &in, f, rtv, p->sc_w, p->sc_h, &dst, &copied, err, err_size);
        if (ret >= 0)
            pr_compose(p, bb, rtv, &dst, err, err_size);
        pr_unlock_dev(p);
        if (ret < 0 && p->shader_failed && p010_no_vp) {
            /* no shader after all: P010 without video processor P010 input is downloaded and converted */
            PR_REL(rtv);
            PR_REL(bb);
            if (pr_download_input(p, f, &in, err, err_size) < 0)
                goto done;
            shader = PR_ROUTE_PICK_VP;
        } else {
            if (ret < 0)
                goto done;
            pr_frame_gpu_timer_end(p, gpu_timer, 0);
            gpu_timer_ended = 1;
            if (p010_no_vp)
                pr_record_fallback(p, "p010_vp_unsupported:shader");
            InterlockedExchange(&p->route, 2);
            InterlockedIncrement(&p->frames_shader);
            if (copied)
                InterlockedIncrement(&p->frames_d3d11_copied);
            *copies = (copied || !in.direct) ? 1 : 0;
            PR_REL(rtv);
            PR_REL(bb);
            goto present;
        }
    }

    CHECK_HR(p->sc->lpVtbl->GetBuffer(p->sc, 0, &pr_IID_ID3D11Texture2D, (void **)&bb), "IDXGISwapChain::GetBuffer");
    if (pr_vp_ensure(p->vdev, &p->disp, in.tex_w, in.tex_h, p->sc_w, p->sc_h, err, err_size) < 0)
        goto done;
    memset(&od, 0, sizeof(od));
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    CHECK_HR(p->vdev->lpVtbl->CreateVideoProcessorOutputView(p->vdev, (ID3D11Resource *)bb, p->disp.en, &od, &ov),
             "CreateVideoProcessorOutputView");
    target.left = target.top = 0;
    target.right = p->sc_w;
    target.bottom = p->sc_h;
    pr_lock_dev(p);
    ret = pr_blt(p, &p->disp, ov, &in, f, &dst, &target, err, err_size);
    if (ret >= 0) {
        PR_REL(ov);
        hr = p->dev->lpVtbl->CreateRenderTargetView(p->dev, (ID3D11Resource *)bb, NULL, &rtv);
        if (SUCCEEDED(hr)) {
            pr_compose(p, bb, rtv, &dst, err, err_size);
        } else {
            p->overlay_frame_rects = 0;
            InterlockedIncrement64(&p->overlay_draw_failures);
            pr_overlay_capture_readback(p, bb, p->sc_w, p->sc_h);
        }
    }
    pr_unlock_dev(p);
    if (ret < 0)
        goto done;
    pr_frame_gpu_timer_end(p, gpu_timer, 0);
    gpu_timer_ended = 1;
    InterlockedExchange(&p->route, 1);
    *copies = in.direct ? 0 : 1;
    PR_REL(ov);
    PR_REL(rtv);   /* a back-buffer view left alive makes the next ResizeBuffers fail (DXGI_ERROR_INVALID_CALL) */
    PR_REL(bb);

present:
    LeaveCriticalSection(&p->cs);
    hr = p->sc->lpVtbl->Present(p->sc, 1, 0);
    LeaveCriticalSection(&p->sc_cs);
    if (FAILED(hr)) {
        pr_set_err(err, err_size, hr == DXGI_ERROR_DEVICE_REMOVED ? "device removed (Present)" :
                                  hr == DXGI_ERROR_DEVICE_RESET ? "device reset (Present)" : "Present", hr);
        return -1;
    }
    if (!p->shown) {
        PostMessageW(p->child, PR_WM_SHOW, 0, 0);
        p->shown = 1;
    }
    return 0;
fail:
done:
    if (gpu_timer >= 0 && !gpu_timer_ended)
        pr_frame_gpu_timer_end(p, gpu_timer, 1);
    PR_REL(rtv);
    PR_REL(ov);
    PR_REL(bb);
    LeaveCriticalSection(&p->cs);
    LeaveCriticalSection(&p->sc_cs);
    return ret > 0 ? 1 : -1;
}

/* the failure of the calling thread's last present / readback: kept for pr_d3d11_device_lost on other threads */
static int pr_note_failure(PrD3D11 *p, unsigned *reason)
{
    InterlockedExchange(&p->failed_hr, (LONG)pr_last_hr);
    *reason = 0;
    return pr_d3d11_device_lost(p, reason);
}

int pr_d3d11_present(PrD3D11 *p, const AVFrame *f, const FFPOverlayLayer *layers, int *copies, int *video_width,
                     int *video_height, char *err, size_t err_size, int *lost, unsigned *reason)
{
    int ret = pr_display(p, f, layers, 0, copies, video_width, video_height, err, err_size);

    *lost = 0;
    *reason = 0;
    if (ret < 0)
        *lost = pr_note_failure(p, reason);
    else
        pr_dbg_drain(p->dbg, "present", 1);
    return ret;
}

static void pr_redraw_held(PrD3D11 *p, int host_resized)
{
    char err[256] = "";
    int copies, video_width, video_height, ret, lost = 0;
    unsigned reason = 0;

    /* pr_d3d11_destroy holds cs while it waits for this thread */
    while (!TryEnterCriticalSection(&p->sc_cs)) {
        if (InterlockedCompareExchange(&p->closing, 0, 0))
            return;
        Sleep(1);
    }
    while (!TryEnterCriticalSection(&p->cs)) {
        if (InterlockedCompareExchange(&p->closing, 0, 0)) {
            LeaveCriticalSection(&p->sc_cs);
            return;
        }
        Sleep(1);
    }
    if (InterlockedCompareExchange(&p->closing, 0, 0) || !p->sc) {
        LeaveCriticalSection(&p->cs);
        LeaveCriticalSection(&p->sc_cs);
        return;
    }
    if (host_resized) {
        RECT rc;
        if (GetClientRect(p->host, &rc) && rc.right > 0 && rc.bottom > 0)
            SetWindowPos(p->child, NULL, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    }
    ret = pr_display(p, NULL, NULL, 1, &copies, &video_width, &video_height, err, sizeof(err));
    if (ret == 1)
        return;
    if (ret < 0)
        lost = pr_note_failure(p, &reason);
    if (p->redraw)
        p->redraw(p->redraw_opaque, ret, video_width, video_height, err, lost, reason);
}

void pr_d3d11_release_held(PrD3D11 *p)
{
    EnterCriticalSection(&p->cs);
    av_frame_free(&p->held);
    LeaveCriticalSection(&p->cs);
}

void pr_d3d11_clear(PrD3D11 *p)
{
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    static const FLOAT black[4] = { 0, 0, 0, 1 };

    EnterCriticalSection(&p->sc_cs);
    EnterCriticalSection(&p->cs);
    av_frame_free(&p->held);
    if (p->sc && SUCCEEDED(p->sc->lpVtbl->GetBuffer(p->sc, 0, &pr_IID_ID3D11Texture2D, (void **)&bb))) {
        if (SUCCEEDED(p->dev->lpVtbl->CreateRenderTargetView(p->dev, (ID3D11Resource *)bb, NULL, &rtv))) {
            pr_lock_dev(p);
            p->ctx->lpVtbl->ClearRenderTargetView(p->ctx, rtv, black);
            pr_unlock_dev(p);
            PR_REL(rtv);
        }
        PR_REL(bb);
        LeaveCriticalSection(&p->cs);
        p->sc->lpVtbl->Present(p->sc, 0, 0);
        LeaveCriticalSection(&p->sc_cs);
        return;
    }
    LeaveCriticalSection(&p->cs);
    LeaveCriticalSection(&p->sc_cs);
}

int pr_d3d11_readback(PrD3D11 *p, const AVFrame *f, uint8_t *bgra, int stride, int dw, int dh, char *err,
                      size_t err_size)
{
    PrInput in;
    ID3D11Texture2D *tex = NULL, *stg = NULL;
    ID3D11VideoProcessorOutputView *ov = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od;
    D3D11_MAPPED_SUBRESOURCE m;
    RECT dst;
    int y, x, ret = -1, mapped = 0, route, copied = 0;

    if (!f || !bgra || dw <= 0 || dh <= 0 || stride < dw * 4 || dw > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        dh > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        pr_set_err(err, err_size, "invalid readback arguments", E_INVALIDARG);
        return -1;
    }
    dst.left = dst.top = 0;
    dst.right = dw;
    dst.bottom = dh;
    EnterCriticalSection(&p->cs);
    pr_last_hr = S_OK;
    tex = pr_make_tex(p->dev, dw, dh, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, NULL, 0);
    stg = pr_make_tex(p->dev, dw, dh, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, NULL, 0);
    if (!tex || !stg) {
        pr_set_err(err, err_size, "readback texture creation failed", S_OK);
        goto done;
    }
    if (pr_get_input(p, f, &in, err, err_size) < 0)
        goto done;
    route = pr_pick_route(p, &in, 0);
    if (route == PR_ROUTE_PICK_DOWNLOAD) {
        if (pr_download_input(p, f, &in, err, err_size) < 0)
            goto done;
        route = PR_ROUTE_PICK_VP;
    }
    if (route == PR_ROUTE_PICK_SHADER) {
        CHECK_HR(p->dev->lpVtbl->CreateRenderTargetView(p->dev, (ID3D11Resource *)tex, NULL, &rtv), "readback target");
        pr_lock_dev(p);
        ret = pr_d3d11_shader_render(p, &in, f, rtv, dw, dh, &dst, &copied, err, err_size);
        pr_unlock_dev(p);
    } else {
        if (pr_vp_ensure(p->vdev, &p->snap, in.tex_w, in.tex_h, dw, dh, err, err_size) < 0)
            goto done;
        memset(&od, 0, sizeof(od));
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        CHECK_HR(p->vdev->lpVtbl->CreateVideoProcessorOutputView(p->vdev, (ID3D11Resource *)tex, p->snap.en, &od, &ov),
                 "CreateVideoProcessorOutputView");
        pr_lock_dev(p);
        ret = pr_blt(p, &p->snap, ov, &in, f, &dst, &dst, err, err_size);
        pr_unlock_dev(p);
    }
    if (ret < 0)
        goto done;
    ret = -1;
    pr_lock_dev(p);
    p->ctx->lpVtbl->CopyResource(p->ctx, (ID3D11Resource *)stg, (ID3D11Resource *)tex);
    mapped = SUCCEEDED(p->ctx->lpVtbl->Map(p->ctx, (ID3D11Resource *)stg, 0, D3D11_MAP_READ, 0, &m));
    if (mapped) {
        for (y = 0; y < dh; y++) {
            const uint8_t *s = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
            uint8_t *d = bgra + (size_t)y * stride;
            memcpy(d, s, (size_t)dw * 4);
            for (x = 0; x < dw; x++)
                d[x * 4 + 3] = 255;
        }
        p->ctx->lpVtbl->Unmap(p->ctx, (ID3D11Resource *)stg, 0);
        ret = 0;
    }
    pr_unlock_dev(p);
    if (!mapped)
        pr_set_err(err, err_size, "readback Map failed", S_OK);
fail:
done:
    PR_REL(rtv);
    PR_REL(ov);
    PR_REL(stg);
    PR_REL(tex);
    if (ret < 0)
        InterlockedExchange(&p->failed_hr, (LONG)pr_last_hr);
    LeaveCriticalSection(&p->cs);
    return ret;
}

void pr_d3d11_stats_json(PrD3D11 *p, AVBPrint *bp)
{
    static const char *const renderers[] = { "d3d11:videoprocessor", "d3d11:videoprocessor", "d3d11:shader" };
    uint32_t present_count = 0, present_refresh_count = 0, sync_refresh_count = 0;
    uint64_t sync_qpc_time = 0, sync_gpu_time = 0, gpu_total_us = 0, gpu_frames = 0, gpu_last_us = 0;
    uint64_t readback_seq, readback_rects;
    char luid[24];
    int dxgi_valid, regions, i;
    LONG route = InterlockedCompareExchange(&p->route, 0, 0);

    EnterCriticalSection(&p->cs);
    pr_lock_dev(p);
    pr_acrylic_timer_poll(p);
    dxgi_valid = pr_frame_stats(p, &present_count, &present_refresh_count, &sync_refresh_count, &sync_qpc_time,
                                &sync_gpu_time, &gpu_total_us, &gpu_frames, &gpu_last_us);
    pr_unlock_dev(p);
    regions = p->acrylic_region_count;
    readback_seq = (uint64_t)p->overlay_readback_seq;
    readback_rects = (uint64_t)p->overlay_readback_rects;
    LeaveCriticalSection(&p->cs);

    av_bprintf(bp, "{\"renderer\":\"%s\",\"route\":\"%s\"", renderers[route], route == 2 ? "shader" : "vp");
    av_bprintf(bp, ",\"frames_shader\":%ld,\"frames_d3d11_copied\":%ld",
               (long)InterlockedCompareExchange(&p->frames_shader, 0, 0),
               (long)InterlockedCompareExchange(&p->frames_d3d11_copied, 0, 0));
    pr_luid_text(luid, sizeof(luid), p->adapter.luid_high, p->adapter.luid_low);
    av_bprintf(bp, ",\"adapter\":{\"name\":");
    pr_json_str(bp, p->adapter.name);
    av_bprintf(bp, ",\"vendor_id\":%u,\"luid\":\"%s\"},\"debug_layer\":%s,\"debug_messages\":%ld,\"fallbacks\":[",
               p->adapter.vendor_id, luid, p->dbg ? "true" : "false",
               p->dbg ? (long)InterlockedCompareExchange(&p->dbg->messages, 0, 0) : 0L);
    EnterCriticalSection(&p->fb_cs);
    for (i = 0; i < p->nb_fallbacks; i++) {
        av_bprintf(bp, "%s", i ? "," : "");
        pr_json_str(bp, p->fallbacks[i]);
    }
    LeaveCriticalSection(&p->fb_cs);
    av_bprintf(bp, "],\"overlay\":{\"cue_uploads\":%lld,\"rects_uploaded\":%lld,\"cues_shown\":%lld,\"rects_drawn\":%lld,"
               "\"draw_failures\":%lld,\"readback_failures\":%lld,\"readback_seq\":%llu,\"readback_rects\":%llu}",
               (long long)InterlockedCompareExchange64(&p->overlay_cue_uploads, 0, 0),
               (long long)InterlockedCompareExchange64(&p->overlay_rects_uploaded, 0, 0),
               (long long)InterlockedCompareExchange64(&p->overlay_cues_shown, 0, 0),
               (long long)InterlockedCompareExchange64(&p->overlay_rects_drawn, 0, 0),
               (long long)InterlockedCompareExchange64(&p->overlay_draw_failures, 0, 0),
               (long long)InterlockedCompareExchange64(&p->overlay_readback_failures, 0, 0),
               (unsigned long long)readback_seq, (unsigned long long)readback_rects);
    av_bprintf(bp, ",\"acrylic\":{\"regions\":%d,\"frames_blurred\":%lld,\"failures\":%lld,\"blur_ms\":%.3f}", regions,
               (long long)InterlockedCompareExchange64(&p->acrylic_frames_blurred, 0, 0),
               (long long)InterlockedCompareExchange64(&p->acrylic_failures, 0, 0),
               InterlockedCompareExchange64(&p->acrylic_last_blur_us, 0, 0) / 1000.0);
    av_bprintf(bp, ",\"frame_stats\":{\"dxgi_valid\":%s,\"dxgi_present_count\":%u,\"dxgi_present_refresh_count\":%u,"
               "\"dxgi_sync_refresh_count\":%u,\"dxgi_sync_qpc_time\":%llu,\"dxgi_sync_gpu_time\":%llu,"
               "\"gpu_total_us\":%llu,\"gpu_frames\":%llu,\"gpu_last_us\":%llu}}",
               dxgi_valid ? "true" : "false", present_count, present_refresh_count, sync_refresh_count,
               (unsigned long long)sync_qpc_time, (unsigned long long)sync_gpu_time, (unsigned long long)gpu_total_us,
               (unsigned long long)gpu_frames, (unsigned long long)gpu_last_us);
}
