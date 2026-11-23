// URTI: "AAA game" sandbox to test the injector without launching a real game.
//
// Same rendering structure as a recent game (and as Red Dead Redemption 2 as the pass survey
// showed it):
//  - deferred G-buffer at render resolution (2/3 of the screen, like DLSS): sRGB albedo, normals,
//    material (smoothness in channel 1 of target 2), misc, R16G16 motion vectors in pixels
//    (from the current frame to the previous one); sub-pixel jitter every frame;
//  - reversed infinite depth (D32S8, cleared to 0); a second half-resolution depth;
//  - atlas of 4 shadow cascades 2048×8192 in D16, each in its viewport, following the camera
//    in texel steps; full-screen lighting reading the atlas (3×3 filtering);
//  - temporal anti-aliasing (TAA) and upscaling to the screen (history reprojected by the motion
//    vectors, clamped by the neighbourhood), HDR10 output (PQ, R10G10B10A2) like RDR2: the presented image
//    is stable from one frame to the next, as in a real game;
//  - optional frame generation: one present in two without any draw (copy only).
// A hard scene for RT: a street, buildings, very thin poles and cables, trees whose branches
// move in the wind (foliage as alpha-tested cut-out cards, as in games: the geometry is a
// rectangle, the texture cuts it out), carts and walking passers-by, puddles (smooth ground in places), barrels through
// ExecuteIndirect. Scripted camera, the same on every launch (walking, looking around, fast U-turns).
//
// Settings (environment variables): URTI_FRAMES (number of frames, default 600), URTI_SHOW=1 (visible
// window), URTI_SANDBOX_CAMERA=walk|fixed|orbit|building|tree|street|occlusion (the last four:
// fixed shots of the tests, see tools/reshade/urti_sandbox_suite.sh), URTI_SANDBOX_FG=1 (frame generation),
// URTI_SANDBOX_SDR=1 (sRGB output instead of HDR10), URTI_SANDBOX_START=seconds (start of the scenario),
// URTI_SANDBOX_UI=0 (without UI: minimap, bars, a menu opening 2 s every 4 s, drawn into
// the screen image after the scene, like a game), URTI_SANDBOX_LEAVES=blocks (leaves as small solid blocks instead of cut-out cards),
// URTI_SANDBOX_JITTER=0 (no sub-pixel jitter), URTI_SANDBOX_WIND=factor (0: no wind), URTI_SANDBOX_MOVERS=0 (carts and passers-by still),
// URTI_SANDBOX_WET=0..1 (share of the ground covered with water; default: scattered puddles), URTI_SANDBOX_TRUTH=1 (ground
// truth: the floor reflects the exact scene, rendered mirrored, blended with the same rule as URTI; a
// launch without URTI in this mode is the image RT reflections should give), URTI_SANDBOX_FPS=maximum frame rate (default 90, 0: no limit;
// a native launch and a launch with RT thus reach the same image at the same moment, after
// ReShade has removed its welcome banner), URTI_SANDBOX_GAMESHADOW=partial (the "game's" shadow map forgets
// the poles, posts, the wall, the carts and the passers-by: URTI must render those shadows as the game would
// have; tools/reshade/urti_sandbox_shadows.sh).

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::fprintf(stderr, "%s : 0x%08lx\n", #x, (unsigned long)hr_); std::exit(1); } } while (0)

namespace
{
constexpr UINT kWidth = 1600, kHeight = 900, kBuffers = 3;
constexpr UINT kRenderW = kWidth * 2 / 3, kRenderH = kHeight * 2 / 3;
constexpr UINT kAtlas = 2048, kCascades = 4;
constexpr UINT kMaxObjects = 512;
constexpr float kPi = 3.14159265f;

// ------------------------------------------------------------------------------------------------
// maths (row-vector convention: v * a * b)
// ------------------------------------------------------------------------------------------------

struct Mat4
{
    float m[16];
};

Mat4 Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
    return r;
}

Mat4 Identity()
{
    Mat4 r = {};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Scale(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[0] = x, r.m[5] = y, r.m[10] = z;
    return r;
}

Mat4 Translate(float x, float y, float z)
{
    Mat4 r = Identity();
    r.m[12] = x, r.m[13] = y, r.m[14] = z;
    return r;
}

Mat4 RotateY(float a)
{
    Mat4 r = Identity();
    r.m[0] = std::cos(a), r.m[2] = -std::sin(a), r.m[8] = std::sin(a), r.m[10] = std::cos(a);
    return r;
}

Mat4 RotateX(float a)
{
    Mat4 r = Identity();
    r.m[5] = std::cos(a), r.m[6] = std::sin(a), r.m[9] = -std::sin(a), r.m[10] = std::cos(a);
    return r;
}

Mat4 RotateZ(float a)
{
    Mat4 r = Identity();
    r.m[0] = std::cos(a), r.m[1] = std::sin(a), r.m[4] = -std::sin(a), r.m[5] = std::cos(a);
    return r;
}

Mat4 LookAtLH(const float eye[3], const float at[3])
{
    float z[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    float l = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (float& v : z)
        v /= l;
    float x[3] = { z[2], 0.0f, -z[0] };
    l = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (l < 1e-5f)
        x[0] = 1, x[2] = 0, l = 1;
    x[0] /= l, x[2] /= l;
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };
    Mat4 r = Identity();
    for (int i = 0; i < 3; i++)
        r.m[i * 4 + 0] = x[i], r.m[i * 4 + 1] = y[i], r.m[i * 4 + 2] = z[i];
    r.m[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    r.m[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    r.m[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    return r;
}

Mat4 OrthoLH(float w, float h, float zn, float zf)
{
    Mat4 r = {};
    r.m[0] = 2.0f / w, r.m[5] = 2.0f / h, r.m[10] = 1.0f / (zf - zn), r.m[14] = -zn / (zf - zn), r.m[15] = 1.0f;
    return r;
}

// perspective with reversed depth and far plane at infinity: depth = near / z
Mat4 ReversedInfiniteLH(float fovY, float aspect, float zn)
{
    const float h = 1.0f / std::tan(fovY * 0.5f), w = h / aspect;
    Mat4 r = {};
    r.m[0] = w, r.m[5] = h, r.m[11] = 1.0f, r.m[14] = zn;
    return r;
}

Mat4 Inverse(const Mat4& a)
{
    // Gauss-Jordan 4×4
    float m[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++)
            m[i][j] = j < 4 ? a.m[i * 4 + j] : (j - 4 == i ? 1.0f : 0.0f);
    for (int c = 0; c < 4; c++)
    {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c]))
                p = r;
        for (int j = 0; j < 8; j++)
            std::swap(m[c][j], m[p][j]);
        const float d = m[c][c];
        for (int j = 0; j < 8; j++)
            m[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c)
            {
                const float f = m[r][c];
                for (int j = 0; j < 8; j++)
                    m[r][j] -= f * m[c][j];
            }
    }
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i * 4 + j] = m[i][j + 4];
    return r;
}

float Halton(int i, int base)
{
    float f = 1, r = 0;
    while (i > 0)
    {
        f /= base;
        r += f * (i % base);
        i /= base;
    }
    return r;
}

// ------------------------------------------------------------------------------------------------
// shaders (a single text, several entry points)
// ------------------------------------------------------------------------------------------------

const char* kShaders = R"(
cbuffer Frame : register(b0)
{
    row_major float4x4 viewProj;       // with sub-pixel jitter
    row_major float4x4 viewProjNJ;     // without jitter
    row_major float4x4 prevViewProjNJ; // previous frame, without jitter
    row_major float4x4 invViewProj;    // inverse of viewProj
    row_major float4x4 lightViewProj[4];
    float4 cameraPos;  // w : temps
    float4 sunDir;     // toward the sun; w: previous frame's time
    float4 renderSize; // width, height, 1 / width, 1 / height
    float4 splits;     // end distances of the cascades
    float4 camForward; // w : vent
    float4 output;     // x: 1 HDR10, 0 sRGB; y: exposure; zw: sub-pixel jitter (NDC)
    float4 misc;       // x: water on the ground (< 0: scattered puddles); y: 1 = TAA history to forget;
                       // z: 1 = exact floor reflections (ground truth)
};
cbuffer Object : register(b1)
{
    row_major float4x4 world;
    row_major float4x4 prevWorld;
    float4 color;  // albedo
    float4 params; // x: wind (multiplier), y: 1 ground with puddles, z: reserved, w: metal
};
cbuffer PassConstants : register(b2)
{
    uint cascade;
    uint3 passPad;
    float4 uiRect;  // UI: rectangle (NDC: left, top, right, bottom)
    float4 uiColor; // colour already encoded for the screen, alpha
};

Texture2D<float4> gAlbedo : register(t0);
Texture2D<float4> gNormal : register(t1);
Texture2D<float4> gMaterial : register(t2);
Texture2D<float> gDepth : register(t3);
Texture2D<float> gAtlas : register(t4);
Texture2D<float4> gMirror : register(t5); // scene seen in the floor's mirror (ground truth)
SamplerState linearClamp : register(s0);
SamplerComparisonState shadowCmp : register(s1);

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float4 col : COLOR;  // alpha: smoothness
    float wind : WIND;   // the vertex's wind weight
    float2 uv : TEXCOORD; // foliage card: coordinates in the cut-out texture; < 0: solid surface
#ifdef INSTANCED
    float4 inst : INSTOFFSET;
#endif
};

// swaying in the wind, according to each vertex's weight (tree foot and cable ends still); the
// phase depends on the position: leaves and cables do not all move together
float3 Wind(float3 wp, float weight, float t)
{
    const float amount = weight * params.x * camForward.w;
    if (amount <= 0)
        return wp;
    const float phase = wp.x * 0.37 + wp.z * 0.23 + wp.y * 0.5;
    return wp + float3(sin(t * 2.1 + phase), 0.3 * sin(t * 3.7 + phase * 1.7), cos(t * 1.7 + phase)) * amount;
}

