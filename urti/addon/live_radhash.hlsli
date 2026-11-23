// Live URTI: world radiance cache (spatial hash table, after NVIDIA's SHaRC) and sky and distance map.
// The game's camera gives the world frame: each point the screen shows deposits its colour there (live_radcache), in a cell whose
// size grows with distance (a few pixels on screen); a reflection hitting a hidden or off-screen point reads back the
// colour seen there (live_reflect). Each cell also keeps its solid share: foliage is made of cut-out cards whose captured
// geometry is just the rectangle, the camera sees through their holes when they are on screen, and what it saw holds off screen.
// To define before inclusion: RAD_BUF (table), RAD_ENV (map), RAD_PARAMS (the frame's settings).
// Cell (16 bytes): key; r, g (16-bit floats); b, date (quarter seconds); colour measurements, solid share, solid-share measurements (8 bits).

static float3 gCamRight, gCamUp, gCamFwd, gCamPos;
static float2 gShear;
static float gVoxel0, gD0, gUpSign, gEnvFar;
static uint gRadMask, gEnvSize, gUpAxis, gRadTick, gRadFrame;

void RadLoadParams()
{
    const float4 a = asfloat(RAD_PARAMS.Load4(0)), b = asfloat(RAD_PARAMS.Load4(16)), c = asfloat(RAD_PARAMS.Load4(32)), d = asfloat(RAD_PARAMS.Load4(48));
    gCamRight = a.xyz, gCamUp = b.xyz, gCamFwd = c.xyz, gCamPos = d.xyz;
    gShear = float2(a.w, b.w), gVoxel0 = c.w, gD0 = d.w;
    const uint4 e = RAD_PARAMS.Load4(64);
    gRadMask = e.x, gEnvSize = e.y, gUpAxis = e.z, gUpSign = asfloat(e.w);
    const uint4 f = RAD_PARAMS.Load4(80);
    gRadTick = f.x, gRadFrame = f.y, gEnvFar = asfloat(f.z);
}

// captured camera space (shifted by the game's sub-pixel jitter) -> world
float3 RadWorld(float3 h)
{
    const float2 t = h.xy - gShear * h.z;
    return gCamPos + gCamRight * t.x + gCamUp * t.y + gCamFwd * h.z;
}
float3 RadWorldDir(float3 d)
{
    const float2 t = d.xy - gShear * d.z;
    return normalize(gCamRight * t.x + gCamUp * t.y + gCamFwd * d.z);
}

uint RadPcg(uint x)
{
    x = x * 747796405u + 2891336453u;
    x = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (x >> 22u) ^ x;
}

static const uint kRadLevels = 13;
// level from the distance travelled to the point (from the eye, mirror included): cells of a few pixels at any distance
uint RadLevel(float dist) { return uint(clamp(floor(log2(max(dist, gD0) / gD0)), 0.0, float(kRadLevels - 1))); }

void RadKey(float3 world, uint level, out uint bucket, out uint tag)
{
    const float size = gVoxel0 * exp2(float(level));
    const int3 c = int3(floor(world / size));
    const uint h = RadPcg(uint(c.x) + RadPcg(uint(c.y) + RadPcg(uint(c.z) + level * 0x9E3779B9u)));
    bucket = (h & gRadMask) & ~3u; // four cells in a row (64 bytes)
    tag = RadPcg((uint(c.z) * 0x9E3779B1u) ^ RadPcg(uint(c.x) ^ RadPcg(uint(c.y) + level * 0x85EBCA6Bu + 0x1234567u)));
    tag = tag ? tag : 1u;
}

struct RadEntry
{
    float3 c;
    float alpha;
    uint nC, nA;
};

bool RadRead(float3 world, uint level, out RadEntry e)
{
    e.c = 0, e.alpha = 1, e.nC = 0, e.nA = 0;
    uint bucket, tag;
    RadKey(world, level, bucket, tag);
    for (uint i = 0; i < 4; i++)
    {
        const uint4 v = RAD_BUF.Load4((bucket + i) * 16);
        if (v.x == tag)
        {
            e.c = float3(f16tof32(v.y & 0xFFFF), f16tof32(v.y >> 16), f16tof32(v.z & 0xFFFF));
            e.nC = v.w & 255, e.alpha = float((v.w >> 8) & 255) / 255.0, e.nA = (v.w >> 16) & 255;
            return true;
        }
    }
    return false;
}

// colour and solid share kept at this point: at this level, then coarser (the point was seen from further away), then one step finer.
// A single cell read, at a position drawn at random within a neighbourhood the size of a cell (jitter, 0 to 1 per axis, new every frame):
// smoothing between cells comes from the reflections' accumulation over time (as in NVIDIA's SHaRC). Reading the eight neighbouring cells cost up to
// forty memory reads per point, at every foliage crossing (17 ms of reflections in busy scenes)
// 0: never seen (alpha still holds if the solid share is known); 1: colour and solid share; 2: only holes here (no colour)
uint RadLookup(float3 world, uint level, float3 jitter, out float3 c, out float alpha)
{
    static const int kTry[4] = { 0, 1, 2, -1 };
    bool haveAlpha = false;
    c = 0, alpha = 1;
    for (uint k = 0; k < 4; k++)
    {
        const int l = int(level) + kTry[k];
        if (l < 0 || l >= int(kRadLevels))
            continue;
        const float size = gVoxel0 * exp2(float(l));
        RadEntry e;
        if (!RadRead(world + (jitter - 0.5) * size, uint(l), e) && !RadRead(world, uint(l), e))
            continue;
        if (!haveAlpha && e.nA > 0)
            alpha = e.alpha, haveAlpha = true;
        if (e.nC > 0 && all(isfinite(e.c)))
        {
            c = e.c;
            return 1;
        }
        if (haveAlpha && alpha < 0.1)
            return 2;
    }
    return 0;
}

// sky and distance map: unfolded octahedron, the world's up at the centre
float2 RadOct(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 e = n.z >= 0 ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    return e * 0.5 + 0.5;
}
float3 RadEnvBasis(float3 w)
{
    const float3 v = gUpAxis == 2 ? w.xyz : gUpAxis == 1 ? w.zxy : w.yzx;
    return float3(v.xy, v.z * gUpSign);
}
bool RadEnv(float3 worldDir, out float3 c)
{
    c = 0;
    if (gEnvSize == 0)
        return false;
    const float2 q = RadOct(RadEnvBasis(worldDir)) * float(gEnvSize) - 0.5;
    const int2 b = int2(floor(q));
    const float2 f = q - b;
    float3 s = 0;
    float w = 0;
    for (int j = 0; j < 4; j++)
    {
        const int2 t = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(gEnvSize - 1, gEnvSize - 1));
        const float4 v = asfloat(RAD_ENV.Load4((uint(t.y) * gEnvSize + uint(t.x)) * 16));
        const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
        if (v.a > 0 && all(isfinite(v.rgb)))
            s += v.rgb * bw, w += bw;
    }
    if (w < 1e-4)
        return false;
    c = s / w;
    return true;
}
