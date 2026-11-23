// Live URTI: ray-traced sun shadows and ambient occlusion (inline ray queries). One thread per pixel of the
// game's depth: point rebuilt (depth = A + B / w), rays toward the sun and the sky.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer depth : register(t1);   // copy of the depth (32 bits per texel, aligned rows)
ByteAddressBuffer sunBuf : register(t2);  // this frame's sun (live_sunvote), camera space, cascades
ByteAddressBuffer atlas : register(t3);   // copy of the game's shadow map (32 bits per texel, aligned rows)
ByteAddressBuffer prevShadow : register(t4); // previous frame's mask (accumulation)
ByteAddressBuffer depthPrev : register(t5);  // previous frame's depth
ByteAddressBuffer motion : register(t6);     // the game's motion vectors (R16G16 floats)
RWByteAddressBuffer shadow : register(u0); // 1 lit, 0 in shadow (float, aligned rows)

cbuffer Constants : register(b0)
{
    float sunSign;     // +1: the map's depth grows away from the sun
    float gameBias;    // tolerance of the depth test in the game's map
    float sunRadius;   // apparent radius of the sun (radians): penumbra
    float bias;
    uint width;
    uint height;
    uint depthPitch;   // bytes per row
    uint shadowPitch;
    float A;
    float B;
    float p00;
    float p11;
    float clearDepth;
    uint depth24;      // 1: D24S8 (24 bits of depth)
    uint atlasPitch;
    uint flags;        // 1 game map read, 2 map in D24, 4 combine with the game's shadow, 8 show the game's shadow, 16 no sun,
                       // 32 map in 16 bits (R16, RDR2), 64 no sun shadow rays (the sun stays known:
                       // ambient occlusion uses it), 128 quarter-rate occlusion (one pixel in four per frame,
                       // the others written -1: live_shadowtemporal rebuilds them), bits 16 to 18: occlusion rays
                       // per computed pixel at rest (0: fixed rule; the add-on tunes them on the pass's time),
                       // 2048: holes of cut-out cards (seen through on screen) let the sun through;
                       // 4096: output for SIGMA (NVIDIA's shadow denoiser): penumbra in penumbra, weight of the
                       // corrections (game shadow, N·L, haze) in shadow; denoising happens afterwards;
                       // 1 << 20: light bounce
    uint motionPitch;
    uint motionConvention; // 0..15 (live_mvcal), 255: no accumulation
    float temporalAlpha;   // share of the current frame (0.3: about three frames accumulated)
    uint debugMode;        // 1 raw shadow (without accumulation), 2 motion vectors, 3 history kept
    uint frameIndex;       // rotates the ray pattern from one frame to the next (accumulation averages it)
    float sunSky;          // sun / sky ratio measured in the game's image (live_sunlight); 0: not measured
    uint gameDilate;       // neighbourhood read in the game's map (radius in texels)
    uint gameMin;          // 1: filtered lit share, the lower visibility of the two; 0: game shadow widened, product
    float aoAmount;        // RT ambient occlusion: share of sky light removed by a very close occluder (0: off)
    float aoRadius;        // range of occlusion rays (metres)
    uint aoMotion;         // motion-vector convention (live_mvcal) for occlusion; 255: unknown
    uint cacheTriangles;   // triangles held by the surface cache (light bounce)
    uint regionCount;      // regions of the RT scene
    float aoMaxDist;       // time budget: occlusion computed up to this distance (metres)
    uint gnMode;           // G-buffer normals: bits 0-1 encoding (1 octahedral RG16 in camera space, 2 RGB8 * 2 - 1), bits 8-10 signs (x, y, z flipped),
                           // bit 12: occlusion uses them, bit 13: comparison with the depth normal (log)
    uint gnPitchPacked;    // bits 0-15: bytes per row of the normals copy; 16-31: of the material target (Red Dead, t10; 0: absent)
    float skyAmount;       // long-range sky visibility: share of sky light removed when an occluder hides it (0: off)
    float skyRange;        // range of those rays (metres)
};

