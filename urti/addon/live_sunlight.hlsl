// Live URTI: share of the sun's direct light in the game's image, measured every frame.
// A shadow removes only sunlight: what remains (sky, bounces) depends on the scene (noon,
// evening, overcast) and on its colour. It is read in the image itself: two points of the same surface,
// one the game lights, the other it shadows (its shadow map), have a colour ratio of
// sky / (sky + sun × N·L). Hence x = sun / sky = (lit / shadowed - 1) / N·L, per channel and in
// luminance, accumulated in a histogram (log2 of x, quarter octaves from 2^-5 to 2^5): the median, read on
// the CPU, gives the share of the sun that RT shadows remove (live_shadow) and their tint
// (live_composite). One thread per 4×4 block of render pixels, one point of the block drawn at random.

ByteAddressBuffer depth : register(t0);   // copy of the depth (32 bits per texel, aligned rows)
ByteAddressBuffer atlas : register(t1);   // copy of the game's shadow map
ByteAddressBuffer image : register(t2);   // presented image (snapshot before the UI if it exists)
RWByteAddressBuffer sunBuf : register(u0); // sun, cascades, sub-pixel jitter; histogram at 1232

cbuffer Constants : register(b0)
{
    uint width;        // render resolution (depth)
    uint height;
    uint depthPitch;
    uint depth24;
    float A;
    float B;
    float p00;
    float p11;
    float sunSign;     // +1: the map's depth grows away from the sun
    float gameBias;
    uint atlasPitch;
    uint atlasFormat;  // 0 32-bit floats, 1 D24, 2 16-bit normalised (RDR2)
    uint imageWidth;   // presented image
    uint imageHeight;
    uint imagePitch;
    uint format;       // 0 RGBA8, 1 BGRA8, 2 RGB10A2 en HDR10 (PQ), 3 RGBA16F (scRGB)
    uint frameIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};

static const uint kCount = 1232;  // number of pairs measured
static const uint kHist = 1248;   // 4 × 40 cases : rouge, vert, bleu, luminance
static const uint kBins = 40;
static const uint kStats = 1888; // check: points lit, shadowed, on the edge, outside the cascades; luminance × 1024 of the first two
static const uint kDepthHist = 1920; // depth of the point in the first cascade covering it in x, y: 16 bins from -0.5 to 1.5
static const uint kGapHist = 1984;   // map - point (depth), covered points: 16 bins from -0.02 to 0.02

static float gA, gB, gClear;

float2 JitterPixels(float2 ab, float2 size) { return float2(ab.x * p00, -ab.y * p11) * 0.5 * size; }

float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

// linear light of the presented image (same decoding as live_composite)
float3 LoadLinear(int2 px)
{
    px = clamp(px, int2(0, 0), int2(imageWidth - 1, imageHeight - 1));
    const uint a = uint(px.y) * imagePitch + uint(px.x) * (format == 3 ? 8 : 4);
    if (format == 3)
    {
        const uint2 raw = image.Load2(a);
        return float3(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF));
    }
    const uint raw = image.Load(a);
    if (format == 2)
        return PqToNits(float3(raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023) / 1023.0) / 203.0;
    const float3 c = float3(raw & 255, (raw >> 8) & 255, (raw >> 16) & 255) / 255.0;
    return SrgbToLinear(format == 1 ? c.bgr : c);
}

// camera-space point of the pixel (z = w; w <= 0 if sky or outside the image)
float3 PointAt(int2 t)
{
    if (t.x < 0 || t.y < 0 || t.x >= int(width) || t.y >= int(height))
        return float3(0, 0, -1);
    const uint raw = depth.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float w = gB / (d - gA);
    if (d == gClear || !(w > 0 && w < 1e6))
        return float3(0, 0, -1);
    const float2 ndc = float2((t.x + 0.5) / width * 2 - 1, 1 - (t.y + 0.5) / height * 2);
    return float3(ndc.x * w / p00, ndc.y * w / p11, w);
}

// depth-derived normal (like live_shadow)
float3 NormalAt(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(1, 0)), r = PointAt(t + int2(1, 0)), u = PointAt(t - int2(0, 1)), b = PointAt(t + int2(0, 1));
    const float3 dx = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
    const float3 dy = (b.z > 0 && (u.z <= 0 || abs(b.z - p.z) < abs(u.z - p.z))) ? b - p : p - u;
    float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n;
}

float AtlasDepth(uint2 t)
{
    if (atlasFormat == 2)
    {
        const uint a = t.y * atlasPitch + t.x * 2;
        const uint raw = atlas.Load(a & ~3u);
        return float((a & 2) ? raw >> 16 : raw & 0xFFFF) / 65535.0;
    }
    const uint raw = atlas.Load(t.y * atlasPitch + t.x * 4);
    return atlasFormat == 1 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
}

