// Live URTI: ray-traced reflections. One ray per smooth pixel (according to the game's G-buffer
// material), reflected around the depth-derived normal, cast into the captured geometry. The hit
// point is reprojected on screen: if it is visible there (same depth), its colour will be read from
// the game's image by the composition. Output per pixel: screen position of the reflected point (u, v),
// weight (Fresnel × smoothness), 0 if nothing usable (the game's own reflection then stays).
//
// Anchoring (geometric memory): the mirror surface and the reflected point are found again in the captured
// geometry (same draw, same triangle, same barycentric coordinates); the previous frame's
// vertices give their position at that time, deformations included. The virtual point (the reflected
// point mirrored across the mirror's plane) of the previous frame gives the exact place where this reflection
// was: stabilisation reads its history there, if the identities (reflected object, mirror) match.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer depth : register(t1);    // the game's depth (32 bits per texel, aligned rows)
ByteAddressBuffer material : register(t2); // copy of the G-buffer's material image (32 bits per texel)
ByteAddressBuffer sunBuf : register(t3);   // bytes 36 to 47: this frame's depth law
StructuredBuffer<uint4> regions : register(t4); // per screen draw: start (bytes), size, counter, identity
ByteAddressBuffer links : register(t5);    // per draw: identity, start (vertices) of the same object in the previous frame + 1
ByteAddressBuffer viewCur : register(t6);  // this frame's vertices (float3, camera space)
ByteAddressBuffer viewPrev : register(t7); // previous frame's vertices (its camera space)
ByteAddressBuffer surfCache : register(t8); // per triangle: colour seen on screen (RGB9E5), identity and measurements (live_surfcache)
ByteAddressBuffer viewT : register(t9);     // layer of flat transparent surfaces (water, glass): float3 per vertex
ByteAddressBuffer geoColors : register(t10); // flags 16384: learned colours of geometry-cache objects (live_surfcache)
RWByteAddressBuffer reflection : register(u0); // float4 per pixel: u, v, weight, smoothness; weight < 0: reflecting
                                               // surface whose reflected point is hidden or off
                                               // screen (colour unknown: stabilisation keeps the history);
                                               // u < -0.5: colour taken from the surface cache (-1 - r, g, weight, b)
RWByteAddressBuffer rstats : register(u2);     // flags 131072 (reflect_stats=1): reflecting pixels, water among them, rays cast, pixels skipped, hops at the cap
RWByteAddressBuffer anchor : register(u1);     // uint4 per pixel: previous position of the reflection (u, v; NaN if
                                               // unknown), identity of the reflected object, identity of the mirror

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint materialPitch;
    float A;
    float B;
    float p00;
    float p11;
    float clearDepth;
    uint depth24;
    uint channel;      // material channel that carries smoothness (0 to 3)
    float smoothMin;   // below: no RT reflection
    float bias;
    uint flags;        // 1 material read, 2 invert the channel (roughness instead of smoothness), 4 everything is smooth (check),
                       // 8 diagnostic: normalised hit distance; 16 diagnostic: smoothness;
                       // 32: holes of cut-out cards (seen through on screen) let the ray through;
                       // 64: surface cache readable; 256: layer of flat transparent surfaces (water, glass)
                       // (otherwise the 4th component is the hit distance, 1e4 for the sky)
    float f0;          // reflectance at normal incidence (dielectric ~0.04)
    uint reflectPitch;
    uint regionCount;
    uint anchors;      // 1: previous frame's vertices available
    uint prevVertexCount; // vertices readable in viewPrev (a read outside a root buffer is not bounded)
    uint cacheTriangles;  // triangles held by the surface cache
    float3 worldUp;       // flags 8192: the world's up in this frame's camera space (exact normal of water)
    uint budget;          // time budget (add-on): bits 0 to 3 one pixel in 1, 2 or 4 gets a ray, bits 4 to 7 hops from hole to hole, bits 8 and up frame number
    float tMaxNow;        // range of reflected rays (metres)
    uint gnInfo;          // G-buffer normals: bits 0-1 encoding (1 octahedral RG16, 2 world RGB8), 2-4 flipped signs, 5 use them, 16-31 row pitch;
                          // glossy reflections: bits 6-12 minimum smoothness (× 127), bits 13-15 strength (× 7)
};

// the game's exact normals (G-buffer): the depth's only guesses it, with the sub-pixel jitter's grain;
// Red Dead stores them in world space: rotated by the camera orientation solved this frame (sunBuf 2048-2095)
ByteAddressBuffer gnormal : register(t14);

// G-buffer normal as the game writes it (Red Dead: world space, z up), signs applied; 0 if absent
float3 GNormalRaw(int2 t)
{
    const uint enc = gnInfo & 3, pitch = gnInfo >> 16;
    if (enc != 2 || pitch == 0)
        return 0;
    const uint v = gnormal.Load(t.y * pitch + t.x * 4);
    float3 n = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0 * 2.0 - 1.0;
    const uint signs = (gnInfo >> 2) & 7;
    if (signs & 1)
        n.x = -n.x;
    if (signs & 2)
        n.y = -n.y;
    if (signs & 4)
        n.z = -n.z;
    const float l = length(n);
    return l > 1e-3 && all(isfinite(n)) ? n / l : 0;
}