// interleaved gradient noise (Jimenez 2014), shifted every frame by 5.588238 pixels: low discrepancy in space (two neighbouring pixels draw
// distant directions, the denoiser's blur averages them fast) and in time. White noise (hashing) left clusters of similar
// directions: grain that only averaged out over many frames (NRD's documentation requires low-discrepancy noise for occlusion)
float Ign(float2 p, uint frame)
{
    p += 5.588238 * float(frame & 63);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

uint AoHash(uint x)
{
    x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
    return x;
}

static float gA, gB, gClear;

// SIGMA: penumbra radius at the point (distance to the occluder × tangent of the sun's apparent radius);
// NRD_FP16_MAX: nothing blocks (same formula as NRD's SIGMA_FrontEnd_PackPenumbra)
static const float kFp16Max = 65504.0;
float PackPenumbra(float distanceToOccluder, float tanOfLightAngularRadius)
{
    return distanceToOccluder >= kFp16Max ? kFp16Max : min(distanceToOccluder * tanOfLightAngularRadius * 0.5, 32768.0);
}
RWByteAddressBuffer penumbra : register(u1); // float per pixel (pitch: shadowPitch), SIGMA mode
ByteAddressBuffer gnormal : register(t9);     // copy of the G-buffer normals target (gnMode)
RWByteAddressBuffer nstats : register(u3);
ByteAddressBuffer materialTarget : register(t10); // Red Dead's material target (r: type, 0.2 foliage, 0.31 skin; g: smoothness)
static uint gnPitch;    // comparison: per sign combination, sum of dot products (×1000) and number of pixels

// G-buffer normal in URTI's camera space (x right, y up, z into the scene), according to encoding and signs; 0 if absent
float3 GNormal(int2 t, uint signs)
{
    const uint enc = gnMode & 3;
    if (enc == 0 || gnPitch == 0)
        return 0;
    const uint v = gnormal.Load(t.y * gnPitch + t.x * 4);
    float3 n;
    if (enc == 1)
    {
        const float2 e = float2(v & 0xFFFF, v >> 16) / 65535.0 * 2.0 - 1.0;
        n = float3(e, 1.0 - abs(e.x) - abs(e.y));
        if (n.z < 0)
            n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    }
    else
        n = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0 * 2.0 - 1.0;
    if (signs & 1)
        n.x = -n.x;
    if (signs & 2)
        n.y = -n.y;
    if (signs & 4)
        n.z = -n.z;
    const float l = length(n);
    return l > 1e-3 && all(isfinite(n)) ? n / l : 0;
}

// G-buffer normal in URTI's camera space, ready for rays: Red Dead's (world space) rotated by the camera orientation
// that live_rotfit solved (previous frame: sunBuf 2048 to 2095, valid at 2092); 0 if absent or not solved yet
float3 GNormalCam(int2 t)
{
    float3 n = GNormal(int2(t), (gnMode >> 8) & 7);
    if ((gnMode & 3) == 2)
    {
        if (sunBuf.Load(2092) == 0)
            return 0;
        n = float3(dot(asfloat(sunBuf.Load3(2048)), n), dot(asfloat(sunBuf.Load3(2060)), n), dot(asfloat(sunBuf.Load3(2072)), n));
        const float l = length(n);
        n = l > 1e-3 && all(isfinite(n)) ? n / l : 0;
    }
    return n;
}

// light bounce (indirect lighting, bit 20 of flags): occlusion rays that hit a triangle take
// the colour the surface cache kept for it (live_surfcache): the light that surface sends back.
// Output: three planes R, G, B (float, pitch shadowPitch, one after the other), -1 on pixels not computed (quarter rate)
ByteAddressBuffer surfCache : register(t7);     // per triangle: colour (RGB9E5), object identity and measurements
StructuredBuffer<uint4> regions : register(t8); // regions of the RT scene: start (bytes), size, counter, identity
RWByteAddressBuffer bounce : register(u2);

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

// colour kept for this triangle, if it belongs to its current object.
// One entry per eight consecutive triangles (neighbours in the mesh): in RDR2, most triangles
// are smaller than a pixel and one entry per triangle was almost never filled (one occlusion ray
// in four found its colour). Empty entry or one from another object: the neighbouring entries of the same object
bool Cached(uint prim, out float3 c)
{
    c = 0;
    if (prim >= cacheTriangles || regionCount == 0)
        return false;
    const uint ident = regions[RegionIndex(prim)].w & 0xFFFFFF;
    const int g = int(prim >> 3), last = int((cacheTriangles - 1) >> 3);
    static const int kNear[5] = { 0, -1, 1, -2, 2 };
    for (uint k = 0; k < 5; k++)
    {
        const int e = g + kNear[k];
        if (e < 0 || e > last)
            continue;
        const uint2 v = surfCache.Load2(uint(e) * 8);
        if ((v.y & 255) != 0 && (v.y >> 8) == ident)
        {
            c = float3(v.x & 511, (v.x >> 9) & 511, (v.x >> 18) & 511) * exp2(float(int(v.x >> 27) - 24));
            return all(isfinite(c));
        }
    }
    return false;
}

// depth of a texel of the game's shadow map: 32-bit float, D24 (bit 2 of flags) or 16-bit
// normalised (bit 32: RDR2, two bytes per texel)
float AtlasDepth(uint2 t)
{
    if (flags & 32)
    {
        const uint a = t.y * atlasPitch + t.x * 2;
        const uint raw = atlas.Load(a & ~3u);
        return float((a & 2) ? raw >> 16 : raw & 0xFFFF) / 65535.0;
    }
    const uint raw = atlas.Load(t.y * atlasPitch + t.x * 4);
    return (flags & 2) ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
}

// sunlight on the point according to the game's shadow map (1 if no cascade covers it)
float GameLit(float3 p)
{
    const uint count = min(sunBuf.Load(32), 8u);
    for (uint k = 0; k < count; k++)
    {
        const uint base = 48 + k * 64;
        const float4 m0 = asfloat(sunBuf.Load4(base)), m1 = asfloat(sunBuf.Load4(base + 16)), m2 = asfloat(sunBuf.Load4(base + 32));
        const float4 vp = asfloat(sunBuf.Load4(base + 48));
        const float M[12] = { m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w };
        float3 s;
        for (uint c = 0; c < 3; c++)
            s[c] = M[9 + c] + p.x * M[c] + p.y * M[3 + c] + p.z * M[6 + c];
        if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z <= 0 || s.z >= 1)
            continue;
        const float2 texel = vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw;
        // gameMin: lit share of the neighbourhood (like the game's filtering: its penumbra included); otherwise
        // lit only if the whole neighbourhood is (game shadow widened by gameDilate texels)
        const int r = int(gameDilate);
        float litCount = 0;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
            {
                const float stored = AtlasDepth(uint2(int2(texel) + int2(dx, dy)));
                const bool texelLit = sunSign > 0 ? s.z <= stored + gameBias : s.z >= stored - gameBias;
                if (!texelLit && gameMin == 0)
                    return 0.0;
                litCount += texelLit ? 1.0 : 0.0;
            }
        return litCount / float((2 * r + 1) * (2 * r + 1));
    }
    return 1.0;
}