float3 WorldPos(VSIn i, row_major float4x4 m, float t)
{
    float3 wp = mul(float4(i.pos, 1), m).xyz;
#ifdef INSTANCED
    wp += i.inst.xyz;
#endif
    return Wind(wp, i.wind, t);
}

struct GVSOut
{
    float4 pos : SV_Position;
    float3 nrm : NORMAL;
    float4 col : COLOR;
    float2 uv : TEXCOORD;
    float3 wpos : WORLDPOS;
    float4 cur : CURCLIP;
    float4 prev : PREVCLIP;
};

GVSOut vsGbuffer(VSIn i)
{
    GVSOut o;
    const float3 wp = WorldPos(i, world, cameraPos.w);
    const float3 pwp = WorldPos(i, prevWorld, sunDir.w);
    o.pos = mul(float4(wp, 1), viewProj);
    o.cur = mul(float4(wp, 1), viewProjNJ);
    o.prev = mul(float4(pwp, 1), prevViewProjNJ);
    o.nrm = normalize(mul(float4(i.nrm, 0), world).xyz);
    o.col = i.col;
    o.uv = i.uv;
    o.wpos = wp;
    return o;
}

float Hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }

// "texture" of a foliage card: a cluster of leaves (ellipses); elsewhere, transparent
// (alpha test: the pixel is discarded, the shadow map and the screen see through it)
bool LeafCut(float2 uv)
{
    if (uv.x < 0)
        return false;
    float a = -1;
    for (int k = 0; k < 12; k++)
    {
        const float2 c = float2(Hash(float2(k, 1.7)), Hash(float2(k, 9.1))) * 0.7 + 0.15;
        const float ang = Hash(float2(k, 4.2)) * 6.2831853;
        const float2 d = uv - c;
        const float2 r = float2(d.x * cos(ang) - d.y * sin(ang), d.x * sin(ang) + d.y * cos(ang));
        a = max(a, 1 - length(r / float2(0.2, 0.08)));
    }
    return a <= 0;
}
float Noise(float2 p)
{
    const float2 i = floor(p), f = frac(p), u = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

struct GOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 material : SV_Target2;
    float4 misc : SV_Target3;
    float2 velocity : SV_Target4;
};

GOut psGbuffer(GVSOut i)
{
    if (LeafCut(i.uv))
        discard;
    GOut o;
    float3 albedo = color.rgb * i.col.rgb;
    float smooth = i.col.a;
    if (params.y > 0.5)
    {
        // ground: cobblestones and puddles (smooth as a mirror, darker)
        const float n = Noise(i.wpos.xz * 0.18) * 0.7 + Noise(i.wpos.xz * 0.9) * 0.3;
        const float puddle = misc.x < 0 ? smoothstep(0.58, 0.66, n) : smoothstep(1.02 - misc.x * 1.1, 1.08 - misc.x * 1.1, n);
        const float cobble = 0.85 + 0.15 * Noise(i.wpos.xz * 3.0);
        albedo *= lerp(cobble, 0.45, puddle);
        smooth = lerp(0.25, 0.97, puddle);
    }
    o.albedo = float4(albedo, 1);
    o.normal = float4(normalize(i.nrm) * 0.5 + 0.5, 1);
    o.material = float4(0.5, smooth, params.w, params.y > 0.5 ? 0.5 : 1.0); // alpha 0,5 : sol
    o.misc = float4(0, 0, 0, 1);
    const float2 curNdc = i.cur.xy / i.cur.w, prevNdc = i.prev.xy / i.prev.w;
    o.velocity = (prevNdc - curNdc) * float2(0.5, -0.5) * renderSize.xy; // pixels, toward the previous frame
    return o;
}

struct SVSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
SVSOut vsShadow(VSIn i)
{
    SVSOut o;
    o.pos = mul(float4(WorldPos(i, world, cameraPos.w), 1), lightViewProj[cascade]);
    o.uv = i.uv;
    return o;
}

// shadow map: foliage is cut out as on screen
void psShadow(SVSOut i)
{
    if (LeafCut(i.uv))
        discard;
}

