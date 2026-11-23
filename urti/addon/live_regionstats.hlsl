// Live URTI: measurement of each captured object (one group per draw) for the geometry budget:
// closest distance to the camera and size (diagonal of its box, camera space). The ratio of the two
// (apparent size) tells its importance: a big close object counts, a small distant one does not.
// Read by the CPU three frames later; out-of-budget draws are re-measured in the measurement
// area (end of the slot, outside the acceleration structure).

StructuredBuffer<float4> clip : register(t0);   // the screen's stream (start of its slot)
ByteAddressBuffer counters : register(t1);      // bytes written per draw
StructuredBuffer<uint4> regions : register(t2); // per draw: start (bytes), size, counter, identity
RWByteAddressBuffer stats : register(u0);       // float4 per draw: minimum distance, size, vertices, 0

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint regionCount;
    uint pad;
};

groupshared float gMin[64];
groupshared float3 gLo[64], gHi[64];

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex)
{
    const uint k = group.x;
    if (k >= regionCount)
        return;
    const uint4 r = regions[k];
    const uint n = min(counters.Load(r.z * 8), r.y) / 16;
    const uint first = r.x / 16;
    // very big draws: one vertex in s is enough for the box (at most ~16,000 read per draw)
    const uint stride = 64 * max(1u, n / 16384);
    float mz = 1e9;
    float3 lo = 1e9, hi = -1e9;
    for (uint i = ti * (stride / 64); i < n; i += stride)
    {
        const float4 c = clip[first + i];
        if (c.w > 0.05 && all(isfinite(c)))
        {
            const float3 v = float3(c.x / p00, c.y / p11, c.w);
            mz = min(mz, v.z);
            lo = min(lo, v), hi = max(hi, v);
        }
    }
    gMin[ti] = mz, gLo[ti] = lo, gHi[ti] = hi;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 32; s > 0; s >>= 1)
    {
        if (ti < s)
        {
            gMin[ti] = min(gMin[ti], gMin[ti + s]);
            gLo[ti] = min(gLo[ti], gLo[ti + s]);
            gHi[ti] = max(gHi[ti], gHi[ti + s]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (ti == 0)
    {
        const float size = all(gHi[0] >= gLo[0]) ? length(gHi[0] - gLo[0]) : 0.0;
        stats.Store4(k * 16, asuint(float4(gMin[0], size, float(n), 0)));
    }
}