// does the sun see through this point h, on the ray leaving the receiver? The game's shadow map
// is the sun's view, alpha test included. The game records each occluder a bit further from the sun
// than it is (its bias against acne): h is a hole only if the first occluder recorded at its texel
// is at least halfway between h and the receiver (cut-out foliage, the sun goes through to the ground).
// Too close to the receiver to decide, or outside the cascades: unknown.
bool SunSeesThrough(float3 h, float3 receiver)
{
    if ((flags & 1) == 0)
        return false;
    const uint count = min(sunBuf.Load(32), 8u);
    for (uint k = 0; k < count; k++)
    {
        const uint base = 48 + k * 64;
        const float4 m0 = asfloat(sunBuf.Load4(base)), m1 = asfloat(sunBuf.Load4(base + 16)), m2 = asfloat(sunBuf.Load4(base + 32));
        const float4 vp = asfloat(sunBuf.Load4(base + 48));
        const float M[12] = { m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w };
        float3 s, r;
        for (uint c = 0; c < 3; c++)
        {
            s[c] = M[9 + c] + h.x * M[c] + h.y * M[3 + c] + h.z * M[6 + c];
            r[c] = M[9 + c] + receiver.x * M[c] + receiver.y * M[3 + c] + receiver.z * M[6 + c];
        }
        if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z <= 0 || s.z >= 1)
            continue;
        const float gap = sunSign * (r.z - s.z); // > 0: h between the sun and the receiver
        if (gap <= 2.0 * gameBias)
            return false;
        const float stored = AtlasDepth(uint2(vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw));
        return sunSign * (stored - s.z) > 0.5 * gap;
    }
    return false;
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

// surface normal from depth: the nearest side on each axis (object edges)
// a point the camera sees through (it sees further than it, or the sky) is not solid: it is
// a hole of an alpha-tested card (foliage, grass, fences, fringes), whose captured
// geometry is just the rectangle. 1: hole; 0: solid; -1: off screen, the camera does not know.
int ScreenSees(float3 h)
{
    if (h.z <= 1e-3)
        return -1;
    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv <= 0) || any(uv >= 1))
        return -1;
    const float3 seen = PointAt(int2(uv * float2(width, height)));
    return (seen.z <= 0 || seen.z > h.z * 1.03 + 0.05) ? 1 : 0;
}

// hit point belonging to a character (Red Dead: character marker in the alpha of the G-buffer normals): it casts neither occlusion nor
// ambient shadow. The player's character, standing against a wall, hid the "sky" from a large area of that wall (16 m long rays): a dark, blurry
// spot that followed it, hence followed the screen (reported by the player). Projected on screen: the pixel shows a character at about this depth
// (its hidden back is a few tens of centimetres behind what the camera sees). Off screen: unknown, kept
bool CharacterAt(float3 h)
{
    if ((gnMode & 3) != 2 || gnPitch == 0 || h.z <= 1e-3)
        return false;
    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv <= 0) || any(uv >= 1))
        return false;
    const int2 t = int2(uv * float2(width, height));
    const float3 seen = PointAt(t);
    if (seen.z <= 0 || abs(seen.z - h.z) > 0.1 * h.z + 0.6)
        return false;
    return float(gnormal.Load(t.y * gnPitch + t.x * 4) >> 24) / 255.0 > 0.25;
}

// hole of a cut-out card: the camera sees through it; off screen, the sun (its map)
bool Hole(float3 h, float3 receiver)
{
    const int screen = ScreenSees(h);
    return screen >= 0 ? screen == 1 : SunSeesThrough(h, receiver);
}

// flat surface around the pixel (like live_reflect): the points 2 pixels away on each side are aligned
// with it on at least one axis; false for grass, foliage, a fence (the depth jumps)
float Flatness(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(2, 0)), r = PointAt(t + int2(2, 0)), u = PointAt(t - int2(0, 2)), b = PointAt(t + int2(0, 2));
    if (l.z <= 0 || r.z <= 0 || u.z <= 0 || b.z <= 0)
        return 0;
    const float3 h = r - l, v = b - u;
    const float eh = length(cross(h, p - l)) / max(length(h), 1e-6);
    const float ev = length(cross(v, p - u)) / max(length(v), 1e-6);
    return saturate(1.0 - min(eh, ev) / (0.004 * p.z + 0.002));
}

// continuous surface around the pixel (trunk, rock, character, wall): 3 pixels away in all four directions, the depth stays close;
// false for a branch, a twig, a foliage edge, which move in the wind and where occlusion shook
bool DepthContinuous(int2 t, float3 p, int k0 = 3)
{
    const int2 kSide[4] = { int2(k0, 0), int2(-k0, 0), int2(0, k0), int2(0, -k0) };
    for (uint k = 0; k < 4; k++)
    {
        const float3 q = PointAt(t + kSide[k]);
        if (q.z <= 0 || abs(q.z - p.z) > 0.04 * p.z + 0.05)
            return false;
    }
    return true;
}

// foliage (cut-out cards, flat and therefore accepted by Flatness): around the pixel, 6 pixels away, the camera
// sees clearly further (the background through the holes of the leaves)
bool SeesThroughAround(int2 t, float3 p)
{
    static const int2 kAround[8] = { int2(6, 0), int2(-6, 0), int2(0, 6), int2(0, -6), int2(4, 4), int2(-4, 4), int2(4, -4), int2(-4, -4) };
    for (uint k = 0; k < 8; k++)
    {
        const float3 q = PointAt(t + kAround[k]);
        if (q.z <= 0 || q.z > p.z * 1.3 + 0.5)
            return true;
    }
    return false;
}