float3 GNormalAt(int2 t)
{
    const uint enc = gnInfo & 3, pitch = gnInfo >> 16;
    if (enc == 0 || (gnInfo & 32) == 0 || pitch == 0)
        return 0;
    const uint v = gnormal.Load(t.y * pitch + t.x * 4);
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
    const uint signs = (gnInfo >> 2) & 7;
    if (signs & 1)
        n.x = -n.x;
    if (signs & 2)
        n.y = -n.y;
    if (signs & 4)
        n.z = -n.z;
    if (enc == 2)
    {
        if (sunBuf.Load(2092) == 0)
            return 0;
        const float3 r0 = asfloat(sunBuf.Load3(2048)), r1 = asfloat(sunBuf.Load3(2060)), r2 = asfloat(sunBuf.Load3(2072));
        n = float3(dot(r0, n), dot(r1, n), dot(r2, n));
    }
    const float l = length(n);
    return l > 1e-3 && all(isfinite(n)) ? n / l : 0;
}

// flags 262144: world radiance cache (game camera known). A hidden or off-screen reflected point takes the colour seen there
// (spatial hash table), a ray that hits nothing the sky's in its direction (sky map); the ray crosses
// foliage according to its learned solid share. Extra output (mixOut): colour already known (rgb) and share to read from the image at (u, v) (a)
ByteAddressBuffer radHash : register(t11);
ByteAddressBuffer envMap : register(t12);
ByteAddressBuffer radParams : register(t13);
RWByteAddressBuffer mixOut : register(u3);
#define RAD_BUF radHash
#define RAD_ENV envMap
#define RAD_PARAMS radParams
#include "live_radhash.hlsli"

static const float kSkipped = -0.25; // u of a reflecting pixel that got no ray this frame (live_reflresolve takes its neighbours' colour)

void Count(uint at)
{
    uint unused;
    rstats.InterlockedAdd(at, 1, unused);
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

float3 Vertex(ByteAddressBuffer b, uint v) { return asfloat(b.Load3(v * 12)); }

// colour kept for this triangle by the surface cache, if it belongs to its current object.
// One entry per eight consecutive triangles (neighbours in the mesh): in RDR2, most triangles
// are smaller than a pixel and one entry per triangle was almost never filled (one occlusion ray
// in four found its colour). Empty entry or one from another object: the neighbouring entries of the same object
bool Cached(uint prim, out float3 c)
{
    c = 0;
    if ((flags & 64) == 0 || prim >= cacheTriangles || regionCount == 0)
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

// geometry-cache object (identity: bit 23, generation, place): the learned colour of its group of triangles, or of a neighbouring group
bool CachedGeo(uint inst, uint prim, uint tris, out float3 c)
{
    c = 0;
    if ((flags & 16384) == 0 || (inst & 0x800000u) == 0)
        return false;
    const uint slot = inst & 0x3FFFu, gen = (inst >> 14) & 255u;
    const int bin = int(min(255u, uint(float(prim) / float(max(tris, 1u)) * 256.0)));
    static const int kNear[5] = { 0, -1, 1, -2, 2 };
    for (uint k = 0; k < 5; k++)
    {
        const int b = bin + kNear[k];
        if (b < 0 || b > 255)
            continue;
        const uint2 v = geoColors.Load2((slot * 256u + uint(b)) * 8u);
        if ((v.y & 255u) != 0 && (v.y >> 8) == gen)
        {
            c = float3(v.x & 511, (v.x >> 9) & 511, (v.x >> 18) & 511) * exp2(float(int(v.x >> 27) - 24));
            return all(isfinite(c));
        }
    }
    return false;
}

// colour and solid share of a hit point the screen does not show: what the world radiance cache saw there, otherwise the learned
// colour of its object (coarser)
// 0: never seen; 1: colour and solid share; 2: only holes at that place (the ray goes through)
static uint gRng = 1u;
float Rand01()
{
    gRng = gRng * 747796405u + 2891336453u;
    uint x = ((gRng >> ((gRng >> 28u) + 4u)) ^ gRng) * 277803737u;
    return float((x >> 22u) ^ x) * (1.0 / 4294967296.0);
}

uint Remembered(float3 h, float path, uint inst, uint prim, uint contrib, out float3 c, out float alpha)
{
    const uint found = RadLookup(RadWorld(h), RadLevel(path), float3(Rand01(), Rand01(), Rand01()), c, alpha);
    if (found != 0)
        return found;
    float3 g;
    if (inst != 0 ? CachedGeo(inst, prim, contrib, g) : Cached(prim, g))
    {
        c = g;
        return 1;
    }
    return 0;
}

// identity of the triangle's object; position and normal of the same point in the previous frame if known
bool Anchor(uint prim, float2 bary, out uint id, out float3 prevPos, out float3 prevNormal)
{
    prevPos = prevNormal = 0;
    id = 0;
    if (regionCount == 0 || anchors == 0)
        return false;
    const uint k = RegionIndex(prim);
    const uint4 r = regions[k];
    const uint byte = prim * 48;
    id = r.w;
    if (anchors == 0 || byte < r.x || byte - r.x >= r.y)
        return false;
    const uint start = links.Load(k * 8 + 4);
    if (start == 0)
        return false;
    const uint v = start - 1 + (byte - r.x) / 16;
    if (v + 2 >= prevVertexCount)
        return false;
    const float3 a = Vertex(viewPrev, v), b = Vertex(viewPrev, v + 1), c = Vertex(viewPrev, v + 2);
    prevPos = a + bary.x * (b - a) + bary.y * (c - a);
    prevNormal = normalize(cross(b - a, c - a));
    return all(isfinite(prevPos)) && all(isfinite(prevNormal));
}

// same thing depending on the copy hit: only the frame's scene (copy 0) has regions and vertices of the previous
// frame; a geometry-cache object only has its structure
bool AnchorOf(uint instance, uint prim, float2 bary, out uint id, out float3 prevPos, out float3 prevNormal)
{
    id = 0, prevPos = 0, prevNormal = 0;
    if (instance != 0)
        return false;
    return Anchor(prim, bary, id, prevPos, prevNormal);
}

static float gA, gB, gClear;

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

// a point the camera sees through (it sees further than it, or the sky) is not solid: it is
// a hole of an alpha-tested card (foliage, grass, fences, fringes), whose captured
// geometry is just the rectangle. Off screen, we do not know: it is assumed solid.
bool SeenThrough(float3 h)
{
    if (h.z <= 1e-3)
        return false;
    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv <= 0) || any(uv >= 1))
        return false;
    const float3 seen = PointAt(int2(uv * float2(width, height)));
    return seen.z <= 0 || seen.z > h.z * 1.03 + 0.05;
}

