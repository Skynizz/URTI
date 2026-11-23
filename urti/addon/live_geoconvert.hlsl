// Live URTI: geometry cache. Objects captured in turn (measurement area of the stream) are converted into the
// cache's fixed frame (camera space of the last reset): vertex = R^T (camera space - t), with the camera
// pose accumulated by live_camfit. Each object then gets its own acceleration structure, kept from one frame to
// the next and placed in the RT scene by that same pose. One group per object. Unwritten or aberrant vertices: NaN
// (inactive triangle).

StructuredBuffer<float4> clip : register(t0);   // the screen's stream (start of its slot)
ByteAddressBuffer counters : register(t1);      // bytes written per draw
StructuredBuffer<uint4> regions : register(t2); // per object: start in the stream (bytes), size (bytes), counter,
                                                // start in view (bytes)
RWByteAddressBuffer view : register(u0);        // float3 per vertex, cache frame
RWByteAddressBuffer motion : register(u1);      // bytes 128 to 175: pose (R by rows, t) from the cache's frame to the camera

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint regionCount;
    uint pad;
};

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex)
{
    if (group.x >= regionCount)
        return;
    const uint4 r = regions[group.x];
    const uint first = r.x / 16, total = r.y / 16 / 3 * 3, filled = min(counters.Load(r.z * 8), r.y) / 16 / 3 * 3;
    const float3 r0 = asfloat(motion.Load3(128)), r1 = asfloat(motion.Load3(140)), r2 = asfloat(motion.Load3(152)), t = asfloat(motion.Load3(164));
    const float nan = asfloat(0x7FC00000);
    const float2 shear = asfloat(motion.Load2(176)); // the game's sub-pixel jitter in this frame (game camera read)
    for (uint i = ti; i < total; i += 64)
    {
        float3 v = float3(nan, nan, nan);
        if (i < filled)
        {
            const float4 c = clip[first + i];
            const float3 cv = float3(c.x / p00 - shear.x * c.w, c.y / p11 - shear.y * c.w, c.w);
            if (all(isfinite(cv)) && all(abs(cv) < 20000.0))
            {
                const float3 d = cv - t;
                v = float3(r0.x * d.x + r1.x * d.y + r2.x * d.z, r0.y * d.x + r1.y * d.y + r2.y * d.z, r0.z * d.x + r1.z * d.y + r2.z * d.z);
            }
        }
        view.Store3(r.w + i * 12, asuint(v));
    }
}