// overall normal of a rough surface (grass, ferns on the ground): slope of the terrain taken 8 pixels away on each
// side; 0 if a neighbour is missing or jumps too much (object edge)
float3 MacroNormal(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(8, 0)), r = PointAt(t + int2(8, 0)), u = PointAt(t - int2(0, 8)), b = PointAt(t + int2(0, 8));
    if (l.z <= 0 || r.z <= 0 || u.z <= 0 || b.z <= 0)
        return 0;
    if (max(max(abs(l.z - p.z), abs(r.z - p.z)), max(abs(u.z - p.z), abs(b.z - p.z))) > 0.3 * p.z + 0.5)
        return 0;
    const float3 n = normalize(cross(r - l, b - u));
    return dot(n, p) > 0 ? -n : n;
}

float3 NormalAt(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(1, 0)), r = PointAt(t + int2(1, 0)), u = PointAt(t - int2(0, 1)), b = PointAt(t + int2(0, 1));
    const float3 dx = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
    const float3 dy = (b.z > 0 && (u.z <= 0 || abs(b.z - p.z) < abs(u.z - p.z))) ? b - p : p - u;
    float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n; // facing the camera
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    gnPitch = gnPitchPacked & 0xFFFF;
    // depth law measured in this frame (live_depthfit), otherwise the constants'
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0; // reversed depth: the sky at 0
    const uint raw = depth.Load(id.y * depthPitch + id.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float3 sun = asfloat(sunBuf.Load3(0));
    float lit = 1.0;
    float hitDistance = kFp16Max; // SIGMA mode: distance to the first ray's occluder
    float weight = 0.0;           // SIGMA mode: share of the shadow kept by the corrections (0: no RT shadow)
    const float w = gB / (d - gA);
    float gameVis = 1.0; // sun received according to the game's map (ambient occlusion: sky share of the light)
    // occlusion only (sun shadows off, flags 64): a pixel without a ray this frame (3 out of 4), the sky or what is beyond the range exit here, without a normal,
    // flatness or the game's map (all that preparation cost several milliseconds, whatever the number of rays)
    const bool aoOnly = (flags & 64) != 0 && (flags & (8u | 4096u)) == 0 && debugMode == 0;
    static const uint2 kQuarter[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
    // motion of this pixel (the game's vectors): a pixel moving by more than one pixel per frame is computed every frame, not at quarter rate. At quarter rate, in
    // motion, it got a fresh value only one frame in four and the rest was old data pasted back: occlusion fizzed and left
    // lines at the edges of objects (reported by the player)
    float motionPxAo = 0;
    if (aoMotion < 16 && w > 0)
    {
        const uint rawMv = motion.Load(id.y * motionPitch + id.x * 4);
        float2 mvA = float2(f16tof32(rawMv & 0xFFFF), f16tof32(rawMv >> 16));
        mvA = (aoMotion & 3) == 0 ? mvA : (aoMotion & 3) == 1 ? mvA * 0.5 : (aoMotion & 3) == 2 ? mvA / float2(width, height) : mvA * 2.0;
        motionPxAo = all(isfinite(mvA)) ? length(mvA * float2(width, height)) : 0.0;
    }
    const bool movingHere = motionPxAo > 1.0;
    // (in motion: every other frame, in a checkerboard, rather than every frame as in v104: same cost per 2×2 block as at rest; when the camera
    // moves, the whole screen moves, and computing every frame quadrupled the pass's cost)
    const bool notThisFrame = (flags & 128) != 0 && (movingHere ? ((id.x + id.y + frameIndex) & 1u) != 0 : any((id & 1u) != kQuarter[frameIndex & 3]));
    if (aoOnly && (d == gClear || !(w > 0) || w >= aoMaxDist || aoAmount <= 0 || notThisFrame))
    {
        const bool later = notThisFrame && d != gClear && w > 0 && w < aoMaxDist && aoAmount > 0; // computed on another frame: -1, rebuilt by live_shadowtemporal
        shadow.Store(id.y * shadowPitch + id.x * 4, asuint(later ? -1.0 : 1.0));
        if (flags & (1u << 20))
        {
            const uint plane = shadowPitch * height, a0 = id.y * shadowPitch + id.x * 4;
            const float v = later ? -1.0 : 0.0;
            bounce.Store(a0, asuint(v)), bounce.Store(plane + a0, asuint(v)), bounce.Store(2 * plane + a0, asuint(v));
        }
        return;
    }
    if (!aoOnly && (flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
    {
        const float2 ndc = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p = float3(ndc.x * w / p00, ndc.y * w / p11, w);
        const float dist = length(p);
        // four rays spread over the sun disc (2×2 strata rotated per pixel): smooth penumbra
        const float3 t1 = normalize(abs(sun.y) < 0.99 ? cross(sun, float3(0, 1, 0)) : cross(sun, float3(1, 0, 0)));
        const float3 t2 = cross(sun, t1);
        const float rot = 6.2831853 * frac(52.9829189 * frac(dot(float2(id), float2(0.06711056, 0.00583715))) + 0.61803399 * float(frameIndex & 1023));
        hitDistance = kFp16Max;
        lit = (flags & 64) ? 1.0 : 0.0;
        // SIGMA wants one ray per pixel (it denoises itself) and the occluder's distance
        const uint rays = (flags & 64) ? 0u : (flags & 4096) ? 1u : clamp((flags >> 8) & 7, 1u, 4u); // 1 to 4 rays (shadow_rays setting)
        for (uint k = 0; k < rays; k++)
        {
            const float u = rays == 1 ? 0.0 : (float(k & 1) + 0.5) * 0.5, v = (float(k >> 1) + 0.5) * 0.5;
            const float r = sunRadius * sqrt(u), a = 6.2831853 * v + rot;
            RayDesc ray;
            ray.Origin = p * (1.0 - bias) + sun * bias * dist;
            ray.Direction = normalize(sun + t1 * (r * cos(a)) + t2 * (r * sin(a)));
            ray.TMin = 0.0;
            ray.TMax = 1e5;
            // first triangle that is not a hole seen through by the camera or by the sun: shadow
            RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
            bool blocked = false;
            while (q.Proceed())
                if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                    ((flags & 2048) == 0 || !Hole(ray.Origin + ray.Direction * q.CandidateTriangleRayT(), p)))
                {
                    blocked = true;
                    if (k == 0)
                        hitDistance = q.CandidateTriangleRayT();
                    q.Abort();
                }
            lit += blocked ? 0.0 : 1.0 / rays;
        }
        // face turned away from the sun: the game already darkens it through its lighting; an extra RT shadow
        // would make spots there (its map's test sees it lit, the surface being its own occluder)
        const float ndl = dot(NormalAt(int2(id), p), sun);
        const bool facesSun = ndl > 0.08;
        weight = 1.0;
        if (!facesSun && (flags & 4))
            lit = 1.0, weight = 0.0;
        else if (flags & 1)
        {
            const float game = GameLit(p);
            gameVis = facesSun ? game : 0.0;
            if (flags & 8)
                lit = game; // check: the game's shadow as recomputed
            else if (flags & 4)
            {
                // add shadow only where the game lights: the remaining sun is its visibility "game";
                // the wanted visibility is the lower of the two, URTI removes the difference
                lit = gameMin ? 1.0 - max(0.0, game - lit) : 1.0 - game * (1.0 - lit);
                weight *= game;
            }
        }
        if (ndl <= 0)
            hitDistance = 0; // SIGMA: surface turned away from the sun, in its own shadow
    }
    // a shadow removes only the sun's direct light, proportional to the surface's angle toward it
    // (N·L); the rest (sky light, bounces) remains. The lit point receives sky + sun × N·L: the
    // sun's share there is x N·L / (1 + x N·L), x = sun / sky measured in the image (live_sunlight);
    // noon: sharp shadows, overcast: almost nothing. Not measured: fixed slope (old rule).
    if (!aoOnly && (flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
    {
        const float2 ndc2 = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p2 = float3(ndc2.x * w / p00, ndc2.y * w / p11, w);
        const float ndl2 = saturate(dot(NormalAt(int2(id), p2), sun));
        const float ndlWeight = sunSky > 0 ? sunSky * ndl2 / (1.0 + sunSky * ndl2) : saturate(ndl2 * 2.5);
        lit = 1.0 - (1.0 - lit) * ndlWeight;
        weight *= ndlWeight;
    }
    // RT ambient occlusion: one short ray per pixel per frame (cosine-weighted hemisphere, pattern
    // rotating from one frame to the next, averaged by denoising), in the scene's real geometry (off
    // screen included); occluder at distance t: occlusion 1 - t / range. It removes only
    // sky light: all of it for a point in shadow, 1 / (1 + x N·L) of it for a sunlit point.
    // Only on flat surfaces (ground, walls, roads): grass and foliage, where every blade is
    // geometry, darkened entirely (the game already has its occlusion there) and shook in the wind
    const float2 ndc3 = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
    const float3 p3 = float3(ndc3.x * w / p00, ndc3.y * w / p11, w);
    bool aoSkipped = false; // quarter-rate occlusion: this pixel is not computed this frame
    float3 bounceSum = 0;   // light bounce: colour of the surfaces hit, weighted like the occlusion
    // rough surface (bit 21): grass, ferns covering the ground; occlusion starts there from the overall slope,
    // above the blades (otherwise the foot of trunks and rocks standing in the grass got none)
    const bool flatHere = Flatness(int2(id), p3) > 0.5;
    // (closer than 20 m: beyond, it is distant foliage, behind the game's haze; occlusion made a dark grain there
    // on top of the fog)
    const float3 macroN = (!flatHere && (flags & (1u << 21)) && w < 20.0) ? MacroNormal(int2(id), p3) : float3(0, 0, 0);
    const bool rough = any(macroN != 0);
    // characters, horses (Red Dead's G-buffer marker, alpha of its normals) and foliage, skin (material type): no occlusion.
    // They move all the time (animation, wind): their occlusion trailed behind them and flickered (measured: the flicker at rest came
    // mostly from the canopy)
    bool aoExcluded = false;
    if ((gnMode & 3) == 2 && gnPitch != 0 && float(gnormal.Load(id.y * gnPitch + id.x * 4) >> 24) / 255.0 > 0.25)
        aoExcluded = true;
    if ((gnPitchPacked >> 16) != 0)
    {
        const float kind = float(materialTarget.Load(id.y * (gnPitchPacked >> 16) + id.x * 4) & 255) / 255.0;
        // ("foliage" also covers grass: excluded in v102, lawns and nature had lost all occlusion. Grass on the ground, with a world normal
        // facing up, keeps it; only the leaves of trees and bushes, oriented every which way, stay excluded)
        const float3 gwUp = (gnMode & 3) == 2 ? GNormal(int2(id), (gnMode >> 8) & 7) : float3(0, 0, 0);
        if ((abs(kind - 0.2) < 0.04 && !(gwUp.z > 0.6)) || abs(kind - 0.31) < 0.04)
            aoExcluded = true;
    }
    // the game's exact normal (gnMode bit 12): occlusion also applies to surfaces that are not flat (trunks, rocks, objects,
    // characters), where the depth normal was only noise; it faces the camera
    float3 gnHere = 0;
    if ((gnMode & 0x1000) && !rough && w > 0 && w < aoMaxDist)
    {
        gnHere = GNormalCam(int2(id));
        if (dot(gnHere, -normalize(p3)) <= 0.05 || !(flatHere || DepthContinuous(int2(id), p3)))
            gnHere = 0;
    }
    const bool gnOk = any(gnHere != 0);
    if (!aoOnly && aoAmount > 0 && (flags & 4096) == 0 && d != gClear && w > 0 && w < aoMaxDist && (flatHere || rough || gnOk) && !aoExcluded && !SeesThroughAround(int2(id), p3))
        aoSkipped = notThisFrame; // quarter rate: the pixel of the 2×2 block assigned to this frame (all four in four frames)
    if (aoAmount > 0 && !aoSkipped && (flags & 4096) == 0 && d != gClear && w > 0 && w < aoMaxDist && (flatHere || rough || gnOk) && !aoExcluded &&
        !SeesThroughAround(int2(id), p3))
    {
        float3 n = rough ? macroN : NormalAt(int2(id), p3);
        // comparison of the G-buffer normals with the depth normal (flat surface, one pixel in 64): the right decoding is the one that fits
        if ((gnMode & 0x2000) && !rough && flatHere && (id.x & 7) == 0 && (id.y & 7) == 0)
            for (uint sv = 0; sv < 8; sv++)
            {
                const float3 g = GNormal(int2(id), sv);
                if (all(g == 0))
                    continue;
                uint unused;
                nstats.InterlockedAdd(sv * 8, uint(int(round(dot(g, n) * 1000.0))), unused);
                nstats.InterlockedAdd(sv * 8 + 4, 1, unused);
            }
        // camera orientation (world normals, gnMode bit 14): sums of the world normal times the camera one (live_rotfit)
        if ((gnMode & 0x4000) && !rough && flatHere && (id.x & 6) == 0 && (id.y & 6) == 0) // (quarter rate: one of the block's four pixels each frame)
        {
            const float3 a = GNormal(int2(id), 0);
            if (any(a != 0))
            {
                uint unused;
                for (uint ai = 0; ai < 3; ai++)
                    for (uint bj = 0; bj < 3; bj++)
                        nstats.InterlockedAdd(64 + (ai * 3 + bj) * 4, uint(int(round(a[ai] * n[bj] * 1000.0))), unused);
                nstats.InterlockedAdd(100, 1, unused);
            }
        }
        // the game's exact normal (flat surface: the depth only guesses it, with the sub-pixel jitter's grain)
        if (gnOk)
            n = gnHere;
        const float3 b1 = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
        const float3 b2 = cross(n, b1);
        // two rays (azimuth strata); four where the image moves by more than a pixel and a half: the history there
        // is rejected or barely kept, the noise of two rays would shake. Quarter rate: twice as many (four times
        // fewer pixels: half as many rays in total)
        const uint aoBase = (flags & 128) ? 2u : 1u;
        // number tuned by the add-on (bits 16 to 18): under arcades, almost the whole screen receives occlusion
        // and the pass's cost, proportional to the rays, tripled (5.5 ms in RDR2)
        const uint aoRest = (flags >> 16) & 7;
        uint aoRays = aoRest ? aoRest : 2 * aoBase;
        // far away the occlusion is smooth and accumulated over many frames: fewer rays cost nothing visible
        if (w > 25.0)
            aoRays = 1;
        else if (w > 12.0)
            aoRays = min(aoRays, 2u);
        // in motion: computed every frame (no longer quarter rate), two rays are enough (the cost per 2×2 block stays of the same order)
        if (movingHere && (flags & 128))
            aoRays = min(aoRays, 2u);
        uint h = AoHash(id.x * 1973u + id.y * 9277u + frameIndex * 26699u);
        float occlusion = 0;
        for (uint k = 0; k < aoRays; k++, h = AoHash(h))
        {
            // (bit 27: low discrepancy; otherwise hashing) ray k: shared rotation of the pixel in azimuth (strata), radius from the golden-ratio sequence
            const float n0 = Ign(float2(id), frameIndex), n1 = Ign(float2(id.yx) + 17.0, frameIndex + 7u);
            const float u1 = (flags & (1u << 27)) ? frac(n0 + 0.618034 * float(k)) : float(h & 0xFFFF) / 65536.0;
            const float u2 = (flags & (1u << 27)) ? (frac(n1) + float(k)) / float(aoRays) : (float(h >> 16) / 65536.0 + float(k)) / float(aoRays);
            const float r = sqrt(u1), phi = 6.2831853 * u2;
            RayDesc ray;
            ray.Origin = p3 + n * (rough ? max(0.08, 0.004 * length(p3)) : max(0.01, 0.002 * length(p3)));
            ray.Direction = normalize(b1 * (r * cos(phi)) + b2 * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
            // occluders closer than 6 cm ignored: the game's decals (mud, marks, puddles), laid just above
            // the ground without writing depth, made dark spots of their exact shape
            ray.TMin = rough ? 0.25 : 0.06; // rough surface: not the blades themselves
            ray.TMax = aoRadius;
            // opaque ray first (the hardware alone finds the closest); slow search, triangle by triangle, only if that point
            // is a hole of a cut-out card (same result, without examining each triangle of the ground and rocks)
            bool aoHit = false;
            float aoT = 0;
            uint aoInst = 0, aoPrim = 0;
            // hole of a leaf: next opaque ray, right after it, at most a few times (budget, bits 24 to 26), then no occluder. A ray's cost is bounded:
            // six hops followed by an unbounded search made 45 ms of occlusion in dense grass
            bool aoHole = false;
            RayDesc stepRay = ray;
            const uint maxHops = (flags >> 24) & 7;
            for (uint hop = 0; hop <= maxHops; hop++)
            {
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
                qf.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, stepRay);
                qf.Proceed();
                aoHit = qf.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                if (!aoHit)
                    break;
                aoT = qf.CommittedRayT(), aoInst = qf.CommittedInstanceID(), aoPrim = qf.CommittedPrimitiveIndex();
                aoHole = ((flags & 2048) && Hole(ray.Origin + ray.Direction * aoT, p3)) || CharacterAt(ray.Origin + ray.Direction * aoT);
                if (!aoHole)
                    break;
                stepRay.TMin = aoT + max(1e-3, 1e-4 * aoT);
            }
            // (flags bit 23: old rule, every triangle examined, to compare)
            if (aoHit && aoHole && (flags & (1u << 23)) == 0)
                aoHit = false;
            if ((flags & 2048) && (flags & (1u << 23)))
            {
                RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
                q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
                while (q.Proceed())
                    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && !Hole(ray.Origin + ray.Direction * q.CandidateTriangleRayT(), p3))
                        q.CommitNonOpaqueTriangleHit(); // the closest stays (further candidates are discarded)
                aoHit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                if (aoHit)
                    aoT = q.CommittedRayT(), aoInst = q.CommittedInstanceID(), aoPrim = q.CommittedPrimitiveIndex();
            }
            // close occluder: strong occlusion; toward the range, it fades out smoothly
            const float t = aoHit ? aoT / aoRadius : 1.0;
            occlusion += (1.0 - t) * (1.0 - t) / float(aoRays) * (rough ? 0.75 : 1.0);
            // the sky light removed in that direction is replaced by the light the occluder sends back (same
            // weight); occluder never seen on screen: nothing (the game keeps its own ambient lighting in the distance)
            float3 hc;
            // (copy 0: the frame's scene; the geometry cache has no per-triangle colours)
            if ((flags & (1u << 20)) && aoHit && aoInst == 0 && Cached(aoPrim, hc))
                bounceSum += hc * ((1.0 - t) * (1.0 - t) / float(aoRays));
        }
        // long-range sky visibility: one or two long rays (skyRange, 12 m by default) tell whether an occluder hides the sky in
        // that direction: canopy above the undergrowth, deep awning, cliff, narrow alley. They remove only sky light,
        // like contact occlusion; a distant occluder counts less (1 - t / range). Leaf holes let the ray through
        // (same rule as occlusion). Named approximation: the sky is assumed uniform (no preferred direction for ambient light)
        // Only directions above the horizon carry sky light: a ray going down hits the ground, which hides none of it
        // (the game already lights a vertical face less). It is sent back above the horizon (mirrored across the horizontal plane) and weighted by its
        // height in the sky. The world's up comes from the camera orientation (Red Dead: world normals, live_rotfit); unknown: no long rays
        float skyOcc = 0, skyW = 0;
        float3 upCam = 0;
        if ((gnMode & 3) == 2 && sunBuf.Load(2092) != 0)
            upCam = float3(asfloat(sunBuf.Load(2056)), asfloat(sunBuf.Load(2068)), asfloat(sunBuf.Load(2080)));
        if (skyAmount > 0 && skyRange > aoRadius && !rough && dot(upCam, upCam) > 0.5 && DepthContinuous(int2(id), p3, 6)) // (large surfaces: not a branch in the wind)
        {
            upCam = normalize(upCam);
            const uint skyRays = (w > 25.0 || (movingHere && (flags & 128))) ? 1u : 2u;
            uint hs = AoHash(id.x * 7919u + id.y * 104729u + frameIndex * 15485863u + 17u);
            for (uint ks = 0; ks < skyRays; ks++, hs = AoHash(hs))
            {
                const float m0 = Ign(float2(id) + 41.0, frameIndex + 3u), m1 = Ign(float2(id.yx) + 71.0, frameIndex + 11u);
                const float s1 = (flags & (1u << 27)) ? frac(m0 + 0.618034 * float(ks)) : float(hs & 0xFFFF) / 65536.0;
                const float s2 = (flags & (1u << 27)) ? (frac(m1) + float(ks)) / float(skyRays) : (float(hs >> 16) / 65536.0 + float(ks)) / float(skyRays);
                const float rr = sqrt(s1), ph = 6.2831853 * s2;
                RayDesc sr;
                sr.Origin = p3 + n * max(0.01, 0.002 * length(p3));
                sr.Direction = normalize(b1 * (rr * cos(ph)) + b2 * (rr * sin(ph)) + n * sqrt(max(0.0, 1.0 - s1)));
                float upDot = dot(sr.Direction, upCam);
                if (upDot < 0)
                    sr.Direction -= 2.0 * upDot * upCam, upDot = -upDot;
                if (upDot < 0.02 || dot(sr.Direction, n) < 0.02)
                    continue;
                skyW += upDot;
                sr.TMin = 0.06;
                sr.TMax = skyRange;
                bool skyHit = false;
                float skyT = 0;
                const uint maxHopsSky = (flags >> 24) & 7;
                for (uint hop2 = 0; hop2 <= maxHopsSky; hop2++)
                {
                    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qs;
                    qs.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, sr);
                    qs.Proceed();
                    skyHit = qs.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                    if (!skyHit)
                        break;
                    skyT = qs.CommittedRayT();
                    if (!(((flags & 2048) && Hole(sr.Origin + sr.Direction * skyT, p3)) || CharacterAt(sr.Origin + sr.Direction * skyT)))
                        break;
                    skyHit = false;
                    sr.TMin = skyT + max(1e-3, 1e-4 * skyT);
                }
                if (skyHit)
                    skyOcc += upDot * (1.0 - skyT / skyRange);
            }
            skyOcc = skyW > 0 ? skyOcc / skyW : 0.0;
            occlusion = 1.0 - (1.0 - saturate(occlusion)) * (1.0 - skyAmount * skyOcc);
        }
        // does this point receive the sun? (occlusion removes only sky light; in the sun, that is only a small share)
        // bit 28: an RT ray toward the sun (leaf holes crossed as for occlusion). The game's shadow map, read otherwise, does not match
        // in Red Dead (v44: everything "on the edge"): sunlit walls were darkened as if they were in shadow
        if (aoOnly && (flags & 16) == 0 && (flags & (1u << 28)))
        {
            gameVis = 0.0;
            if (dot(n, sun) > 0.08)
            {
                RayDesc sr2;
                sr2.Origin = p3 + n * max(0.01, 0.002 * length(p3));
                sr2.Direction = sun;
                sr2.TMin = 0.05;
                sr2.TMax = 300.0;
                bool sunBlocked = false;
                const uint maxHopsSun = (flags >> 24) & 7;
                for (uint hop3 = 0; hop3 <= maxHopsSun; hop3++)
                {
                    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> qs3;
                    qs3.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, sr2);
                    qs3.Proceed();
                    sunBlocked = qs3.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                    if (!sunBlocked)
                        break;
                    const float t3 = qs3.CommittedRayT();
                    if (!(((flags & 2048) && Hole(sr2.Origin + sr2.Direction * t3, p3)) || CharacterAt(sr2.Origin + sr2.Direction * t3)))
                        break;
                    sunBlocked = false;
                    sr2.TMin = t3 + max(1e-3, 1e-4 * t3);
                }
                gameVis = sunBlocked ? 0.0 : 1.0;
            }
        }
        else if (aoOnly && (flags & 17) == 1)
            gameVis = dot(n, sun) > 0.08 ? GameLit(p3) : 0.0;
        const float ndlSky = (flags & 16) ? 0.0 : saturate(dot(n, sun)) * gameVis;
        const float skyShare = sunSky > 0 ? 1.0 / (1.0 + sunSky * ndlSky) : 1.0 - 0.6 * ndlSky;
        // (the last third of the range: faded out smoothly, the range changes with the time budget)
        lit = max(0.0, lit - aoAmount * occlusion * skyShare * saturate((aoMaxDist - w) / (0.3 * aoMaxDist)));
    }
    // in the distance, the game drowns the landscape in haze: a full RT shadow makes black spots there; it
    // fades out between 120 and 300 units (metres in RDR2)
    if (w > 0)
        lit = lerp(lit, 1.0, saturate((w - 120.0) / 180.0)), weight *= 1.0 - saturate((w - 120.0) / 180.0);
    if (flags & 4096)
    {
        // SIGMA denoises the visibility alone; the corrections are applied afterwards (live_nrdpost)
        penumbra.Store(id.y * shadowPitch + id.x * 4, asuint(PackPenumbra(hitDistance, tan(sunRadius))));
        shadow.Store(id.y * shadowPitch + id.x * 4, asuint(weight));
        return;
    }
    const float rawLit = lit;
    float motionPixels = 0;
    bool historyUsed = false;
    // accumulation over time: the previous frame's mask, read where the game's motion vector
    // says this point was, if the depth matches there (otherwise: newly uncovered point, nothing to reuse)
    if (motionConvention < 16 && w > 0 && w < 1e5)
    {
        const uint raw = motion.Load(id.y * motionPitch + id.x * 4);
        float2 mv = float2(f16tof32(raw & 0xFFFF), f16tof32(raw >> 16));
        const uint k = motionConvention;
        mv = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / float2(width, height) : mv * 2.0;
        if (k & 4)
            mv = -mv;
        if (k & 8)
            mv.y = -mv.y;
        const float2 prevUv = (float2(id) + 0.5) / float2(width, height) + mv;
        motionPixels = length(mv * float2(width, height));
        if (all(isfinite(mv)) && all(prevUv > 0) && all(prevUv < 1))
        {
            const float2 q = prevUv * float2(width, height) - 0.5;
            const int2 b = int2(floor(q));
            const float2 f = q - b;
            float hist = 0, wsum = 0;
            for (int j = 0; j < 4; j++)
            {
                const int2 t = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(width - 1, height - 1));
                const uint rawD = depthPrev.Load(t.y * depthPitch + t.x * 4);
                const float dp = depth24 != 0 ? float(rawD & 0xFFFFFF) / 16777215.0 : asfloat(rawD);
                const float wp = gB / (dp - gA);
                const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
                if (wp > 0 && abs(wp - w) < 0.04 * w + 0.05)
                    hist += asfloat(prevShadow.Load(t.y * shadowPitch + t.x * 4)) * bw, wsum += bw;
            }
            if (wsum > 0.25)
                lit = lerp(hist / wsum, lit, temporalAlpha), historyUsed = true;
        }
    }
    // diagnostic views (shown by the "mask" view)
    if (debugMode == 1)
        lit = rawLit;
    else if (debugMode == 2)
        lit = saturate(motionPixels / 20.0); // white: 20 pixels of motion or more
    else if (debugMode == 3)
        lit = historyUsed ? 1.0 : 0.0;       // black: history rejected (newly uncovered point, depth changed)
    shadow.Store(id.y * shadowPitch + id.x * 4, asuint(aoSkipped && debugMode == 0 ? -1.0 : lit));
    if (flags & (1u << 20))
    {
        const uint plane = shadowPitch * height, a = id.y * shadowPitch + id.x * 4;
        const float3 b = aoSkipped ? float3(-1, -1, -1) : bounceSum;
        bounce.Store(a, asuint(b.r)), bounce.Store(plane + a, asuint(b.g)), bounce.Store(2 * plane + a, asuint(b.b));
    }
}
