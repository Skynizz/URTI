// Live URTI: filling of the world radiance cache (live_radhash.hlsli) from the image the game shows.
// Mode 0, on every computed frame: one render pixel in four (in turn) deposits its colour into the world cell where it
// is, at three levels (its own and two coarser ones: a reflection reads it back from further away); the sky and the distance go into the sky
// map, by direction. One pixel in sixteen also casts the camera ray into the RT scene: each triangle crossed before the
// surface the image shows is a hole (cut-out foliage card), the one that falls at its depth is solid.
// Mode 1: the sky map spreads step by step to directions never seen, and ages.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer image : register(t1);       // presented image, without the UI if the game provides it (aligned rows)
ByteAddressBuffer depth : register(t2);       // render depth (this frame)
ByteAddressBuffer sunBuf : register(t3);      // depth law (36 to 47), sub-pixel jitter (1184)
ByteAddressBuffer reflection : register(t4);  // live_reflect: non-zero weight = reflecting surface (water), whose colour is not its own
ByteAddressBuffer radParams : register(t5);
RWByteAddressBuffer radHash : register(u0);
RWByteAddressBuffer envMap : register(u1);
RWByteAddressBuffer radStats : register(u2);  // flags 2: colours deposited, refused (table full), holes, solids, sky, cells created, cells reclaimed

cbuffer Constants : register(b0)
{
    uint width;        // presented image
    uint height;
    uint pitch;
    uint format;       // 0 RGBA8, 1 BGRA8, 2 HDR10 PQ, 3 RGBA16F
    uint renderWidth;
    uint renderHeight;
    uint depthPitch;
    uint depth24;
    float p00;
    float p11;
    uint mode;
    uint flags;        // 1: this frame's RT scene available (solid share learned); 2: counters
    float uiX0;        // fixed UI zone (uv), skipped
    float uiY0;
    float uiX1;
    float uiY1;
    float maxLuminance;
};

#define RAD_BUF radHash
#define RAD_ENV envMap
#define RAD_PARAMS radParams
#include "live_radhash.hlsli"

float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