struct FullOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };
FullOut vsFull(uint id : SV_VertexID)
{
    FullOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float ShadowAt(float3 p, float ndl)
{
    const float viewDepth = dot(p - cameraPos.xyz, camForward.xyz);
    uint k = viewDepth < splits.x ? 0 : viewDepth < splits.y ? 1 : viewDepth < splits.z ? 2 : 3;
    if (viewDepth > splits.w)
        return 1;
    const float4 lp = mul(float4(p, 1), lightViewProj[k]);
    const float2 uv = float2(lp.x * 0.5 + 0.5, (k + 0.5 - lp.y * 0.5) / 4.0);
    const float bias = 0.0015 * (1 + k) * (1.5 - ndl);
    float s = 0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            s += gAtlas.SampleCmpLevelZero(shadowCmp, uv + float2(x, y) * float2(1.0 / 2048.0, 1.0 / 8192.0), lp.z - bias);
    return s / 9.0;
}

float3 Sky(float3 dir)
{
    const float h = saturate(dir.y);
    float3 sky = lerp(float3(0.75, 0.82, 0.9), float3(0.25, 0.45, 0.85), pow(h, 0.5)) * 1.4;
    sky += float3(6, 5.4, 4.5) * pow(saturate(dot(dir, sunDir.xyz)), 600); // disque solaire
    return sky;
}

// direct and ambient lighting of a point (without reflection)
float3 Shade(float3 albedo, float3 n, float3 p)
{
    const float ndl = saturate(dot(n, sunDir.xyz));
    const float shadow = ndl > 0 ? ShadowAt(p, ndl) : 0;
    const float3 sunColor = float3(3.2, 2.9, 2.5);
    const float3 ambient = lerp(float3(0.18, 0.17, 0.16), float3(0.35, 0.42, 0.55), n.y * 0.5 + 0.5);
    return albedo * (sunColor * ndl * shadow + ambient);
}

// ground truth: the scene drawn mirrored under the floor (y -> -y), lit at its true place
GVSOut vsMirror(VSIn i)
{
    GVSOut o;
    const float3 wp = WorldPos(i, world, cameraPos.w);
    o.pos = mul(float4(wp.x, -wp.y, wp.z, 1), viewProj);
    o.cur = o.prev = o.pos;
    o.nrm = normalize(mul(float4(i.nrm, 0), world).xyz);
    o.col = i.col;
    o.uv = i.uv;
    o.wpos = wp;
    return o;
}

float4 psMirror(GVSOut i) : SV_Target
{
    if (i.wpos.y < -0.01 || LeafCut(i.uv))
        discard;
    return float4(Shade(color.rgb * i.col.rgb, normalize(i.nrm), i.wpos), 1);
}

float4 psLighting(FullOut i) : SV_Target
{
    const int2 px = int2(i.pos.xy);
    const float d = gDepth.Load(int3(px, 0));
    const float2 ndc = float2(i.uv.x * 2 - 1, 1 - i.uv.y * 2);
    const float4 far4 = mul(float4(ndc, 1e-6, 1), invViewProj);
    const float3 viewDir = normalize(far4.xyz / far4.w - cameraPos.xyz);
    if (d <= 0)
        return float4(Sky(viewDir), 1);
    const float4 wp4 = mul(float4(ndc, d, 1), invViewProj);
    const float3 p = wp4.xyz / wp4.w;
    const float3 albedo = gAlbedo.Load(int3(px, 0)).rgb;
    const float3 n = normalize(gNormal.Load(int3(px, 0)).xyz * 2 - 1);
    const float4 mat = gMaterial.Load(int3(px, 0));
    float3 c = Shade(albedo, n, p);
    // the "game's" reflection (sky only, like a game without RT): Fresnel according to smoothness
    const float3 r = reflect(viewDir, n);
    const float fres = 0.04 + 0.96 * pow(1 - saturate(dot(-viewDir, n)), 5);
    c = lerp(c, Sky(r) * 0.8, fres * mat.y * mat.y);
    if (misc.z > 0.5 && mat.w < 0.75)
    {
        // ground truth: exact floor reflection, blended the way URTI does it (Fresnel f0 = 0.04, smoothness above
        // 0.35, wet floor 0.9; colour capped at 4)
        const float sm = max(mat.y, 0.9 * saturate((n.y - 0.55) / 0.3));
        const float w = (0.04 + 0.96 * pow(1 - saturate(dot(-viewDir, n)), 5)) * saturate((sm - 0.35) / 0.65);
        const float4 m = gMirror.Load(int3(px, 0));
        float3 rc = m.a > 0 ? m.rgb : Sky(r);
        const float lum = dot(rc, float3(0.2126, 0.7152, 0.0722));
        if (lum > 4.0)
            rc *= 4.0 / lum;
        c = lerp(c, rc, w);
    }
    return float4(c, 1);
}

Texture2D<float4> gLit : register(t0);
Texture2D<float2> gVelocity : register(t1);
Texture2D<float4> gHistory : register(t2);

// TAA: current image (rid of the sub-pixel jitter) blended with the reprojected history, clamped by
// the current image's 3×3 neighbourhood; output at screen resolution
float4 psTaa(FullOut i) : SV_Target
{
    const float2 rs = renderSize.xy;
    const float2 uvCur = i.uv + float2(output.z, -output.w) * 0.5;
    const float3 cur = gLit.SampleLevel(linearClamp, uvCur, 0).rgb;
    const int2 c = int2(uvCur * rs);
    float3 lo = cur, hi = cur;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            const float3 n = gLit.Load(int3(clamp(c + int2(x, y), int2(0, 0), int2(rs) - 1), 0)).rgb;
            lo = min(lo, n), hi = max(hi, n);
        }
    const float2 vel = gVelocity.Load(int3(clamp(int2(i.uv * rs), int2(0, 0), int2(rs) - 1), 0));
    const float2 prevUv = i.uv + vel / rs;
    if (misc.y > 0.5 || any(prevUv < 0) || any(prevUv > 1))
        return float4(cur, 1);
    const float3 hist = clamp(gHistory.SampleLevel(linearClamp, prevUv, 0).rgb, lo, hi);
    return float4(lerp(hist, cur, 0.1), 1);
}
float3 NitsToPq(float3 l)
{
    const float3 y = pow(saturate(l / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// the game's UI: a solid rectangle, blended by its alpha, drawn into the screen image
float4 vsUi(uint id : SV_VertexID) : SV_Position
{
    const float2 c = float2((id & 1) ? uiRect.z : uiRect.x, (id & 2) ? uiRect.w : uiRect.y);
    return float4(c, 0, 1);
}
float4 psUi() : SV_Target { return uiColor; }

float4 psUpscale(FullOut i) : SV_Target
{
    const float3 lit = gLit.SampleLevel(linearClamp, i.uv, 0).rgb * output.y;
    if (output.x > 0.5)
        return float4(NitsToPq(lit * 203.0), 1);       // HDR10: reference white at 203 nits
    return float4(LinearToSrgb(saturate(lit / (1 + lit))), 1); // sRGB, compression douce
}
)";

ComPtr<ID3DBlob> Compile(const char* entry, const char* target, bool instanced)
{
    const D3D_SHADER_MACRO macros[] = { { "INSTANCED", "1" }, { nullptr, nullptr } };
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kShaders, std::strlen(kShaders), "urti_sandbox", instanced ? macros : macros + 1, nullptr, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        std::fprintf(stderr, "%s: %s\n", entry, errors ? (const char*)errors->GetBufferPointer() : "compilation error");
        std::exit(1);
    }
    return code;
}

// ------------------------------------------------------------------------------------------------
// meshes and scene
// ------------------------------------------------------------------------------------------------

// vertex: position, normal, colour (alpha: smoothness), wind weight (0: still)
struct Vertex
{
    float p[3], n[3], c[4], wind, uv[2];
};

// a mesh: its range in the shared index buffer (like a game, each model has its own)
struct Mesh
{
    UINT firstIndex = 0, indexCount = 0;
};

struct Geometry
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    std::vector<Mesh> meshes;

    int Begin()
    {
        meshes.push_back({ UINT(idx.size()), 0 });
        return int(meshes.size()) - 1;
    }
    void End(int mesh) { meshes[mesh].indexCount = UINT(idx.size()) - meshes[mesh].firstIndex; }

    // unit box transformed by m (scale first, then rotation and translation); wind(x, y, z) gives the
    // wind weight of each vertex, from its position in the model
    template <class WindFn>
    void Box(const Mat4& m, float r, float g, float b, float smooth, WindFn wind)
    {
        const float n[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        for (int f = 0; f < 6; f++)
        {
            const float* N = n[f];
            const float u[3] = { N[1] != 0 ? 1.0f : 0.0f, N[1] == 0 ? 1.0f : 0.0f, 0.0f };
            const float t[3] = { N[1] * u[2] - N[2] * u[1], N[2] * u[0] - N[0] * u[2], N[0] * u[1] - N[1] * u[0] };
            const uint32_t base = uint32_t(verts.size());
            float wn[3];
            for (int c = 0; c < 3; c++)
                wn[c] = N[0] * m.m[c] + N[1] * m.m[4 + c] + N[2] * m.m[8 + c];
            const float l = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
            for (int k = 0; k < 4; k++)
            {
                const float a = (k == 1 || k == 2) ? 0.5f : -0.5f, bb = (k >= 2) ? 0.5f : -0.5f;
                float lp[3];
                for (int c = 0; c < 3; c++)
                    lp[c] = 0.5f * N[c] + a * u[c] + bb * t[c];
                Vertex x;
                for (int c = 0; c < 3; c++)
                    x.p[c] = lp[0] * m.m[c] + lp[1] * m.m[4 + c] + lp[2] * m.m[8 + c] + m.m[12 + c], x.n[c] = wn[c] / l;
                x.c[0] = r, x.c[1] = g, x.c[2] = b, x.c[3] = smooth;
                x.wind = wind(x.p[0], x.p[1], x.p[2]);
                x.uv[0] = x.uv[1] = -1; // surface pleine
                verts.push_back(x);
            }
            const uint32_t q[6] = { 0, 1, 2, 0, 2, 3 };
            for (uint32_t k : q)
                idx.push_back(base + k);
        }
    }
    void Box(const Mat4& m, float r, float g, float b, float smooth)
    {
        Box(m, r, g, b, smooth, [](float, float, float) { return 0.0f; });
    }
    // square foliage card (side size) centred on c, turned by yaw, tilted by tilt, normal n
    // (outward from the tree, as games do to light foliage), cut out by alpha test
    template <class WindFn>
    void Card(const float c[3], float size, float yaw, float tilt, const float n[3], float r, float g, float b, WindFn wind)
    {
        const float ux = std::cos(yaw), uz = -std::sin(yaw);
        const float vx = -std::sin(tilt) * uz, vy = std::cos(tilt), vz = std::sin(tilt) * ux;
        const uint32_t base = uint32_t(verts.size());
        const float corner[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
        for (const auto& k : corner)
        {
            Vertex x;
            x.p[0] = c[0] + (k[0] * ux + k[1] * vx) * size;
            x.p[1] = c[1] + k[1] * vy * size;
            x.p[2] = c[2] + (k[0] * uz + k[1] * vz) * size;
            for (int i = 0; i < 3; i++)
                x.n[i] = n[i];
            x.c[0] = r, x.c[1] = g, x.c[2] = b, x.c[3] = 0.35f;
            x.wind = wind(x.p[0], x.p[1], x.p[2]);
            x.uv[0] = k[0] + 0.5f, x.uv[1] = 0.5f - k[1];
            verts.push_back(x);
        }
        const uint32_t q[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint32_t k : q)
            idx.push_back(base + k);
    }
    // thin box from a to b (cable, branch), section s
    template <class WindFn>
    void Beam(const float a[3], const float b[3], float s, float r, float g, float bl, float smooth, WindFn wind)
    {
        const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const float yaw = std::atan2(-d[2], d[0]), pitch = std::asin(d[1] / len);
        const Mat4 m = Mul(Mul(Mul(Scale(len, s, s), RotateZ(pitch)), RotateY(yaw)), Translate((a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f));
        Box(m, r, g, bl, smooth, wind);
    }
};

struct Object
{
    int mesh = 0;
    Mat4 world, prevWorld;
    float color[4];
    float params[4]; // wind (multiplier), ground with puddles, reserved, metal
    int mover = -1;  // animated object (cart, passer-by)
    float moverPhase = 0;
    bool optionalShadow = false; // missing from the shadow map in URTI_SANDBOX_GAMESHADOW=partial mode
};

struct ObjectConstants
{
    Mat4 world, prevWorld;
    float color[4], params[4];
    float pad[24]; // 256 octets
};

struct FrameConstants
{
    Mat4 viewProj, viewProjNJ, prevViewProjNJ, invViewProj, lightViewProj[4];
    float cameraPos[4], sunDir[4], renderSize[4], splits[4], camForward[4], output[4], misc[4];
};

float Rand(uint32_t& s)
{
    s = s * 1664525u + 1013904223u;
    return float(s >> 8) / 16777216.0f;
}

// the scene: models as in a game (a tree, a building, a span of cables = one mesh,
// hence one draw per pass), plus small props drawn one by one (fence posts)
std::vector<Object> BuildScene(Geometry& geo, bool leafCards)
{
    std::vector<Object> o;
    uint32_t seed = 12345;
    auto add = [&](int mesh, float puddle = 0) -> Object& {
        Object x;
        x.mesh = mesh;
        x.world = x.prevWorld = Identity();
        x.color[0] = x.color[1] = x.color[2] = x.color[3] = 1;
        x.params[0] = 1, x.params[1] = puddle, x.params[2] = 0, x.params[3] = 0;
        o.push_back(x);
        return o.back();
    };
    // ground (cobblestones and puddles)
    {
        const int m = geo.Begin();
        geo.Box(Mul(Scale(240, 0.2f, 240), Translate(0, -0.1f, 0)), 0.42f, 0.38f, 0.33f, 0.25f);
        geo.End(m);
        add(m, 1);
    }
    // buildings on both sides of the street: walls, smooth windows, cornice, door
    for (int side = -1; side <= 1; side += 2)
        for (int i = 0; i < 11; i++)
        {
            const float h = 6 + 8 * Rand(seed), w = 7 + 2 * Rand(seed), x = -55 + i * 10.0f, z = side * 15.0f, front = z - side * 4.0f;
            const int m = geo.Begin();
            geo.Box(Mul(Scale(w, h, 8), Translate(x, h * 0.5f, z)), 0.45f + 0.3f * Rand(seed), 0.35f + 0.2f * Rand(seed), 0.3f + 0.15f * Rand(seed), 0.15f);
            for (int f = 0; f < 2; f++)
                geo.Box(Mul(Scale(w * 0.3f, h * 0.35f, 0.06f), Translate(x + (f ? 1 : -1) * w * 0.22f, h * 0.6f, front - side * 0.03f)), 0.08f, 0.1f, 0.12f, 0.95f);
            geo.Box(Mul(Scale(w + 0.6f, 0.4f, 0.8f), Translate(x, h + 0.2f, front)), 0.3f, 0.25f, 0.2f, 0.2f);
            geo.Box(Mul(Scale(1.4f, 2.4f, 0.1f), Translate(x, 1.2f, front - side * 0.05f)), 0.25f, 0.15f, 0.08f, 0.3f);
            geo.End(m);
            add(m);
        }
    // power lines: poles, then each span (3 cables hanging and swaying in the wind)
    for (int side = -1; side <= 1; side += 2)
        for (int i = 0; i < 8; i++)
        {
            const float x = -52 + i * 15.0f, z = side * 8.0f;
            const int pole = geo.Begin();
            geo.Box(Mul(Scale(0.25f, 8, 0.25f), Translate(x, 4, z)), 0.3f, 0.22f, 0.15f, 0.1f);
            geo.Box(Mul(Scale(0.15f, 0.15f, 1.6f), Translate(x, 7.6f, z)), 0.3f, 0.22f, 0.15f, 0.1f);
            geo.End(pole);
            add(pole).optionalShadow = true;
            if (i == 7)
                continue;
            const int span = geo.Begin();
            for (int c = 0; c < 3; c++)
            {
                const float cz = z + (c - 1) * 0.6f, top = 7.7f;
                const int segments = 16;
                for (int k = 0; k < segments; k++)
                {
                    const float u0 = float(k) / segments, u1 = float(k + 1) / segments;
                    const float a[3] = { x + 15 * u0, top - 1.2f * 4 * u0 * (1 - u0), cz }, b[3] = { x + 15 * u1, top - 1.2f * 4 * u1 * (1 - u1), cz };
                    geo.Beam(a, b, 0.03f, 0.05f, 0.05f, 0.05f, 0.4f, [x](float px, float, float) {
                        const float u = std::clamp((px - x) / 15.0f, 0.0f, 1.0f);
                        return 0.12f * std::sin(kPi * u);
                    });
                }
            }
            geo.End(span);
            add(span);
        }
    // cables across the street
    for (int i = 0; i < 4; i++)
    {
        const float x = -37 + i * 30.0f;
        const int m = geo.Begin();
        const int segments = 16;
        for (int k = 0; k < segments; k++)
        {
            const float u0 = float(k) / segments, u1 = float(k + 1) / segments;
            const float a[3] = { x, 7.5f - 1.5f * 4 * u0 * (1 - u0), -8 + 16 * u0 }, b[3] = { x, 7.5f - 1.5f * 4 * u1 * (1 - u1), -8 + 16 * u1 };
            geo.Beam(a, b, 0.03f, 0.05f, 0.05f, 0.05f, 0.4f, [](float, float, float pz) {
                const float u = std::clamp((pz + 8) / 16.0f, 0.0f, 1.0f);
                return 0.15f * std::sin(kPi * u);
            });
        }
        geo.End(m);
        add(m);
    }
    // trees: trunk, thin branches and foliage; the wind grows with height
    const float treeX[5] = { -40, -18, 4, 26, 46 };
    for (float tx : treeX)
        for (int side = -1; side <= 1; side += 2)
        {
            const float tz = side * 10.5f;
            auto windTree = [](float, float py, float) { return 0.22f * std::clamp((py - 2.8f) / 3.5f, 0.0f, 1.0f); };
            const int m = geo.Begin();
            geo.Box(Mul(Scale(0.45f, 4.5f, 0.45f), Translate(tx, 2.25f, tz)), 0.28f, 0.2f, 0.12f, 0.1f);
            for (int b = 0; b < 26; b++)
            {
                const float yaw = Rand(seed) * 2 * kPi, rise = 0.3f + 0.8f * Rand(seed), len = 1.2f + 1.4f * Rand(seed), y = 3.4f + 1.6f * Rand(seed);
                const float a[3] = { tx, y, tz };
                const float e[3] = { tx + std::cos(yaw) * std::cos(rise) * len, y + std::sin(rise) * len, tz + std::sin(yaw) * std::cos(rise) * len };
                geo.Beam(a, e, 0.07f, 0.3f, 0.22f, 0.13f, 0.1f, windTree);
            }
            for (int l = 0; l < 70; l++)
            {
                const float a = Rand(seed) * 2 * kPi, r = 0.6f + 2.0f * Rand(seed), y = 4.0f + 2.6f * Rand(seed), s = 0.35f + 0.35f * Rand(seed);
                const float yaw = Rand(seed) * kPi, cr = 0.18f + 0.1f * Rand(seed), cg = 0.35f + 0.15f * Rand(seed);
                if (!leafCards)
                {
                    geo.Box(Mul(Mul(Scale(s, s * 0.6f, s), RotateY(yaw)), Translate(tx + std::cos(a) * r, y, tz + std::sin(a) * r)), cr, cg, 0.12f, 0.35f, windTree);
                    continue;
                }
                // cluster: two crossed cards, three times larger than a block (the texture cuts the leaves out)
                const float c[3] = { tx + std::cos(a) * r, y, tz + std::sin(a) * r };
                float n[3] = { std::cos(a), 0.6f, std::sin(a) };
                const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (float& v : n)
                    v /= nl;
                const float tilt = (Rand(seed) - 0.5f) * 0.8f;
                geo.Card(c, s * 3.0f, yaw, tilt, n, cr, cg, 0.12f, windTree);
                geo.Card(c, s * 3.0f, yaw + kPi * 0.5f, -tilt, n, cr, cg, 0.12f, windTree);
            }
            geo.End(m);
            add(m);
        }
    // fences in front of the houses: one post = one draw (games have many small draws)
    for (int side = -1; side <= 1; side += 2)
    {
        const float z = side * 12.3f;
        const int rails = geo.Begin();
        for (int r = 0; r < 2; r++)
            geo.Box(Mul(Scale(110, 0.08f, 0.05f), Translate(0, 0.45f + r * 0.4f, z)), 0.5f, 0.4f, 0.3f, 0.2f);
        geo.End(rails);
        add(rails);
        for (int i = 0; i < 74; i++)
        {
            const int post = geo.Begin();
            geo.Box(Mul(Scale(0.1f, 1.1f, 0.1f), Translate(-55 + i * 1.5f, 0.55f, z)), 0.5f, 0.4f, 0.3f, 0.2f);
            geo.End(post);
            add(post).optionalShadow = true;
        }
    }
    // a wall in the middle of the street: two passers-by walk behind it (occlusion test)
    {
        const int m = geo.Begin();
        geo.Box(Mul(Scale(4, 2.2f, 0.3f), Translate(1, 1.1f, 4)), 0.55f, 0.5f, 0.45f, 0.15f);
        geo.End(m);
        add(m).optionalShadow = true;
    }
    // carts (box, wheels, shafts) and passers-by: they move (object motion vectors)
    for (int c = 0; c < 3; c++)
    {
        const int m = geo.Begin();
        geo.Box(Mul(Scale(2.6f, 0.9f, 1.3f), Translate(0, 1.15f, 0)), 0.4f, 0.27f, 0.15f, 0.3f);
        for (int w = 0; w < 4; w++)
            geo.Box(Mul(Scale(0.9f, 0.9f, 0.12f), Translate((w & 1 ? 0.8f : -0.8f), 0.45f, (w & 2 ? 0.72f : -0.72f))), 0.2f, 0.14f, 0.08f, 0.2f);
        for (int s = 0; s < 2; s++)
            geo.Box(Mul(Scale(2.0f, 0.08f, 0.08f), Translate(2.2f, 0.9f, s ? 0.5f : -0.5f)), 0.35f, 0.25f, 0.15f, 0.2f);
        geo.End(m);
        Object& cart = add(m);
        cart.mover = 0, cart.moverPhase = c * 2.1f, cart.optionalShadow = true;
    }
    for (int p = 0; p < 5; p++)
    {
        const int m = geo.Begin();
        geo.Box(Mul(Scale(0.5f, 1.4f, 0.35f), Translate(0, 0.7f, 0)), 0.25f + 0.1f * p, 0.2f, 0.18f, 0.2f);
        geo.Box(Mul(Scale(0.25f, 0.28f, 0.25f), Translate(0, 1.55f, 0)), 0.7f, 0.55f, 0.45f, 0.3f);
        geo.Box(Mul(Scale(0.5f, 0.08f, 0.5f), Translate(0, 1.72f, 0)), 0.15f, 0.12f, 0.1f, 0.2f); // chapeau
        geo.End(m);
        Object& walker = add(m);
        walker.mover = 1, walker.moverPhase = p * 1.3f, walker.optionalShadow = true;
    }
    return o;
}

// animated position of a moving object at time t
Mat4 MoverMatrix(const Object& o, float t)
{
    if (o.mover == 0) // cart: back and forth along the street
    {
        const float x = 30 * std::sin(t * 0.18f + o.moverPhase), lane = (o.moverPhase > 2 ? -1.0f : 1.0f) * 2.6f;
        const float dir = std::cos(t * 0.18f + o.moverPhase) >= 0 ? 0.0f : kPi;
        return Mul(RotateY(dir), Translate(x, 0, lane + 0.4f * o.moverPhase));
    }
    const float x = -20 + std::fmod(t * 1.3f + o.moverPhase * 9, 45.0f), z = 5.8f - o.moverPhase;
    return Translate(x, 0.05f * std::sin(t * 9 + o.moverPhase), z);
}

// scripted camera: walks down the street, looks around, fast U-turns; identical on every launch
void CameraAt(const std::string& mode, float t, float eye[3], float at[3])
{
    if (mode == "fixed")
    {
        eye[0] = -6, eye[1] = 1.7f, eye[2] = -3, at[0] = 6, at[1] = 2.5f, at[2] = 3;
        return;
    }
    // fixed shots of the tests (wet ground advised: reflections cover the image)
    struct Shot { const char* name; float eye[3], at[3]; };
    static const Shot shots[] = {
        { "building", { -5, 1.7f, 3 }, { -5, 2.5f, 11 } },       // facade, windows, nothing moves
        { "tree", { 4, 1.7f, 4 }, { 4, 0.2f, -10.5f } },        // a tree and its reflection in the puddle
        { "street", { -12, 1.7f, -2 }, { -12, 1.0f, 5 } },          // passers-by cross
        { "occlusion", { 1, 1.7f, -3 }, { 1, 1.4f, 5 } },        // passers-by disappear behind the wall
    };
    for (const Shot& shot : shots)
        if (mode == shot.name)
        {
            for (int c = 0; c < 3; c++)
                eye[c] = shot.eye[c], at[c] = shot.at[c];
            return;
        }
    if (mode == "orbit")
    {
        eye[0] = 20 * std::cos(t * 0.2f), eye[1] = 6, eye[2] = 20 * std::sin(t * 0.2f), at[0] = 0, at[1] = 2, at[2] = 0;
        return;
    }
    eye[0] = -45 + std::fmod(t * 1.6f, 90.0f), eye[1] = 1.7f + 0.03f * std::sin(t * 9.5f), eye[2] = 1.2f * std::sin(t * 0.3f);
    float yaw = 0.8f * std::sin(t * 0.45f);
    const float cycle = std::fmod(t, 9.0f); // fast U-turn every 9 s (0.6 s), back 3 s later
    if (cycle > 5.0f && cycle < 5.6f)
        yaw += kPi * (cycle - 5.0f) / 0.6f;
    else if (cycle >= 5.6f && cycle < 8.4f)
        yaw += kPi;
    else if (cycle >= 8.4f)
        yaw += kPi * (1 - (cycle - 8.4f) / 0.6f);
    const float pitch = -0.08f + 0.12f * std::sin(t * 0.7f);
    at[0] = eye[0] + std::cos(yaw) * std::cos(pitch), at[1] = eye[1] + std::sin(pitch), at[2] = eye[2] + std::sin(yaw) * std::cos(pitch);
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, const void* data = nullptr)
{
    D3D12_HEAP_PROPERTIES heap = { heapType };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)));
    if (data)
    {
        void* p = nullptr;
        CHECK(r->Map(0, nullptr, &p));
        std::memcpy(p, data, size);
        r->Unmap(0, nullptr);
    }
    return r;
}

