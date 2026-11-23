// Live URTI: the game's depth law measured every frame (clip z = A w + B, hence
// depth = A + B / w), by least squares on vertices of the screen geometry captured in
// this frame (regions per draw, only their filled part). Written into the sun buffer
// (bytes 36 to 47: A, B, validity); without enough vertices, the previous value stays.

ByteAddressBuffer so : register(t0);            // the screen's stream (start of its slot)
ByteAddressBuffer counters : register(t1);      // bytes written per draw
StructuredBuffer<uint4> regions : register(t2); // per draw: start (bytes), size, counter
RWByteAddressBuffer sun : register(u0);

cbuffer Constants : register(b0)
{
    uint regionCount;
    uint totalBytes; // bytes of the frame's regions
    uint pad0;
    uint pad1;
};

#define T 256
#define SAMPLES 8192
groupshared double sAcc[T][5];

uint4 RegionOf(uint byte)
{
    uint lo = 0, hi = regionCount;
    while (lo + 1 < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (regions[mid].x <= byte)
            lo = mid;
        else
            hi = mid;
    }
    return regions[lo];
}

[numthreads(T, 1, 1)]
void main(uint t : SV_GroupThreadID)
{
    const uint vertices = totalBytes / 16;
    const uint step = max(1u, vertices / SAMPLES);
    double acc[5] = { 0, 0, 0, 0, 0 }; // n, Σw, Σz, Σw², Σwz
    for (uint i = t * step; i < vertices && regionCount > 0; i += T * step)
    {
        const uint4 r = RegionOf(i * 16);
        if (i * 16 < r.x || i * 16 - r.x >= min(counters.Load(r.z * 8), r.y))
            continue;
        const float4 c = asfloat(so.Load4(i * 16));
        if (c.w > 1e-3 && c.w < 1e6 && abs(c.z) < 1e6)
            acc[0] += 1, acc[1] += c.w, acc[2] += c.z, acc[3] += double(c.w) * c.w, acc[4] += double(c.w) * c.z;
    }
    for (uint k = 0; k < 5; k++)
        sAcc[t][k] = acc[k];
    GroupMemoryBarrierWithGroupSync();
    for (uint s = T / 2; s > 0; s >>= 1)
    {
        if (t < s)
            for (uint k2 = 0; k2 < 5; k2++)
                sAcc[t][k2] += sAcc[t + s][k2];
        GroupMemoryBarrierWithGroupSync();
    }
    if (t != 0)
        return;
    const double n = sAcc[0][0], sw = sAcc[0][1], sz = sAcc[0][2], sww = sAcc[0][3], swz = sAcc[0][4];
    const double den = n * sww - sw * sw;
    if (n < 64 || abs(den) < 1e-12 * n * n)
        return;
    const double A = (n * swz - sw * sz) / den, B = (sz - A * sw) / n;
    if (!(abs(B) > 1e-9) || !(abs(A) < 1e3))
        return;
    sun.Store3(36, asuint(float3(float(A), float(B), 1.0)));
}