float3 NormalAt(int2 t, float3 p)
{
    // centred difference four pixels away on each side when both sides are on the same surface
    // (their middle falls back on the point): smoothed normal, the water's ripples and the depth's grain
    // no longer make the reflection shake; otherwise (object edge) the nearest side, over two pixels
    float3 axis[2];
    for (uint k = 0; k < 2; k++)
    {
        const int2 o = k == 0 ? int2(1, 0) : int2(0, 1);
        const float3 l4 = PointAt(t - o * 4), r4 = PointAt(t + o * 4);
        if (l4.z > 0 && r4.z > 0 && length(0.5 * (l4 + r4) - p) < 0.01 * p.z + 0.01)
            axis[k] = r4 - l4;
        else
        {
            const float3 l = PointAt(t - o * 2), r = PointAt(t + o * 2);
            axis[k] = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
        }
    }
    float3 n = normalize(cross(axis[0], axis[1]));
    return dot(n, p) > 0 ? -n : n;
}

// flat surface around the pixel: the points 4 pixels away on each side, on each axis, are aligned
// with it (true for a floor, a roof, a puddle, whatever the perspective; false for foliage,
// branches, cables, where the depth jumps). 1: flat; 0: not a continuous surface.
float Flatness(int2 t, float3 p, int k = 4)
{
    const float3 l = PointAt(t - int2(k, 0)), r = PointAt(t + int2(k, 0)), u = PointAt(t - int2(0, k)), b = PointAt(t + int2(0, k));
    if (l.z <= 0 || r.z <= 0 || u.z <= 0 || b.z <= 0)
        return 0;
    const float3 h = r - l, v = b - u;
    const float eh = length(cross(h, p - l)) / max(length(h), 1e-6);
    const float ev = length(cross(v, p - u)) / max(length(v), 1e-6);
    // relief in both directions (foliage, hair); in only one (edge of a puddle against a wall, distant object
    // along the vertical axis), the surface stays flat
    return saturate(1.0 - (k == 4 ? max(eh, ev) : min(eh, ev)) / (0.004 * p.z + 0.002));
}

// material type (Red Dead: red channel of the material target; 0.2 foliage, 0.31 skin, 0.5 common); 0.5 without a material read
float MaterialKind(int2 t)
{
    return (flags & 1) ? float(material.Load(t.y * materialPitch + t.x * 4) & 255) / 255.0 : 0.5;
}

float Smoothness(int2 t)
{
    if (flags & 4) // check: every surface smooth
        return 1.0;
    if ((flags & 1) == 0)
        return 0;
    const uint raw = material.Load(t.y * materialPitch + t.x * 4);
    float s = float((raw >> (8 * channel)) & 255) / 255.0;
    return (flags & 2) ? 1.0 - s : s;
}