float3 LoadLinear(int2 px)
{
    px = clamp(px, int2(0, 0), int2(width - 1, height - 1));
    const uint a = uint(px.y) * pitch + uint(px.x) * (format == 3 ? 8 : 4);
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

float3 SampleLinear(float2 uv)
{
    const float2 q = uv * float2(width, height) - 0.5;
    const int2 b = int2(floor(q));
    const float2 f = q - b;
    return lerp(lerp(LoadLinear(b), LoadLinear(b + int2(1, 0)), f.x), lerp(LoadLinear(b + int2(0, 1)), LoadLinear(b + int2(1, 1)), f.x), f.y);
}

float2 JitterPixels(float2 ab, float2 size) { return float2(ab.x * p00, -ab.y * p11) * 0.5 * size; }

static float gA, gB, gClear;

// point seen by a render pixel; z = -1: off screen; z = 0: the sky
float3 PointAt(int2 t)
{
    if (t.x < 0 || t.y < 0 || t.x >= int(renderWidth) || t.y >= int(renderHeight))
        return float3(0, 0, -1);
    const uint raw = depth.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float w = gB / (d - gA);
    if (d == gClear || !(w > 0 && w < 1e6))
        return float3(0, 0, 0);
    const float2 ndc = float2((t.x + 0.5) / renderWidth * 2 - 1, 1 - (t.y + 0.5) / renderHeight * 2);
    return float3(ndc.x * w / p00, ndc.y * w / p11, w);
}

void Count(uint at)
{
    if (flags & 2)
    {
        uint unused;
        radStats.InterlockedAdd(at, 1, unused);
    }
}

// place of the cell (bytes), taken if it does not exist; table full at that place: the oldest of the four is reclaimed if it
// has not been used for two seconds; otherwise -1
int RadClaim(float3 world, uint level)
{
    uint bucket, tag;
    RadKey(world, level, bucket, tag);
    uint i;
    for (i = 0; i < 4; i++)
        if (radHash.Load((bucket + i) * 16) == tag)
            return int((bucket + i) * 16);
    for (i = 0; i < 4; i++)
    {
        uint prev;
        radHash.InterlockedCompareExchange((bucket + i) * 16, 0u, tag, prev);
        if (prev == 0u)
            Count(20);
        if (prev == 0u || prev == tag)
            return int((bucket + i) * 16);
    }
    uint worst = 0, at = 0, key = 0;
    for (i = 0; i < 4; i++)
    {
        const uint3 v = radHash.Load3((bucket + i) * 16);
        const uint age = (gRadTick - (v.z >> 16)) & 0xFFFF;
        if (age >= worst)
            worst = age, at = (bucket + i) * 16, key = v.x;
    }
    if (worst < 8)
        return -1;
    uint prev;
    radHash.InterlockedCompareExchange(at, key, tag, prev);
    if (prev != key)
        return prev == tag ? int(at) : -1;
    radHash.Store3(at + 4, uint3(0, 0, 0));
    Count(24);
    return int(at);
}

// colour seen at this point (average of the first measurements, then running); solid: the RT scene really has a solid triangle here
void RadColor(float3 world, uint level, float3 c, bool solid)
{
    if (level >= kRadLevels)
        return;
    const int at = RadClaim(world, level);
    if (at < 0)
    {
        Count(4);
        return;
    }
    const uint3 v = radHash.Load3(uint(at) + 4);
    const uint nC = v.z & 255;
    uint nA = (v.z >> 16) & 255;
    float alpha = float((v.z >> 8) & 255) / 255.0;
    if (nC > 0)
    {
        const float3 old = float3(f16tof32(v.x & 0xFFFF), f16tof32(v.x >> 16), f16tof32(v.y & 0xFFFF));
        if (all(isfinite(old)))
            c = lerp(old, c, max(1.0 / float(nC + 1), 0.125));
    }
    if (solid)
    {
        alpha = nA > 0 ? lerp(alpha, 1.0, max(1.0 / float(nA + 1), 0.0625)) : 1.0;
        nA = min(nA + 1, 255u);
    }
    radHash.Store3(uint(at) + 4, uint3(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | ((gRadTick & 0xFFFF) << 16),
                                       min(nC + 1, 255u) | (uint(round(saturate(alpha) * 255.0)) << 8) | (nA << 16)));
    Count(0);
}

// an RT-scene triangle crossed here by the view: a hole
void RadHole(float3 world, uint level)
{
    if (level >= kRadLevels)
        return;
    const int at = RadClaim(world, level);
    if (at < 0)
        return;
    const uint2 v = radHash.Load2(uint(at) + 8);
    const uint nA = (v.y >> 16) & 255;
    const float alpha = nA > 0 ? lerp(float((v.y >> 8) & 255) / 255.0, 0.0, max(1.0 / float(nA + 1), 0.0625)) : 0.0;
    radHash.Store2(uint(at) + 8, uint2((v.x & 0xFFFF) | ((gRadTick & 0xFFFF) << 16), (v.y & 255) | (uint(round(saturate(alpha) * 255.0)) << 8) | (min(nA + 1, 255u) << 16)));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    RadLoadParams();
    if (mode == 1)
    {
        // sky map: each direction ages; a direction never seen, or seen long ago, takes the average of its neighbours
        // more certain than itself (the known sky spreads step by step to the whole map)
        if (id.x >= gEnvSize || id.y >= gEnvSize)
            return;
        const uint at = (id.y * gEnvSize + id.x) * 16;
        float4 v = asfloat(envMap.Load4(at));
        if (!all(isfinite(v)))
            v = 0;
        v.a *= 0.9997;
        if (v.a < 0.5)
        {
            float3 s = 0;
            float w = 0, best = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (dx == 0 && dy == 0)
                        continue;
                    const int2 t = clamp(int2(id) + int2(dx, dy), int2(0, 0), int2(gEnvSize - 1, gEnvSize - 1));
                    const float4 n = asfloat(envMap.Load4((uint(t.y) * gEnvSize + uint(t.x)) * 16));
                    if (n.a > 0 && all(isfinite(n)))
                        s += n.rgb * n.a, w += n.a, best = max(best, n.a);
                }
            if (w > 0 && best * 0.85 > v.a)
                v = float4(s / w, best * 0.85);
        }
        envMap.Store4(at, asuint(v));
        return;
    }
    // one pixel per 2×2 block, in turn
    static const uint2 kFour[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
    const int2 t = int2(id * 2 + kFour[(gRadFrame >> 1) & 3]);
    if (t.x >= int(renderWidth) || t.y >= int(renderHeight))
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.x, gB = law.y;
    gClear = gB > 0 ? 0.0 : 1.0;
    if (!(law.z > 0))
        return;
    const float3 p = PointAt(t);
    const float2 jitterUv = JitterPixels(asfloat(sunBuf.Load2(1184)), float2(renderWidth, renderHeight)) / float2(renderWidth, renderHeight);
    const float2 uv = (float2(t) + 0.5) / float2(renderWidth, renderHeight) - jitterUv;
    if (all(uv >= float2(uiX0, uiY0)) && all(uv <= float2(uiX1, uiY1)))
        return;
    if (any(uv < 0.002) || any(uv > 0.998))
        return;
    float3 c = SampleLinear(uv);
    if (any(!isfinite(c)))
        return;
    c = max(c, 0.0);
    const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (lum > maxLuminance)
        c *= maxLuminance / lum;
    // sky and distance (beyond the rays' range): by direction
    if (p.z == 0 || p.z > gEnvFar)
    {
        const float2 ndc = float2((t.x + 0.5) / renderWidth * 2 - 1, 1 - (t.y + 0.5) / renderHeight * 2);
        const float3 dir = RadWorldDir(float3(ndc.x / p00, ndc.y / p11, 1.0));
        const int2 e = clamp(int2(RadOct(RadEnvBasis(dir)) * float(gEnvSize)), int2(0, 0), int2(gEnvSize - 1, gEnvSize - 1));
        const uint at = (uint(e.y) * gEnvSize + uint(e.x)) * 16;
        const float4 old = asfloat(envMap.Load4(at));
        envMap.Store4(at, asuint(float4(old.a > 0.5 && all(isfinite(old)) ? lerp(old.rgb, c, 0.2) : c, 1.0)));
        Count(16);
        if (p.z == 0)
            return;
    }
    if (p.z <= 0)
        return;
    // reflecting surface (water): what the image shows there is a reflection, not its colour
    if (asfloat(reflection.Load((uint(t.y) * renderWidth + uint(t.x)) * 16 + 8)) != 0)
        return;
    // object edge against the sky or against a much further object: the image's colour mixes two surfaces there (upscaling, anti-aliasing)
    for (uint j = 0; j < 4; j++)
    {
        const int2 o = j == 0 ? int2(1, 0) : j == 1 ? int2(-1, 0) : j == 2 ? int2(0, 1) : int2(0, -1);
        const float3 n = PointAt(t + o);
        if (n.z <= 0 || abs(n.z - p.z) > 0.03 * p.z + 0.05)
            return;
    }
    const float dist = length(p);
    bool solid = false;
    if ((flags & 1) && ((id.x & 1) | ((id.y & 1) << 1)) == ((gRadFrame >> 3) & 3))
    {
        RayDesc eye;
        eye.Origin = 0;
        eye.Direction = p / dist;
        eye.TMin = 0.0;
        eye.TMax = dist * 1.03 + 0.1;
        const float tol = 0.03 * dist + 0.05;
        for (uint hop = 0; hop < 4; hop++)
        {
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, eye);
            q.Proceed();
            if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                break;
            const float th = q.CommittedRayT();
            if (abs(th - dist) <= tol)
            {
                solid = true;
                Count(12);
                break;
            }
            if (th > dist)
                break;
            const float3 hw = RadWorld(eye.Direction * th);
            const uint lh = RadLevel(th);
            for (uint k = 0; k < 3; k++)
                RadHole(hw, lh + k);
            Count(8);
            eye.TMin = th + max(1e-3, 1e-4 * th);
        }
    }
    const float3 world = RadWorld(p);
    const uint level = RadLevel(dist);
    for (uint k = 0; k < 3; k++)
        RadColor(world, level + k, c, solid);
}
