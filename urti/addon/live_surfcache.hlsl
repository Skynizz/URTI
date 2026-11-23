// Live URTI: surface cache. Every frame, one render pixel per 4×4 block (in turn: the whole
// screen in 16 frames): the RT-scene triangle the camera sees there is found (ray from the
// camera, kept if it falls at the image's depth) and the colour the image shows at that place is
// kept for it (running average). A reflection hitting that triangle while it is hidden or off screen
// takes that colour (live_reflect) instead of staying unknown.
// Per group of eight consecutive triangles (neighbours in the mesh; in RDR2 most triangles are
// smaller than a pixel), 8 bytes: linear colour (RGB9E5); identity of its object (24 bits) and number of
// measurements (8 bits). Fixed places keep an object at the same place in the stream: its triangles keep their number.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer image : register(t1);         // presented image (aligned rows)
ByteAddressBuffer depth : register(t2);         // render depth (this frame)
ByteAddressBuffer sunBuf : register(t3);        // depth law (36 to 47), sub-pixel jitter (1184)
StructuredBuffer<uint4> regions : register(t4); // per screen draw: start (bytes), size, counter, identity
RWByteAddressBuffer cache : register(u0);
RWByteAddressBuffer geoColors : register(u1);   // geometry-cache objects: 256 colours per place (RGB9E5), generation and measurements

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
    uint regionCount;
    uint frameIndex;
    uint triangles;    // triangles the cache can hold
    uint geoColorsOn;  // 1: colours of geometry-cache objects learned too
    float uiX0;        // fixed UI zone (uv), skipped; four numbers: a float4 here would be moved to the
    float uiY0;        // next 16-byte boundary, further than what the add-on writes
    float uiX1;
    float uiY1;
};

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

// linear colour <-> RGB9E5 (shared exponent, like DXGI_FORMAT_R9G9B9E5_SHAREDEXP)
uint PackRgb9e5(float3 c)
{
    c = clamp(c, 0.0, 65408.0);
    const float m = max(c.r, max(c.g, c.b));
    int e = max(-16, int(floor(log2(max(m, 1e-30))))) + 16;
    float denom = exp2(float(e - 24));
    if (floor(m / denom + 0.5) >= 512.0)
        denom *= 2.0, e += 1;
    const uint3 q = uint3(min(floor(c / denom + 0.5), 511.0));
    return q.r | (q.g << 9) | (q.b << 18) | (uint(e) << 27);
}
float3 UnpackRgb9e5(uint x) { return float3(x & 511, (x >> 9) & 511, (x >> 18) & 511) * exp2(float(int(x >> 27) - 24)); }

// the game's sub-pixel jitter (same computation as live_reflresolve): the depth is jittered, not the presented image
float2 JitterPixels(float2 ab, float2 size) { return float2(ab.x * p00, -ab.y * p11) * 0.5 * size; }

static float gA, gB, gClear;

float3 PointAt(int2 t)
{
    if (t.x < 0 || t.y < 0 || t.x >= int(renderWidth) || t.y >= int(renderHeight))
        return float3(0, 0, -1);
    const uint raw = depth.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float w = gB / (d - gA);
    if (d == gClear || !(w > 0 && w < 1e6))
        return float3(0, 0, -1);
    const float2 ndc = float2((t.x + 0.5) / renderWidth * 2 - 1, 1 - (t.y + 0.5) / renderHeight * 2);
    return float3(ndc.x * w / p00, ndc.y * w / p11, w);
}

// draw that holds this triangle (regions sorted by start)
uint RegionIndex(uint prim)
{
    const uint byte = prim * 48;
    uint lo = 0, hi = regionCount;
    while (lo + 1 < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (regions[mid].x <= byte)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    // one pixel per 4×4 block, Bayer order: sixteen frames cover the whole block
    static const uint kOrder[16] = { 0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12 };
    const uint k = kOrder[frameIndex & 15];
    const int2 t = int2(id * 4 + uint2(k & 3, k >> 2));
    if (t.x >= int(renderWidth) || t.y >= int(renderHeight) || regionCount == 0)
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.x, gB = law.y;
    gClear = gB > 0 ? 0.0 : 1.0;
    if (!(law.z > 0))
        return;
    const float3 p = PointAt(t);
    if (p.z <= 0)
        return;
    // object edge: the image's colour mixes two surfaces there (upscaling, anti-aliasing)
    for (uint j = 0; j < 4; j++)
    {
        const int2 o = j == 0 ? int2(1, 0) : j == 1 ? int2(-1, 0) : j == 2 ? int2(0, 1) : int2(0, -1);
        const float3 n = PointAt(t + o);
        if (n.z <= 0 || abs(n.z - p.z) > 0.03 * p.z + 0.05)
            return;
    }
    const float2 jitterUv = JitterPixels(asfloat(sunBuf.Load2(1184)), float2(renderWidth, renderHeight)) / float2(renderWidth, renderHeight);
    const float2 uv = (float2(t) + 0.5) / float2(renderWidth, renderHeight) - jitterUv;
    if (all(uv >= float2(uiX0, uiY0)) && all(uv <= float2(uiX1, uiY1)))
        return;
    RayDesc eye;
    eye.Origin = 0;
    eye.Direction = normalize(p);
    eye.TMin = 0.0;
    eye.TMax = length(p) * 1.05 + 0.1;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, eye);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT || abs(q.CommittedRayT() - length(p)) > 0.02 * length(p) + 0.05)
        return; // the camera sees something other than the RT scene here (object out of budget, hole of a cut-out card)
    const uint inst = q.CommittedInstanceID();
    if (inst != 0)
    {
        // geometry-cache object (bit 23: place, generation): the colour of its group of triangles (256 per object, neighbouring triangles in the mesh)
        if ((inst & 0x800000u) == 0 || geoColorsOn == 0)
            return;
        float3 cg = SampleLinear(uv);
        if (any(!isfinite(cg)))
            return;
        const uint slot = inst & 0x3FFFu, gen = (inst >> 14) & 255u;
        const uint tris = max(1u, q.CommittedInstanceContributionToHitGroupIndex());
        const uint bin = min(255u, uint(float(q.CommittedPrimitiveIndex()) / float(tris) * 256.0));
        const uint at = (slot * 256u + bin) * 8u;
        const uint2 was = geoColors.Load2(at);
        const uint m = (was.y >> 8) == gen ? (was.y & 255u) : 0u;
        if (m > 0)
            cg = lerp(UnpackRgb9e5(was.x), cg, max(1.0 / float(m + 1), 0.2));
        geoColors.Store2(at, uint2(PackRgb9e5(cg), (gen << 8) | min(m + 1, 255u)));
        return;
    }
    const uint prim = q.CommittedPrimitiveIndex();
    if (prim >= triangles)
        return;
    const uint4 r = regions[RegionIndex(prim)];
    if (prim * 48 < r.x || prim * 48 - r.x >= r.y)
        return;
    float3 c = SampleLinear(uv);
    if (any(!isfinite(c)))
        return;
    // average of the first measurements, then running (0.2: follows a lighting change within about ten measurements)
    const uint ident = r.w & 0xFFFFFF;
    const uint entry = prim >> 3;
    const uint2 old = cache.Load2(entry * 8);
    const uint n = (old.y >> 8) == ident ? (old.y & 255) : 0;
    if (n > 0)
        c = lerp(UnpackRgb9e5(old.x), c, max(1.0 / float(n + 1), 0.2));
    cache.Store2(entry * 8, uint2(PackRgb9e5(c), (ident << 8) | min(n + 1, 255u)));
}