ComPtr<ID3D12Resource> Texture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                               const D3D12_CLEAR_VALUE* clear)
{
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> r;
    CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&r)));
    return r;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}
} // namespace

int main()
{
    auto env = [](const char* name, const char* fallback) {
        const char* v = std::getenv(name);
        return std::string(v && *v ? v : fallback);
    };
    const int frames = std::atoi(env("URTI_FRAMES", "600").c_str());
    const bool show = env("URTI_SHOW", "0") != "0";
    const std::string cameraMode = env("URTI_SANDBOX_CAMERA", "walk");
    const bool frameGen = env("URTI_SANDBOX_FG", "0") != "0";
    const bool hdr = env("URTI_SANDBOX_SDR", "0") == "0";
    const float start = float(std::atof(env("URTI_SANDBOX_START", "0").c_str()));
    const float wind = float(std::atof(env("URTI_SANDBOX_WIND", "1").c_str()));
    const bool movers = env("URTI_SANDBOX_MOVERS", "1") != "0";
    const float wet = float(std::atof(env("URTI_SANDBOX_WET", "-1").c_str()));
    const bool truth = env("URTI_SANDBOX_TRUTH", "0") != "0";
    const bool partialShadows = env("URTI_SANDBOX_GAMESHADOW", "full") == "partial";
    const bool leafCards = env("URTI_SANDBOX_LEAVES", "cards") != "blocks"; // foliage as cut-out cards (default) or as blocks
    const bool ui = env("URTI_SANDBOX_UI", "1") != "0";
    const float jitter = float(std::atof(env("URTI_SANDBOX_JITTER", "1").c_str())); // 0: no sub-pixel jitter
    const double fpsCap = std::atof(env("URTI_SANDBOX_FPS", "90").c_str());

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UrtiSandbox";
    RegisterClassW(&wc);
    RECT rect = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"URTI sandbox", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                              rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (show)
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    ComPtr<IDXGIFactory4> factory;
    CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));

    const DXGI_FORMAT outFormat = hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width = kWidth;
    scDesc.Height = kHeight;
    scDesc.Format = outFormat;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = kBuffers;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc1;
    CHECK(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scDesc, nullptr, nullptr, &sc1));
    ComPtr<IDXGISwapChain3> swapchain;
    CHECK(sc1.As(&swapchain));
    if (hdr)
        swapchain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020); // HDR10 if the screen allows it
    ComPtr<ID3D12Resource> backBuffers[kBuffers];
    for (UINT i = 0; i < kBuffers; i++)
        CHECK(swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])));

    // ---------------- cibles ----------------
    const DXGI_FORMAT gFormats[5] = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT };
    ComPtr<ID3D12Resource> gbuffer[5];
    for (int i = 0; i < 5; i++)
    {
        D3D12_CLEAR_VALUE cv = { gFormats[i], { 0, 0, 0, 0 } };
        gbuffer[i] = Texture(device.Get(), kRenderW, kRenderH, gFormats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    }
    D3D12_CLEAR_VALUE depthClear = { DXGI_FORMAT_D32_FLOAT_S8X24_UINT };
    depthClear.DepthStencil.Depth = 0.0f; // reversed depth: far at 0
    ComPtr<ID3D12Resource> depth = Texture(device.Get(), kRenderW, kRenderH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &depthClear);
    D3D12_CLEAR_VALUE halfClear = { DXGI_FORMAT_D32_FLOAT };
    halfClear.DepthStencil.Depth = 0.0f;
    ComPtr<ID3D12Resource> halfDepth = Texture(device.Get(), kRenderW / 2, kRenderH / 2, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &halfClear);
    D3D12_CLEAR_VALUE atlasClear = { DXGI_FORMAT_D16_UNORM };
    atlasClear.DepthStencil.Depth = 1.0f;
    ComPtr<ID3D12Resource> atlas = Texture(device.Get(), kAtlas, kAtlas * kCascades, DXGI_FORMAT_R16_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &atlasClear);
    D3D12_CLEAR_VALUE litClear = { DXGI_FORMAT_R16G16B16A16_FLOAT, { 0, 0, 0, 0 } };
    ComPtr<ID3D12Resource> lit = Texture(device.Get(), kRenderW, kRenderH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &litClear);
    D3D12_CLEAR_VALUE finalClear = { outFormat, { 0, 0, 0, 0 } };
    ComPtr<ID3D12Resource> finalTex = Texture(device.Get(), kWidth, kHeight, outFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                              D3D12_RESOURCE_STATE_COPY_SOURCE, &finalClear);

    // ---------------- descripteurs ----------------
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 10 + kBuffers };
    ComPtr<ID3D12Resource> mirror = Texture(device.Get(), kRenderW, kRenderH, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &litClear);
    ComPtr<ID3D12Resource> mirrorDepth = Texture(device.Get(), kRenderW, kRenderH, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                                 D3D12_RESOURCE_STATE_DEPTH_WRITE, &halfClear);
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4 };
    ComPtr<ID3D12Resource> taaHistory[2];
    for (auto& h : taaHistory)
        h = Texture(device.Get(), kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    &litClear);
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap, srvHeap;
    CHECK(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)));
    CHECK(device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&dsvHeap)));
    CHECK(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap)));
    const UINT rtvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT dsvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const UINT srvStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto rtv = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * rtvStep; return h; };
    auto dsv = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * dsvStep; return h; };
    auto srvCpu = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += i * srvStep; return h; };
    auto srvGpu = [&](UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE h = srvHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += i * srvStep; return h; };
    for (int i = 0; i < 5; i++)
        device->CreateRenderTargetView(gbuffer[i].Get(), nullptr, rtv(i));
    device->CreateRenderTargetView(lit.Get(), nullptr, rtv(5));
    device->CreateRenderTargetView(finalTex.Get(), nullptr, rtv(6));
    device->CreateRenderTargetView(taaHistory[0].Get(), nullptr, rtv(7));
    device->CreateRenderTargetView(taaHistory[1].Get(), nullptr, rtv(8));
    device->CreateRenderTargetView(mirror.Get(), nullptr, rtv(9));
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth.Get(), &d, dsv(0));
        d.Format = DXGI_FORMAT_D16_UNORM;
        device->CreateDepthStencilView(atlas.Get(), &d, dsv(1));
        d.Format = DXGI_FORMAT_D32_FLOAT;
        device->CreateDepthStencilView(halfDepth.Get(), &d, dsv(2));
        device->CreateDepthStencilView(mirrorDepth.Get(), &d, dsv(3));
    }
    auto srv2d = [&](ID3D12Resource* r, DXGI_FORMAT f, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = f, s.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, s.Texture2D.MipLevels = 1;
        s.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        device->CreateShaderResourceView(r, &s, srvCpu(slot));
    };
    // lighting table (t0..t4) then upscaling table (t0 = lit image, the rest filled in)
    for (int i = 0; i < 3; i++)
        srv2d(gbuffer[i].Get(), gFormats[i], i);
    srv2d(depth.Get(), DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 3);
    srv2d(atlas.Get(), DXGI_FORMAT_R16_UNORM, 4);
    srv2d(mirror.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 5); // t5 of the lighting
    for (int i = 6; i < 10; i++)
        srv2d(lit.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, i);
    // TAA tables (writes history k, reads history 1 - k): 10 + 5 k; output: 20 + 5 k
    for (int k = 0; k < 2; k++)
    {
        srv2d(lit.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 10 + 5 * k);
        srv2d(gbuffer[4].Get(), gFormats[4], 11 + 5 * k);
        srv2d(taaHistory[1 - k].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 12 + 5 * k);
        srv2d(lit.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 13 + 5 * k);
        srv2d(lit.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 14 + 5 * k);
        srv2d(taaHistory[k].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 20 + 5 * k);
        for (int j = 1; j < 5; j++)
            srv2d(lit.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 20 + 5 * k + j);
    }
    for (int i = 30; i < 32; i++) // the tables read 6 descriptors: the last one overflows here
        srv2d(lit.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, i);

    // ---------------- root signature (without ALLOW_STREAM_OUTPUT, like a game) ----------------
    ComPtr<ID3D12RootSignature> rootSig;
    {
        D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, 0 };
        D3D12_ROOT_PARAMETER p[4] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV, p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV, p[1].Descriptor.ShaderRegister = 1;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, p[2].Constants.ShaderRegister = 2, p[2].Constants.Num32BitValues = 12;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, p[3].DescriptorTable.NumDescriptorRanges = 1, p[3].DescriptorTable.pDescriptorRanges = &range;
        p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC s[2] = {};
        s[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s[0].AddressU = s[0].AddressV = s[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s[0].ShaderRegister = 0, s[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL, s[0].MaxLOD = D3D12_FLOAT32_MAX;
        s[1] = s[0];
        s[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, s[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL, s[1].ShaderRegister = 1;
        D3D12_ROOT_SIGNATURE_DESC d = { 4, p, 2, s, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        ComPtr<ID3DBlob> blob, err;
        CHECK(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err));
        CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSig)));
    }

    // ---------------- pipelines ----------------
    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "WIND", 0, DXGI_FORMAT_R32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "INSTOFFSET", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
    };
    auto geometryPso = [&](const char* vs, const char* ps, bool instanced, UINT targets, const DXGI_FORMAT* formats, DXGI_FORMAT dsvFormat,
                           int depthBias) {
        ComPtr<ID3DBlob> v = Compile(vs, "vs_5_1", instanced), p;
        if (ps)
            p = Compile(ps, "ps_5_1", instanced);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = rootSig.Get();
        d.VS = { v->GetBufferPointer(), v->GetBufferSize() };
        if (p)
            d.PS = { p->GetBufferPointer(), p->GetBufferSize() };
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        for (UINT i = 1; i < 8; i++)
            d.BlendState.RenderTarget[i] = d.BlendState.RenderTarget[0];
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState.DepthClipEnable = TRUE;
        d.RasterizerState.DepthBias = depthBias;
        d.RasterizerState.SlopeScaledDepthBias = depthBias ? 1.5f : 0.0f;
        d.DepthStencilState.DepthEnable = dsvFormat != DXGI_FORMAT_UNKNOWN;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = depthBias ? D3D12_COMPARISON_FUNC_LESS : D3D12_COMPARISON_FUNC_GREATER; // shadows: normal; screen: reversed
        d.InputLayout = { layout, instanced ? 6u : 5u };
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = targets;
        for (UINT i = 0; i < targets; i++)
            d.RTVFormats[i] = formats[i];
        d.DSVFormat = dsvFormat;
        d.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        CHECK(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
        return pso;
    };
    auto fullscreenPso = [&](const char* ps, DXGI_FORMAT format) {
        ComPtr<ID3DBlob> v = Compile("vsFull", "vs_5_1", false), p = Compile(ps, "ps_5_1", false);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = rootSig.Get();
        d.VS = { v->GetBufferPointer(), v->GetBufferSize() };
        d.PS = { p->GetBufferPointer(), p->GetBufferSize() };
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = format;
        d.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pso;
        CHECK(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
        return pso;
    };
    ComPtr<ID3D12PipelineState> gbufferPso = geometryPso("vsGbuffer", "psGbuffer", false, 5, gFormats, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0);
    ComPtr<ID3D12PipelineState> gbufferInstPso = geometryPso("vsGbuffer", "psGbuffer", true, 5, gFormats, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0);
    ComPtr<ID3D12PipelineState> shadowPso = geometryPso("vsShadow", "psShadow", false, 0, nullptr, DXGI_FORMAT_D16_UNORM, 40);
    ComPtr<ID3D12PipelineState> shadowInstPso = geometryPso("vsShadow", "psShadow", true, 0, nullptr, DXGI_FORMAT_D16_UNORM, 40);
    ComPtr<ID3D12PipelineState> halfPso = geometryPso("vsGbuffer", nullptr, false, 0, nullptr, DXGI_FORMAT_D32_FLOAT, 0);
    const DXGI_FORMAT mirrorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D12PipelineState> mirrorPso = geometryPso("vsMirror", "psMirror", false, 1, &mirrorFormat, DXGI_FORMAT_D32_FLOAT, 0);
    ComPtr<ID3D12PipelineState> mirrorInstPso = geometryPso("vsMirror", "psMirror", true, 1, &mirrorFormat, DXGI_FORMAT_D32_FLOAT, 0);
    ComPtr<ID3D12PipelineState> lightingPso = fullscreenPso("psLighting", DXGI_FORMAT_R16G16B16A16_FLOAT);
    ComPtr<ID3D12PipelineState> upscalePso = fullscreenPso("psUpscale", outFormat);
    ComPtr<ID3D12PipelineState> taaPso = fullscreenPso("psTaa", DXGI_FORMAT_R16G16B16A16_FLOAT);
    // UI: rectangles blended by their alpha, into the screen image (one view per screen image)
    ComPtr<ID3D12PipelineState> uiPso;
    {
        ComPtr<ID3DBlob> v = Compile("vsUi", "vs_5_1", false), q = Compile("psUi", "ps_5_1", false);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = rootSig.Get();
        d.VS = { v->GetBufferPointer(), v->GetBufferSize() };
        d.PS = { q->GetBufferPointer(), q->GetBufferSize() };
        D3D12_RENDER_TARGET_BLEND_DESC& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA, b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA, b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE, b.DestBlendAlpha = D3D12_BLEND_ZERO, b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        d.SampleMask = UINT_MAX;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = outFormat;
        d.SampleDesc.Count = 1;
        CHECK(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&uiPso)));
    }
    for (UINT i = 0; i < kBuffers; i++)
        device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, rtv(10 + i));

    // ---------------- meshes, barrels through ExecuteIndirect ----------------
    Geometry geo;
    std::vector<Object> scene = BuildScene(geo, leafCards);
    const int barrelMesh = geo.Begin();
    geo.Box(Scale(0.7f, 1.0f, 0.7f), 0.35f, 0.22f, 0.12f, 0.3f);
    geo.Box(Mul(Scale(0.74f, 0.08f, 0.74f), Translate(0, 0.3f, 0)), 0.15f, 0.15f, 0.15f, 0.6f); // metal hoop
    geo.Box(Mul(Scale(0.74f, 0.08f, 0.74f), Translate(0, -0.3f, 0)), 0.15f, 0.15f, 0.15f, 0.6f);
    geo.End(barrelMesh);
    const std::vector<Vertex>& verts = geo.verts;
    const std::vector<uint32_t>& idx = geo.idx;
    std::printf("urti_sandbox : %zu objets, %zu triangles\n", scene.size() + 1, idx.size() / 3);
    ComPtr<ID3D12Resource> vb = Buffer(device.Get(), verts.size() * sizeof(Vertex), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, verts.data());
    ComPtr<ID3D12Resource> ib = Buffer(device.Get(), idx.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, idx.data());
    const D3D12_VERTEX_BUFFER_VIEW vbv = { vb->GetGPUVirtualAddress(), UINT(verts.size() * sizeof(Vertex)), sizeof(Vertex) };
    const D3D12_INDEX_BUFFER_VIEW ibv = { ib->GetGPUVirtualAddress(), UINT(idx.size() * 4), DXGI_FORMAT_R32_UINT };
    std::vector<float> instances;
    for (int i = 0; i < 24; i++)
        instances.insert(instances.end(), { -50 + i * 4.3f, 0.5f, (i % 2 ? 6.5f : -6.5f) + 0.3f * float(i % 3), 0 });
    ComPtr<ID3D12Resource> instBuf = Buffer(device.Get(), instances.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, instances.data());
    const D3D12_VERTEX_BUFFER_VIEW instView = { instBuf->GetGPUVirtualAddress(), UINT(instances.size() * 4), 16 };
    const D3D12_DRAW_INDEXED_ARGUMENTS indirect = { geo.meshes[barrelMesh].indexCount, 24, geo.meshes[barrelMesh].firstIndex, 0, 0 };
    ComPtr<ID3D12Resource> argBuf = Buffer(device.Get(), sizeof(indirect), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &indirect);
    ComPtr<ID3D12CommandSignature> cmdSig;
    {
        D3D12_INDIRECT_ARGUMENT_DESC a = {};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC d = { sizeof(D3D12_DRAW_INDEXED_ARGUMENTS), 1, &a, 0 };
        CHECK(device->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&cmdSig)));
    }

    if (scene.size() + 1 > kMaxObjects) // too many objects for the buffer
        return 1;
    ComPtr<ID3D12Resource> objectBuf = Buffer(device.Get(), kMaxObjects * sizeof(ObjectConstants), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> frameBuf = Buffer(device.Get(), 256 * ((sizeof(FrameConstants) + 255) / 256), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    ObjectConstants* objectData = nullptr;
    FrameConstants* frameData = nullptr;
    CHECK(objectBuf->Map(0, nullptr, (void**)&objectData));
    CHECK(frameBuf->Map(0, nullptr, (void**)&frameData));

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto waitGpu = [&]() {
        CHECK(queue->Signal(fence.Get(), ++fenceValue));
        CHECK(fence->SetEventOnCompletion(fenceValue, fenceEvent));
        WaitForSingleObject(fenceEvent, INFINITE);
    };

    const float fovY = 55.0f * kPi / 180.0f, aspect = float(kWidth) / kHeight, zNear = 0.15f;
    const float splitDist[4] = { 10, 28, 70, 180 }, cascadeSize[4] = { 24, 60, 150, 380 };
    float sun[3] = { 0.45f, 0.62f, 0.3f };
    {
        const float l = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        for (float& v : sun)
            v /= l;
    }
    Mat4 prevViewProjNJ = Identity();
    std::printf("urti_sandbox: %d frames, camera %s%s, output %s, render %ux%u -> %ux%u\n", frames, cameraMode.c_str(), frameGen ? ", frame generation" : "",
                hdr ? "HDR10" : "sRGB", kRenderW, kRenderH, kWidth, kHeight);

    // GPU time of each pass (average shown at the end)
    constexpr UINT kStamps = 7;
    const char* stampNames[kStamps - 1] = { "cascades", "G-buffer", "half resolution", "lighting", "upscaling", "copy" };
    ComPtr<ID3D12QueryHeap> stampHeap;
    {
        D3D12_QUERY_HEAP_DESC d = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps };
        CHECK(device->CreateQueryHeap(&d, IID_PPV_ARGS(&stampHeap)));
    }
    ComPtr<ID3D12Resource> stampBuf = Buffer(device.Get(), kStamps * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 gpuFreq = 1;
    queue->GetTimestampFrequency(&gpuFreq);
    double stampSum[kStamps - 1] = {};
    int stampFrames = 0;
    auto stamp = [&](UINT i) { list->EndQuery(stampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i); };
    LARGE_INTEGER freq, tick0 = {}, tickNow;
    double presentSum = 0, waitSum = 0;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER pace;
    QueryPerformanceCounter(&pace);
    for (int frame = 0; frame < frames; frame++)
    {
        if (frame == 30)
            QueryPerformanceCounter(&tick0);
        if (fpsCap > 0)
        {
            // fixed pace: wait for this frame's planned moment
            pace.QuadPart += LONGLONG(freq.QuadPart / fpsCap);
            for (LARGE_INTEGER now; QueryPerformanceCounter(&now), now.QuadPart < pace.QuadPart;)
                if (pace.QuadPart - now.QuadPart > freq.QuadPart / 500)
                    Sleep(1);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            if (now.QuadPart > pace.QuadPart + freq.QuadPart / 10)
                pace = now; // far behind (busy machine): no catching up in a burst
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            TranslateMessage(&msg), DispatchMessageW(&msg);
        const float dt = 1.0f / 60.0f, t = start + frame * dt, tPrev = t - dt;

        // ---------------- camera (sub-pixel jitter like DLSS) ----------------
        float eye[3], at[3];
        CameraAt(cameraMode, t, eye, at);
        const Mat4 view = LookAtLH(eye, at);
        const Mat4 proj = ReversedInfiniteLH(fovY, aspect, zNear);
        Mat4 projJ = proj;
        const int jitterIndex = (frame % 8) + 1;
        projJ.m[8] = jitter * (Halton(jitterIndex, 2) - 0.5f) * 2.0f / kRenderW;
        projJ.m[9] = jitter * (Halton(jitterIndex, 3) - 0.5f) * 2.0f / kRenderH;
        const Mat4 viewProj = Mul(view, projJ), viewProjNJ = Mul(view, proj);
        if (frame == 0)
            prevViewProjNJ = viewProjNJ;
        float fwd[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
        {
            const float l = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
            for (float& v : fwd)
                v /= l;
        }

        // ---------------- cascades: follow the camera, in texel steps ----------------
        Mat4 lightVP[4];
        for (UINT k = 0; k < kCascades; k++)
        {
            const float centerDist = (k == 0 ? 0.0f : splitDist[k - 1]) * 0.5f + splitDist[k] * 0.5f;
            const float c[3] = { eye[0] + fwd[0] * centerDist * 0.6f, 0, eye[2] + fwd[2] * centerDist * 0.6f };
            const float lightEye[3] = { c[0] + sun[0] * 300, c[1] + sun[1] * 300, c[2] + sun[2] * 300 };
            Mat4 lv = LookAtLH(lightEye, c);
            const float texel = cascadeSize[k] / kAtlas;
            lv.m[12] = std::floor(lv.m[12] / texel) * texel, lv.m[13] = std::floor(lv.m[13] / texel) * texel;
            // depth over 400 m around the centre (D16: 6 mm per step), bias of 40 steps (~25 cm) like a game
            lightVP[k] = Mul(lv, OrthoLH(cascadeSize[k], cascadeSize[k], 100, 500));
        }

        FrameConstants& fc = *frameData;
        fc.viewProj = viewProj, fc.viewProjNJ = viewProjNJ, fc.prevViewProjNJ = prevViewProjNJ, fc.invViewProj = Inverse(viewProj);
        for (UINT k = 0; k < kCascades; k++)
            fc.lightViewProj[k] = lightVP[k];
        fc.cameraPos[0] = eye[0], fc.cameraPos[1] = eye[1], fc.cameraPos[2] = eye[2], fc.cameraPos[3] = t;
        fc.sunDir[0] = sun[0], fc.sunDir[1] = sun[1], fc.sunDir[2] = sun[2], fc.sunDir[3] = tPrev;
        fc.renderSize[0] = float(kRenderW), fc.renderSize[1] = float(kRenderH), fc.renderSize[2] = 1.0f / kRenderW, fc.renderSize[3] = 1.0f / kRenderH;
        for (int k = 0; k < 4; k++)
            fc.splits[k] = splitDist[k];
        fc.camForward[0] = fwd[0], fc.camForward[1] = fwd[1], fc.camForward[2] = fwd[2], fc.camForward[3] = wind;
        fc.output[0] = hdr ? 1.0f : 0.0f, fc.output[1] = 1.0f, fc.output[2] = projJ.m[8], fc.output[3] = projJ.m[9];
        fc.misc[0] = wet, fc.misc[1] = frame == 0 ? 1.0f : 0.0f, fc.misc[2] = truth ? 1.0f : 0.0f;

        // objects: positions in this frame and the previous one (motion vectors)
        for (size_t i = 0; i < scene.size(); i++)
        {
            Object& o = scene[i];
            if (o.mover >= 0)
                o.world = MoverMatrix(o, movers ? t : start), o.prevWorld = frame == 0 ? o.world : MoverMatrix(o, movers ? tPrev : start);
            ObjectConstants& oc = objectData[i];
            oc.world = o.world, oc.prevWorld = o.prevWorld;
            std::memcpy(oc.color, o.color, sizeof(o.color));
            std::memcpy(oc.params, o.params, sizeof(o.params));
        }
        ObjectConstants& barrel = objectData[scene.size()];
        barrel.world = barrel.prevWorld = Identity();
        barrel.color[0] = barrel.color[1] = barrel.color[2] = barrel.color[3] = 1;
        barrel.params[0] = barrel.params[1] = barrel.params[2] = barrel.params[3] = 0;

        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), nullptr));
        list->SetGraphicsRootSignature(rootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootConstantBufferView(0, frameBuf->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetIndexBuffer(&ibv);
        auto drawScene = [&](ID3D12PipelineState* plain, ID3D12PipelineState* instanced, bool shadowPass = false) {
            list->SetPipelineState(plain);
            list->IASetVertexBuffers(0, 1, &vbv);
            for (size_t i = 0; i < scene.size(); i++)
            {
                if (shadowPass && partialShadows && scene[i].optionalShadow)
                    continue;
                list->SetGraphicsRootConstantBufferView(1, objectBuf->GetGPUVirtualAddress() + i * sizeof(ObjectConstants));
                list->DrawIndexedInstanced(geo.meshes[scene[i].mesh].indexCount, 1, geo.meshes[scene[i].mesh].firstIndex, 0, 0);
            }
            list->SetPipelineState(instanced);
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instView };
            list->IASetVertexBuffers(0, 2, views);
            list->SetGraphicsRootConstantBufferView(1, objectBuf->GetGPUVirtualAddress() + scene.size() * sizeof(ObjectConstants));
            list->ExecuteIndirect(cmdSig.Get(), 1, argBuf.Get(), 0, nullptr, 0);
        };

        stamp(0);
        // 1. cascade atlas (one pass per cascade, each in its viewport)
        Barrier(list.Get(), atlas.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv(1));
        list->ClearDepthStencilView(dsv(1), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        for (UINT k = 0; k < kCascades; k++)
        {
            const D3D12_VIEWPORT vp = { 0, float(k * kAtlas), float(kAtlas), float(kAtlas), 0, 1 };
            const D3D12_RECT sr = { 0, LONG(k * kAtlas), LONG(kAtlas), LONG((k + 1) * kAtlas) };
            list->RSSetViewports(1, &vp);
            list->RSSetScissorRects(1, &sr);
            const UINT pass[4] = { k, 0, 0, 0 };
            list->SetGraphicsRoot32BitConstants(2, 4, pass, 0);
            drawScene(shadowPso.Get(), shadowInstPso.Get(), true);
        }
        Barrier(list.Get(), atlas.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        stamp(1);
        // 2. G-buffer at render resolution
        for (int i = 0; i < 5; i++)
            Barrier(list.Get(), gbuffer[i].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Barrier(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CPU_DESCRIPTOR_HANDLE gtargets[5] = { rtv(0), rtv(1), rtv(2), rtv(3), rtv(4) };
        list->OMSetRenderTargets(5, gtargets, FALSE, &dsv(0));
        const float zero[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 5; i++)
            list->ClearRenderTargetView(gtargets[i], zero, 0, nullptr);
        list->ClearDepthStencilView(dsv(0), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT rvp = { 0, 0, float(kRenderW), float(kRenderH), 0, 1 };
        const D3D12_RECT rsr = { 0, 0, LONG(kRenderW), LONG(kRenderH) };
        list->RSSetViewports(1, &rvp);
        list->RSSetScissorRects(1, &rsr);
        drawScene(gbufferPso.Get(), gbufferInstPso.Get());
        for (int i = 0; i < 5; i++)
            Barrier(list.Get(), gbuffer[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 2b. ground truth: the scene mirrored under the floor (without the floor itself)
        if (truth)
        {
            Barrier(list.Get(), mirror.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->OMSetRenderTargets(1, &rtv(9), FALSE, &dsv(3));
            list->ClearRenderTargetView(rtv(9), zero, 0, nullptr);
            list->ClearDepthStencilView(dsv(3), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            list->SetGraphicsRootDescriptorTable(3, srvGpu(0));
            list->SetPipelineState(mirrorPso.Get());
            list->IASetVertexBuffers(0, 1, &vbv);
            for (size_t i = 1; i < scene.size(); i++)
            {
                list->SetGraphicsRootConstantBufferView(1, objectBuf->GetGPUVirtualAddress() + i * sizeof(ObjectConstants));
                list->DrawIndexedInstanced(geo.meshes[scene[i].mesh].indexCount, 1, geo.meshes[scene[i].mesh].firstIndex, 0, 0);
            }
            list->SetPipelineState(mirrorInstPso.Get());
            const D3D12_VERTEX_BUFFER_VIEW views[2] = { vbv, instView };
            list->IASetVertexBuffers(0, 2, views);
            list->SetGraphicsRootConstantBufferView(1, objectBuf->GetGPUVirtualAddress() + scene.size() * sizeof(ObjectConstants));
            list->DrawIndexedInstanced(indirect.IndexCountPerInstance, indirect.InstanceCount, indirect.StartIndexLocation, 0, 0);
            Barrier(list.Get(), mirror.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        stamp(2);
        // 3. half-resolution depth (like an ambient-occlusion pass)
        list->OMSetRenderTargets(0, nullptr, FALSE, &dsv(2));
        list->ClearDepthStencilView(dsv(2), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        const D3D12_VIEWPORT hvp = { 0, 0, float(kRenderW / 2), float(kRenderH / 2), 0, 1 };
        const D3D12_RECT hsr = { 0, 0, LONG(kRenderW / 2), LONG(kRenderH / 2) };
        list->RSSetViewports(1, &hvp);
        list->RSSetScissorRects(1, &hsr);
        list->SetPipelineState(halfPso.Get());
        list->IASetVertexBuffers(0, 1, &vbv);
        for (size_t i = 0; i < scene.size(); i += 3) // part of the scene is enough
        {
            list->SetGraphicsRootConstantBufferView(1, objectBuf->GetGPUVirtualAddress() + i * sizeof(ObjectConstants));
            list->DrawIndexedInstanced(geo.meshes[scene[i].mesh].indexCount, 1, geo.meshes[scene[i].mesh].firstIndex, 0, 0);
        }

        stamp(3);
        // 4. full-screen lighting (reads the G-buffer and the atlas)
        Barrier(list.Get(), lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &rtv(5), FALSE, nullptr);
        list->RSSetViewports(1, &rvp);
        list->RSSetScissorRects(1, &rsr);
        list->SetPipelineState(lightingPso.Get());
        list->SetGraphicsRootDescriptorTable(3, srvGpu(0));
        list->DrawInstanced(3, 1, 0, 0);
        Barrier(list.Get(), lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        stamp(4);
        // 5. TAA and upscaling to the screen, then HDR10 or sRGB output
        const int k = frame & 1;
        const D3D12_VIEWPORT dvp = { 0, 0, float(kWidth), float(kHeight), 0, 1 };
        const D3D12_RECT dsr = { 0, 0, LONG(kWidth), LONG(kHeight) };
        list->RSSetViewports(1, &dvp);
        list->RSSetScissorRects(1, &dsr);
        Barrier(list.Get(), taaHistory[k].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &rtv(7 + k), FALSE, nullptr);
        list->SetPipelineState(taaPso.Get());
        list->SetGraphicsRootDescriptorTable(3, srvGpu(10 + 5 * k));
        list->DrawInstanced(3, 1, 0, 0);
        Barrier(list.Get(), taaHistory[k].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(list.Get(), finalTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &rtv(6), FALSE, nullptr);
        list->SetPipelineState(upscalePso.Get());
        list->SetGraphicsRootDescriptorTable(3, srvGpu(20 + 5 * k));
        list->DrawInstanced(3, 1, 0, 0);
        Barrier(list.Get(), finalTex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        stamp(5);
        // 6. copy into the presented image
        ID3D12Resource* bb = backBuffers[swapchain->GetCurrentBackBufferIndex()].Get();
        Barrier(list.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(bb, finalTex.Get());
        if (ui)
        {
            // 7. the game's UI, drawn over the scene into the screen image
            Barrier(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const D3D12_CPU_DESCRIPTOR_HANDLE bbv = rtv(10 + swapchain->GetCurrentBackBufferIndex());
            list->OMSetRenderTargets(1, &bbv, FALSE, nullptr);
            list->RSSetViewports(1, &dvp);
            list->RSSetScissorRects(1, &dsr);
            list->SetPipelineState(uiPso.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            auto rect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
                const float c[8] = { x0, y0, x1, y1, r, g, b, a };
                list->SetGraphicsRoot32BitConstants(2, 8, c, 4);
                list->DrawInstanced(4, 1, 0, 0);
            };
            // values already encoded for the screen (PQ in HDR10: 0.58 ~ 200 nits)
            rect(-0.95f, -0.55f, -0.65f, -0.95f, 0.25f, 0.22f, 0.18f, 0.85f); // mini-carte
            rect(-0.92f, -0.6f, -0.68f, -0.62f, 0.62f, 0.6f, 0.55f, 1.0f);
            rect(-0.95f, 0.92f, -0.55f, 0.89f, 0.6f, 0.15f, 0.12f, 1.0f); // health and stamina bars
            rect(-0.95f, 0.87f, -0.62f, 0.84f, 0.55f, 0.55f, 0.55f, 1.0f);
            if (std::fmod(t, 4.0f) > 2.0f) // menu ouvert
            {
                rect(-0.45f, 0.6f, 0.45f, -0.6f, 0.05f, 0.05f, 0.06f, 0.75f);
                for (int k = 0; k < 6; k++)
                    rect(-0.38f, 0.48f - k * 0.17f, 0.2f - 0.05f * k, 0.43f - k * 0.17f, 0.62f, 0.6f, 0.52f, 1.0f);
            }
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            Barrier(list.Get(), bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        else
            Barrier(list.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        stamp(6);
        list->ResolveQueryData(stampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, kStamps, stampBuf.Get(), 0);
        CHECK(list->Close());
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        LARGE_INTEGER p0, p1, p2;
        QueryPerformanceCounter(&p0);
        CHECK(swapchain->Present(0, 0));
        QueryPerformanceCounter(&p1);
        waitGpu();
        QueryPerformanceCounter(&p2);
        if (frame >= 30)
            presentSum += double(p1.QuadPart - p0.QuadPart) / freq.QuadPart * 1000, waitSum += double(p2.QuadPart - p1.QuadPart) / freq.QuadPart * 1000;
        if (frame >= 30)
        {
            const D3D12_RANGE range = { 0, kStamps * 8 };
            UINT64* t = nullptr;
            if (SUCCEEDED(stampBuf->Map(0, &range, (void**)&t)))
            {
                for (UINT i = 0; i + 1 < kStamps; i++)
                    stampSum[i] += double(t[i + 1] - t[i]) * 1000.0 / gpuFreq;
                stampFrames++;
                const D3D12_RANGE none = { 0, 0 };
                stampBuf->Unmap(0, &none);
            }
        }

        // frame generation: a present inserted without any draw (copy of the last image)
        if (frameGen)
        {
            CHECK(allocator->Reset());
            CHECK(list->Reset(allocator.Get(), nullptr));
            ID3D12Resource* gb = backBuffers[swapchain->GetCurrentBackBufferIndex()].Get();
            Barrier(list.Get(), gb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(gb, finalTex.Get());
            Barrier(list.Get(), gb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            CHECK(list->Close());
            queue->ExecuteCommandLists(1, lists);
            CHECK(swapchain->Present(0, 0));
            waitGpu();
        }

        prevViewProjNJ = viewProjNJ;
        for (Object& o : scene)
            o.prevWorld = o.world;
    }
    waitGpu();
    QueryPerformanceCounter(&tickNow);
    if (frames > 30)
        std::printf("urti_sandbox: %.1f frames per second (excluding startup)\n", (frames - 30) / (double(tickNow.QuadPart - tick0.QuadPart) / freq.QuadPart));
    if (stampFrames > 0)
    {
        std::printf("urti_sandbox: CPU per frame (ms): present %.2f, GPU wait %.2f\n", presentSum / stampFrames, waitSum / stampFrames);
        std::printf("urti_sandbox: GPU per frame (ms):");
        for (UINT i = 0; i + 1 < kStamps; i++)
            std::printf(" %s %.2f%s", stampNames[i], stampSum[i] / stampFrames, i + 2 < kStamps ? "," : "\n");
    }
    std::printf("urti_sandbox: done\n");
    return 0;
}