// water according to the material: at least three of its four neighbours too, at about the same distance (not an isolated point on foliage)
bool WaterAround(int2 t, float3 p)
{
    uint count = 0;
    for (uint j = 0; j < 4; j++)
    {
        const int2 o = t + (j == 0 ? int2(1, 0) : j == 1 ? int2(-1, 0) : j == 2 ? int2(0, 1) : int2(0, -1));
        const float3 q = PointAt(o);
        if (q.z > 0 && abs(q.z - p.z) < 0.1 * p.z + 0.05 && Smoothness(o) > 0.98)
            count++;
    }
    return count >= 3;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    // depth law measured in this frame (live_depthfit), otherwise the constants'
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0; // reversed depth: the sky at 0
    float3 p = PointAt(int2(id));
    float waterFade = 1.0; // water: reflection faded where it is too thin (banks, bottom showing through)
    // water, glass: flat transparent surface in front of the point the depth shows (layer 2 of the RT scene, sorted by
    // live_planes, urti.ini water=1; for a game that does not write it into the depth): the reflection starts from it, with the
    // normal of its triangle, smooth as a mirror
    bool water = false;
    float3 nWater = 0;
    if ((flags & 256) && p.z > 0)
    {
        RayDesc ew;
        ew.Origin = 0;
        ew.Direction = normalize(p);
        ew.TMin = 0.05;
        ew.TMax = max(0.06, length(p) - 0.03);
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qw;
        qw.TraceRayInline(scene, RAY_FLAG_NONE, 0x02, ew);
        qw.Proceed();
        if (qw.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const uint t3 = qw.CommittedPrimitiveIndex() * 36;
            const float3 a = asfloat(viewT.Load3(t3)), b = asfloat(viewT.Load3(t3 + 12)), c = asfloat(viewT.Load3(t3 + 24));
            float3 nw = cross(b - a, c - a);
            if (all(isfinite(nw)) && dot(nw, nw) > 1e-12)
            {
                nw = normalize(nw);
                water = true, nWater = dot(nw, ew.Direction) > 0 ? -nw : nw;
                // water thickness along the view (from the surface to the bottom the depth shows): the game's surface
                // continues under the banks, a full reflection over a few millimetres of water made spots there
                waterFade = saturate((length(p) - qw.CommittedRayT() - 0.05) / 0.3);
                p = ew.Direction * qw.CommittedRayT();
            }
        }
    }
    // wet floor (smoothMin < 0: -smoothMin = amount of water): surfaces facing up
    // become smooth like a street after the rain
    const float wet = smoothMin < 0 ? -smoothMin : 0.0;
    const float sMin = smoothMin < 0 ? 0.35 : smoothMin;
    // glossy reflections (surfaces less smooth than the mirror threshold: parquet, waxed wood, metal, cobblestones): from glossMin to sMin, the reflection fades
    // in, blurred according to roughness (below). Above sMin, nothing changes
    const float glossMin = float((gnInfo >> 6) & 127) / 127.0, glossAmt = float((gnInfo >> 13) & 7) / 7.0;
    const bool glossOn = smoothMin >= 0 && glossMin > 0 && glossMin < sMin && glossAmt > 0;
    const float sLow = glossOn ? glossMin : sMin;
    // material first: outside water and smooth surfaces (most of the screen), nothing more to compute (neither normal nor flatness)
    float material = Smoothness(int2(id));
    if (!water && wet <= 0 && (flags & 24) == 0 && (p.z <= 0 || material < sLow || ((flags & 65536) && material <= 0.98)))
    {
        reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(float4(0, 0, 0, 0)));
        if (anchors != 0)
            anchor.Store4(id.y * reflectPitch + id.x * 16, uint4(0x7FC00000, 0x7FC00000, 0, 0));
        return;
    }
    float3 n = water ? nWater : p.z > 0 ? NormalAt(int2(id), p) : float3(0, 1, 0);
    // the game's exact normal, if it faces the camera and roughly agrees with the depth's shape (not a texture detail)
    if (!water && p.z > 0)
    {
        const float3 g = GNormalAt(int2(id));
        if (dot(g, p) < 0 && dot(g, n) > 0.5)
            n = g;
    }
    // RT reflections only on a flat surface (floor, puddle, glass, water): on foliage, hair, a
    // face, the depth normal is noise, the reflection went anywhere and blinked (grey
    // dots); the game keeps its own reflections there. Wet floor: also facing up
    const float flat = water ? 1.0 : p.z > 0 ? Flatness(int2(id), p, 2) : 0.0; // close neighbours: in the distance, 4 pixels fall on another object
    // perfect mirror according to the material (water: some games write exactly 0 into a material target): the surface is
    // smooth by definition, its waves change nothing
    // game camera known: perfect mirror according to the material only on a horizontal and extended surface (water), and with its exact normal:
    // derived from depth, it changes every frame with the sub-pixel jitter, the reflection shook and only cleaned up by accumulating, with the game paused
    if ((flags & 8192) && !water && p.z > 0 && material > 0.98)
    {
        if (dot(n, worldUp) > 0.7 && WaterAround(int2(id), p))
            n = worldUp;
        else
            material = 0;
    }
    const float s = water ? 1.0 : max(material * ((flags & 4) || material > 0.98 ? 1.0 : flat), wet > 0 ? wet * saturate((n.y - 0.55) / 0.3) * flat : 0.0);
    // (flags 524288) extended smooth surface only: at least 5 of its 8 neighbours 2 pixels away smooth too and on the same surface. A rail, a cobblestone edge, an
    // isolated smooth ridge received a reflection whose direction (from depth) changed every frame: lines that lit up and went out
    if ((flags & 524288) && !water && (flags & 4) == 0 && wet <= 0 && s >= sLow && p.z > 0)
    {
        uint ok = 0;
        for (uint j = 0; j < 8; j++)
        {
            static const int2 kRing[8] = { int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2), int2(2, 2), int2(-2, 2), int2(2, -2), int2(-2, -2) };
            const int2 o = int2(id) + kRing[j];
            const float3 q = PointAt(o);
            if (q.z > 0 && abs(q.z - p.z) < 0.05 * p.z + 0.05 && Smoothness(o) >= (s >= sMin ? sMin : sLow)) // (mirror: mirror neighbours too, v90's rule; glossy: glossy neighbours)
                ok++;
        }
        waterFade *= saturate((float(ok) - 4.0) / 3.0);
    }
    float4 result = float4(0, 0, 0, (flags & 16) ? s : 0.0);
    float4 mixResult = float4(0, 0, 0, 1);
    const bool useRad = (flags & 262144) != 0 && (flags & 24) == 0;
    const float nan = asfloat(0x7FC00000);
    uint4 anchorOut = uint4(asuint(nan), asuint(nan), 0, 0);
    // no ray where the reflection would be almost invisible (surface seen face on: Fresnel ~4 %): below 3 %
    const float fresnelEarly = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(-normalize(p), n)), 5.0);
    // sorting of glossy surfaces (below the mirror threshold): only a floor (flat, extended, facing up: polished stone, parquet), close
    // (faded from 10 to 18 m: beyond, the game's haze drowns everything and a reflection looked pasted on), not foliage or skin (Red Dead's
    // material type, red channel of its material target: 0.2 foliage, 0.31 skin). The world's up: camera orientation (Red Dead), otherwise the
    // game's; unknown: no glossy reflection. A first try without this sorting put them on 20 % of the screen, too much and too far (player's opinion)
    float glossRamp = 0;
    if (glossOn && p.z > 0 && s >= glossMin && s < sMin)
    {
        float3 upG = 0;
        if (sunBuf.Load(2092) != 0)
            upG = float3(asfloat(sunBuf.Load(2056)), asfloat(sunBuf.Load(2068)), asfloat(sunBuf.Load(2080)));
        else if (flags & 8192)
            upG = worldUp;
        const float kind = MaterialKind(int2(id));
        const bool skinOrLeaf = abs(kind - 0.2) < 0.04 || abs(kind - 0.31) < 0.04;
        // floor: the game's world normal facing up (Red Dead, without going through the camera orientation, which is only solved when
        // occlusion runs); otherwise the world's up in camera space
        const float3 gw = GNormalRaw(int2(id));
        const bool floorHere = any(gw != 0) ? gw.z > 0.85 : (dot(upG, upG) > 0.5 && dot(n, normalize(upG)) > 0.85);
        if (floorHere && !skinOrLeaf)
            glossRamp = glossAmt * saturate((s - glossMin) / max(1e-3, sMin - glossMin)) * saturate((18.0 - length(p)) / 8.0);
    }
    const float ramp = max(saturate((s - sMin) / max(1e-3, 1.0 - sMin)), glossRamp);
    if (p.z > 0 && s >= sLow && fresnelEarly * ramp * waterFade >= 0.03)
    {
        // mirror surface in the captured geometry: the first triangle on the camera ray, if it is at
        // the depth the image shows
        bool mirrorKnown = false;
        float3 prevMirror = 0, prevMirrorN = 0;
        if (anchors != 0)
        {
            RayDesc eye;
            eye.Origin = 0;
            eye.Direction = normalize(p);
            eye.TMin = 0.0;
            eye.TMax = length(p) * 1.05 + 0.1;
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qe;
            qe.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, eye);
            qe.Proceed();
            if (qe.CommittedStatus() == COMMITTED_TRIANGLE_HIT && abs(qe.CommittedRayT() - length(p)) < 0.02 * length(p) + 0.05)
            {
                uint mirrorId;
                mirrorKnown = AnchorOf(qe.CommittedInstanceID(), qe.CommittedPrimitiveIndex(), qe.CommittedTriangleBarycentrics(), mirrorId, prevMirror, prevMirrorN);
                anchorOut.w = mirrorId;
            }
        }
        const float3 v = normalize(p);
        const float3 r = reflect(v, n);
        const float cosTheta = saturate(dot(-v, n));
        // Fresnel (Schlick); smoothness above the threshold modulates the weight
        const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
        const float weight = fresnel * ramp * waterFade;
        // by default: reflection expected, colour unknown (replaced below if the reflected point is visible)
        if (weight > 0)
            result.z = -weight;
        if (flags & 131072)
        {
            Count(0);
            if (material > 0.98 || water)
                Count(4);
        }
        // time budget exceeded: one pixel in two or in four gets a ray this frame (the pattern rotates), the others take their neighbours' colour
        {
            const uint stride = budget & 15, frameNo = budget >> 8;
            static const uint2 kFour[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
            const bool skipNow = stride == 2 ? ((id.x + id.y + frameNo) & 1) != 0 : stride >= 4 ? any((id & 1u) != kFour[frameNo & 3]) : false;
            if (skipNow)
            {
                if (flags & 131072)
                    Count(12);
                reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(float4(kSkipped, 0, max(weight, 0.0), 0)));
                if (anchors != 0)
                    anchor.Store4(id.y * reflectPitch + id.x * 16, uint4(0x7FC00000, 0x7FC00000, 0, 0));
                return;
            }
        }
        RayDesc ray;
        ray.Origin = p + n * (bias * p.z);
        ray.Direction = r;
        ray.TMin = 0.05; // not the game's decals laid just above the surface
        ray.TMax = tMaxNow > 0 ? tMaxNow : 500.0; // beyond, the game's haze erases every reflection
        if (useRad)
        {
            // the ray moves from triangle to triangle (a few at most: budget). On screen, the image's depth decides: visible point
            // (its colour is in the image), hole of a cut-out card (the camera sees further: the ray goes on), or hidden. Hidden or off
            // screen: the colour and solid share the world cache kept there; the rest of the ray goes on behind. Nothing hit: the sky
            RadLoadParams();
            gRng = (id.x * 1973u + id.y * 9277u) ^ ((budget >> 8) * 26699u) ^ 0x9E3779B9u;
            const float eyeDist = length(p);
            float3 acc = 0;
            float T = 1.0, share = 0, tOut = -1.0;
            float2 uvOut = 0;
            bool unknown = false;
            RayDesc stepRay = ray;
            const uint maxHops = (budget >> 4) & 15;
            for (uint hop = 0; hop <= maxHops; hop++)
            {
                if (flags & 131072)
                    Count(8);
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
                qf.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, stepRay);
                qf.Proceed();
                if (qf.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                    break;
                const float th = qf.CommittedRayT();
                const uint inst = qf.CommittedInstanceID(), prim = qf.CommittedPrimitiveIndex(), contrib = qf.CommittedInstanceContributionToHitGroupIndex();
                const float3 h = ray.Origin + r * th;
                const bool last = hop == maxHops;
                uint state = 0; // 0 off screen, 1 visible, 2 hole (seen through), 3 hidden
                float2 uv = 0;
                float seenZ = 0;
                if (h.z > 1e-3)
                {
                    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
                    uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if (all(uv > 0.001) && all(uv < 0.999))
                    {
                        seenZ = PointAt(int2(uv * float2(width, height))).z;
                        state = seenZ > 0 && abs(seenZ - h.z) < 0.03 * h.z + 0.05 ? 1u : (seenZ <= 0 || seenZ > h.z) ? 2u : 3u;
                    }
                }
                if (state == 1)
                {
                    // toward the screen's edges, the image's colour gives way to the cache's (no cut at the edge)
                    const float2 e2 = saturate(min(uv, 1.0 - uv) * 12.0);
                    float edge = e2.x * e2.y;
                    float3 c = 0;
                    float a1 = 1;
                    if (edge < 1.0 && Remembered(h, eyeDist + th, inst, prim, contrib, c, a1) != 1)
                        edge = 1.0;
                    acc += T * (1.0 - edge) * c;
                    share = T * edge, uvOut = uv, T = 0;
                    if (tOut < 0)
                        tOut = th;
                    if (hop == 0)
                    {
                        // anchoring: reflected point in the previous frame, mirrored across the mirror's plane at that time
                        uint hitId;
                        float3 prevHit, prevHitN;
                        const bool hitKnown = AnchorOf(inst, prim, qf.CommittedTriangleBarycentrics(), hitId, prevHit, prevHitN);
                        anchorOut.z = hitId;
                        if (hitKnown && mirrorKnown)
                        {
                            const float3 virt = prevHit - 2.0 * dot(prevHit - prevMirror, prevMirrorN) * prevMirrorN;
                            if (virt.z > 1e-3)
                            {
                                const float2 pn = float2(virt.x * p00 / virt.z, virt.y * p11 / virt.z);
                                anchorOut.xy = asuint(float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5));
                            }
                        }
                    }
                    break;
                }
                if (state == 2 && (flags & 32) && !last)
                {
                    stepRay.TMin = th + max(1e-3, 1e-4 * th);
                    continue;
                }
                float3 c;
                float alpha;
                const uint found = Remembered(h, eyeDist + th, inst, prim, contrib, c, alpha);
                if (found == 2)
                {
                    // only holes here: the ray goes through (at the last hop: the sky behind)
                    stepRay.TMin = th + max(1e-3, 1e-4 * th);
                    continue;
                }
                if (found == 0)
                {
                    if (state == 2)
                        break; // hole never seen up close: the sky behind
                    // never seen, far behind other distant objects: same material, same haze, the screen's colour holds for it
                    if (state == 3 && h.z > 8.0 && seenZ > 0.35 * h.z)
                    {
                        share = T, uvOut = uv, T = 0;
                        if (tOut < 0)
                            tOut = th;
                        break;
                    }
                    unknown = true;
                    break;
                }
                if (flags & 131072)
                    Count(20);
                if (last)
                    alpha = 1.0;
                acc += T * alpha * c;
                T *= 1.0 - alpha;
                if (tOut < 0 && alpha > 0.3)
                    tOut = th;
                if (T < 0.04)
                {
                    T = 0;
                    break;
                }
                stepRay.TMin = th + max(1e-3, 1e-4 * th);
            }
            if (!unknown && T > 0)
            {
                // what is left of the ray goes into the distance: the sky, or the distance, as the screen shows it in that direction (sharper) when it
                // is there, otherwise as the sky map kept it
                float3 env;
                const bool envKnown = RadEnv(RadWorldDir(r), env);
                bool onScreen = false;
                float2 uv = 0;
                if (r.z > 1e-3)
                {
                    const float2 ndc = float2(r.x * p00 / r.z, r.y * p11 / r.z);
                    uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if (all(uv > 0.001) && all(uv < 0.999))
                    {
                        const float seenFar = PointAt(int2(uv * float2(width, height))).z;
                        onScreen = seenFar <= 0 || seenFar > 20.0;
                    }
                }
                if (onScreen)
                {
                    const float2 e2 = saturate(min(uv, 1.0 - uv) * 12.0);
                    const float edge = envKnown ? e2.x * e2.y : 1.0;
                    acc += T * (1.0 - edge) * env;
                    share = T * edge, uvOut = uv, T = 0;
                }
                else if (envKnown)
                {
                    acc += T * env, T = 0;
                    if (flags & 131072)
                        Count(24);
                }
                else
                    unknown = true;
                if (tOut < 0)
                    tOut = 1e4;
            }
            // a small unknown share: the rest of the ray stands for it; almost all unknown: reflection expected, colour unknown (as before)
            if (!(unknown && T > 0.5))
            {
                const float k = 1.0 / max(1.0 - T, 1e-3);
                result = float4(uvOut, weight, tOut < 0 ? 20.0 : tOut);
                mixResult = float4(acc * k, share * k);
            }
            else if (flags & 131072)
                Count(28);
        }
        else
        {
        // first an opaque ray (the hardware alone finds the closest); only if that point is a hole the camera sees through
        // (cut-out foliage card), the slow search where every triangle met is examined. Same result: before, every ray
        // examined every triangle (ground, rocks, trunks included)
        bool hitNow = false;
        float hitT = 0;
        uint hitInst = 0, hitPrim = 0, hitContrib = 0;
        float2 hitBary = 0;
        // hole of a leaf: next opaque ray, right after it, at most a few times (budget); the last point hit is kept (foliage, whether seen
        // through or not). A ray's cost is bounded: six hops followed by an unbounded search made 20 ms of reflections in dense vegetation
        bool holeLeft = false;
        RayDesc stepRay = ray;
        const uint maxHops = (budget >> 4) & 15;
        for (uint hop = 0; hop <= maxHops; hop++)
        {
            if (flags & 131072)
                Count(8);
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
            qf.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, stepRay);
            qf.Proceed();
            hitNow = qf.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
            if (!hitNow)
                break;
            hitT = qf.CommittedRayT(), hitInst = qf.CommittedInstanceID(), hitPrim = qf.CommittedPrimitiveIndex(),
            hitContrib = qf.CommittedInstanceContributionToHitGroupIndex(), hitBary = qf.CommittedTriangleBarycentrics();
            holeLeft = (flags & 32) && SeenThrough(ray.Origin + r * hitT);
            if (!holeLeft)
                break;
            if (hop == maxHops && (flags & 131072))
                Count(16);
            stepRay.TMin = hitT + max(1e-3, 1e-4 * hitT);
        }
        // (flags 32768: old rule, every triangle examined, to compare)
        if ((flags & 32) && (flags & 32768))
        {
            RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
            while (q.Proceed())
                if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && !SeenThrough(ray.Origin + r * q.CandidateTriangleRayT()))
                    q.CommitNonOpaqueTriangleHit();
            hitNow = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
            if (hitNow)
                hitT = q.CommittedRayT(), hitInst = q.CommittedInstanceID(), hitPrim = q.CommittedPrimitiveIndex(),
                hitContrib = q.CommittedInstanceContributionToHitGroupIndex(), hitBary = q.CommittedTriangleBarycentrics();
        }
        // diagnostic (flags 8, view 7): why no reflection. 0.1 nothing hit and backward; 0.2 nothing hit, no
        // sky at that place on screen; 0.4 hit off screen; 0.6 hit, the screen shows something closer there (hidden); 0.8
        // hit, the screen shows something further there
        float why = hitNow ? 0.4 : (r.z > 1e-3 ? 0.2 : 0.1);
        if (!hitNow && r.z > 1e-3)
        {
            // nothing hit: the ray goes into the distance. What is at infinity is reflected as it is seen in that direction: the
            // sky, or the distance the screen shows there (forest drowned in haze beyond the RT scene: almost the whole reflection
            // of a lake seen at a grazing angle)
            const float2 ndc = float2(r.x * p00 / r.z, r.y * p11 / r.z);
            const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            const float seenFar = PointAt(int2(clamp(uv, 0.0, 0.999) * float2(width, height))).z;
            if (all(uv > 0.001) && all(uv < 0.999) && (seenFar <= 0 || seenFar > 20.0))
            {
                const float2 edge = saturate(min(uv, 1.0 - uv) * 12.0);
                result = float4(uv, weight * edge.x * edge.y, (flags & 16) ? s : 1e4);
            }
        }
        else if (hitNow && r.z > -0.999)
        {
            const float3 h = ray.Origin + r * hitT;
            bool onScreen = false;
            if (h.z > 1e-3)
            {
                const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
                const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                if (all(uv > 0.001) && all(uv < 0.999))
                {
                    // the hit point must be the one the image shows at that place (otherwise it is hidden)
                    const float3 seen = PointAt(int2(uv * float2(width, height)));
                    why = seen.z > 0 && seen.z < h.z ? 0.6 : 0.8;
                    // distant point behind other distant objects: the reflected ray hits foliage in depth,
                    // the camera sees the one in front: same material, same haze, the screen's colour holds for it. Except
                    // behind a clearly closer object (character, foreground grass)
                    const bool behindFar = h.z > 8.0 && seen.z > 0.35 * h.z && seen.z < h.z;
                    if (seen.z > 0 && (abs(seen.z - h.z) < 0.03 * h.z + 0.05 || behindFar))
                    {
                        // anchoring: reflected point in the previous frame, mirrored across the mirror's plane at that time
                        uint hitId;
                        float3 prevHit, prevHitN;
                        const bool hitKnown = AnchorOf(hitInst, hitPrim, hitBary, hitId, prevHit, prevHitN);
                        anchorOut.z = hitId;
                        if (hitKnown && mirrorKnown)
                        {
                            const float3 virt = prevHit - 2.0 * dot(prevHit - prevMirror, prevMirrorN) * prevMirrorN;
                            if (virt.z > 1e-3)
                            {
                                const float2 pn = float2(virt.x * p00 / virt.z, virt.y * p11 / virt.z);
                                anchorOut.xy = asuint(float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5));
                            }
                        }
                        // fade toward the screen's edges (colour unknown beyond)
                        const float2 edge = saturate(min(uv, 1.0 - uv) * 12.0);
                        result = float4(uv, weight * edge.x * edge.y, (flags & 8) ? saturate(hitT / 50.0) : (flags & 16) ? s : hitT);
                        onScreen = true;
                    }
                }
            }
            // hidden or off screen: the colour the surface cache kept for this triangle (not in
            // the diagnostic views, which read the 4th component)
            float3 cached;
            if (!onScreen && (flags & 24) == 0 && hitInst != 0 && CachedGeo(hitInst, hitPrim, hitContrib, cached))
                result = float4(-1.0 - cached.r, cached.g, weight, cached.b); // cached object: its learned colour
            else if (!onScreen && (flags & 24) == 0 && hitInst == 0 && Cached(hitPrim, cached))
            {
                uint hitId;
                float3 prevHit, prevHitN;
                const bool hitKnown = Anchor(hitPrim, hitBary, hitId, prevHit, prevHitN);
                anchorOut.z = hitId;
                if (hitKnown && mirrorKnown)
                {
                    const float3 virt = prevHit - 2.0 * dot(prevHit - prevMirror, prevMirrorN) * prevMirrorN;
                    if (virt.z > 1e-3)
                    {
                        const float2 pn = float2(virt.x * p00 / virt.z, virt.y * p11 / virt.z);
                        anchorOut.xy = asuint(float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5));
                    }
                }
                result = float4(-1.0 - cached.r, cached.g, weight, cached.b);
            }
        }
        if ((flags & 8) && result.z <= 0)
            result.w = why;
        }
        // diagnostic (reflect_diag=): reflected ray direction, component 1: y, 2: z, 3: normal y, 4: normal z
        if (flags & 512)
            result.w = 0.5 + 0.5 * r.y;
        if (flags & 1024)
            result.w = 0.5 + 0.5 * r.z;
        if (flags & 2048)
            result.w = 0.5 + 0.5 * n.y;
        if (flags & 4096)
            result.w = 0.5 + 0.5 * n.z;
    }
    // glossy reflection: blur radius (render pixels) stored with the reflected point's distance, w = distance + 20,000 × radius (live_reflresolve
    // reads it back). Lobe of a rough material: half-angle ~ alpha = roughness², footprint at the reflected point seen from the camera = distance × alpha / (eye
    // distance + distance), like a mirror's virtual image. Named approximation: one mirror ray, then a blurred read, no rays drawn in
    // the lobe (no noise, but the blur ignores what is hidden on screen)
    // (glossy surfaces only, below the mirror threshold: a puddle, smoothness 0.60, also got this blur in v99-v100 and its reflection became blurry)
    if (glossOn && s < sMin && glossRamp > 0 && (flags & 24) == 0 && result.z > 0 && result.x >= -0.5 && result.w >= 0 && result.w < 2e4)
    {
        const float rough = 1.0 - saturate(s), alpha = rough * rough;
        if (alpha > 0.02)
        {
            const float t = result.w, eye = length(p);
            const float blurPx = min(round(t * alpha / max(eye + t, 1e-3) * p11 * float(height) * 0.5), 60.0);
            result.w = t + 2e4 * blurPx;
        }
    }
    reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(result));
    if (useRad)
        mixOut.Store4(id.y * reflectPitch + id.x * 16, asuint(mixResult));
    if (anchors != 0)
        anchor.Store4(id.y * reflectPitch + id.x * 16, anchorOut);
}