// state of the point in the game's shadow map: 1 lit, 0 shadowed (the whole 5×5 neighbourhood in the same
// state: far from blurred edges and penumbra), -1 edge or outside the cascades
int GameState(float3 p)
{
    const uint count = min(sunBuf.Load(32), 8u);
    bool record = true;
    for (uint k = 0; k < count; k++)
    {
        const uint base = 48 + k * 64;
        const float4 m0 = asfloat(sunBuf.Load4(base)), m1 = asfloat(sunBuf.Load4(base + 16)), m2 = asfloat(sunBuf.Load4(base + 32));
        const float4 vp = asfloat(sunBuf.Load4(base + 48));
        const float M[12] = { m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w };
        float3 s;
        for (uint c = 0; c < 3; c++)
            s[c] = M[9 + c] + p.x * M[c] + p.y * M[3 + c] + p.z * M[6 + c];
        if (abs(s.x) > 0.95 || abs(s.y) > 0.95)
            continue;
        // check: where the point's depth falls in the cascade, and the difference to the map (for a lit
        // point, the map records the point itself: zero difference up to the game's bias)
        if (record)
            sunBuf.InterlockedAdd(kDepthHist + uint(clamp(floor((s.z + 0.5) * 8.0), 0.0, 15.0)) * 4, 1), record = false;
        if (s.z <= 0 || s.z >= 1)
            continue;
        const float2 texel = vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw;
        if (count > 0)
            sunBuf.InterlockedAdd(kGapHist + uint(clamp(floor((AtlasDepth(uint2(texel)) - s.z + 0.02) * 400.0), 0.0, 15.0)) * 4, 1);
        uint lit = 0;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
            {
                const float stored = AtlasDepth(uint2(int2(texel) + int2(dx, dy)));
                lit += (sunSign > 0 ? s.z <= stored + gameBias : s.z >= stored - gameBias) ? 1u : 0u;
            }
        return lit == 25 ? 1 : lit == 0 ? 0 : -1;
    }
    return -2; // outside the cascades
}

uint Hash(uint x)
{
    x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
    return x;
}

// colour of the presented image at the point seen by render pixel t (2×2 average: less texture noise)
float3 ColorAt(int2 t, float2 jitter)
{
    const float2 q = (float2(t) + 0.5 - jitter) / float2(width, height) * float2(imageWidth, imageHeight) - 0.5;
    const int2 b = int2(floor(q));
    return 0.25 * (LoadLinear(b) + LoadLinear(b + int2(1, 0)) + LoadLinear(b + int2(0, 1)) + LoadLinear(b + int2(1, 1)));
}

void Record(uint channel, float shadowed, float lit, float ndl)
{
    if (lit < 0.01)
        return; // too dark for a reliable ratio
    const float x = (lit / max(shadowed, 1e-4) - 1.0) / ndl;
    const uint bin = x <= 0.03125 ? 0u : uint(clamp(floor((log2(x) + 5.0) * 4.0), 0.0, float(kBins - 1)));
    sunBuf.InterlockedAdd(kHist + (channel * kBins + bin) * 4, 1);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x * 4 >= width || id.y * 4 >= height)
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0;
    const float3 sun = asfloat(sunBuf.Load3(0));
    const uint h = Hash(id.x * 7919 + id.y * 104729 + frameIndex * 2654435761u);
    const int2 t = int2(id * 4) + int2(h & 3, (h >> 2) & 3);
    const float3 p = PointAt(t);
    // in the distance, the game's haze mixes into the colour: no measurement
    if (p.z <= 0 || p.z > 100)
        return;
    const float3 n = NormalAt(t, p);
    const float ndl = dot(n, sun);
    if (ndl < 0.2)
        return; // grazing surface or facing away from the sun: the ratio says little there
    const int state = GameState(p);
    // check of the map read: if it really is the sun's, the points it lights are clearly
    // brighter in the image than those it shadows
    sunBuf.InterlockedAdd(kStats + (state == 1 ? 0 : state == 0 ? 4 : state == -1 ? 8 : 12), 1);
    if (state >= 0)
    {
        const float2 jit = JitterPixels(asfloat(sunBuf.Load2(1184)), float2(width, height));
        const float y = min(dot(ColorAt(t, jit), float3(0.2126, 0.7152, 0.0722)), 4.0);
        sunBuf.InterlockedAdd(kStats + (state == 1 ? 16 : 20), uint(y * 1024.0));
    }
    if (state < 0)
        return;
    // partner: 10 pixels away, on the same plane (same normal, at the same distance from the plane), in the other state
    static const int2 kDirs[4] = { int2(1, 0), int2(0, 1), int2(-1, 0), int2(0, -1) };
    for (uint k = 0; k < 4; k++)
    {
        const int2 u = t + kDirs[(k + (h >> 4)) & 3] * 10;
        const float3 q = PointAt(u);
        if (q.z <= 0 || abs(dot(n, q - p)) > 0.01 * p.z + 0.02 || dot(NormalAt(u, q), n) < 0.97)
            continue;
        const int other = GameState(q);
        if (other < 0 || other == state)
            continue;
        const float2 jitter = JitterPixels(asfloat(sunBuf.Load2(1184)), float2(width, height));
        const float3 cp = ColorAt(t, jitter), cq = ColorAt(u, jitter);
        const float3 lit = state == 1 ? cp : cq, shadowed = state == 1 ? cq : cp;
        const float3 kLum = float3(0.2126, 0.7152, 0.0722);
        for (uint c = 0; c < 3; c++)
            Record(c, shadowed[c], lit[c], ndl);
        Record(3, dot(shadowed, kLum), dot(lit, kLum), ndl);
        sunBuf.InterlockedAdd(kCount, 1);
        return;
    }
}
