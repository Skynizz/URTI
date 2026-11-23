// Live URTI: camera pose realigned on reference points. Each witness (small still object, far from the
// camera) keeps sixteen of its vertices in the geometry cache's fixed frame, recorded the first time it is seen. On
// every frame, these points placed by the last known pose and their real position in this frame give the pairs
// for live_camfit: the pose correction. Nothing accumulates: a frame without a measurement (capture window without a witness,
// game frame split between two presents) loses no displacement, and the game's sub-pixel jitter (TAA)
// does not add up from one frame to the next.

StructuredBuffer<float4> clip : register(t0);   // the screen's stream (start of its slot)
ByteAddressBuffer counters : register(t1);      // bytes written per draw
StructuredBuffer<uint4> table : register(t2);   // per witness: start in the stream (bytes), size, counter, place of the
                                                // points (bit 31: new witness)
RWByteAddressBuffer refs : register(u0);        // per witness, 208 bytes: 16 × float3 (cache frame), number of vertices
RWByteAddressBuffer pairs : register(u1);       // number of pairs, then per pair: p.xyz, q.xyz
RWByteAddressBuffer motion : register(u2);      // bytes 128 to 175: last pose (R by rows, t)

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint count;
    uint maxPairs;
    uint identityPose; // 1: the frame restarts from this frame (the pose read is no longer valid)
    uint pad0;
    uint pad1;
    uint pad2;
};

#define S 16
groupshared uint gFresh;

[numthreads(S, 1, 1)]
void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex)
{
    if (group.x >= count)
        return;
    const uint4 r = table[group.x];
    const uint n = min(counters.Load(r.z * 8), r.y) / 16;
    if (n < S)
        return;
    const uint base = (r.w & 0x7FFFFFFF) * 208;
    // new witness, or another number of vertices (another level of detail of the same mesh): points recorded
    if (ti == 0)
        gFresh = ((r.w >> 31) != 0 || refs.Load(base + 192) != n) ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    const float4 c = clip[r.x / 16 + ti * (n / S)];
    const float2 shear = asfloat(motion.Load2(176)); // the game's sub-pixel jitter (game camera read; 0 otherwise)
    const float3 q = float3(c.x / p00 - shear.x * c.w, c.y / p11 - shear.y * c.w, c.w);
    const bool ok = all(isfinite(q)) && all(abs(q) < 20000.0);
    float3 x0 = float3(1, 0, 0), x1 = float3(0, 1, 0), x2 = float3(0, 0, 1), xt = 0;
    if (identityPose == 0)
        x0 = asfloat(motion.Load3(128)), x1 = asfloat(motion.Load3(140)), x2 = asfloat(motion.Load3(152)), xt = asfloat(motion.Load3(164));
    if (gFresh != 0)
    {
        const float3 d = q - xt;
        const float nan = asfloat(0x7FC00000);
        const float3 w = ok ? float3(x0.x * d.x + x1.x * d.y + x2.x * d.z, x0.y * d.x + x1.y * d.y + x2.y * d.z, x0.z * d.x + x1.z * d.y + x2.z * d.z) : float3(nan, nan, nan);
        refs.Store3(base + ti * 12, asuint(w));
        if (ti == 0)
            refs.Store(base + 192, n);
        return;
    }
    const float3 w = asfloat(refs.Load3(base + ti * 12));
    if (!ok || !all(isfinite(w)))
        return;
    const float3 p = float3(dot(x0, w), dot(x1, w), dot(x2, w)) + xt;
    uint k;
    pairs.InterlockedAdd(0, 1, k);
    if (k < maxPairs)
        pairs.Store3(16 + k * 24, asuint(p)), pairs.Store3(16 + k * 24 + 12, asuint(q));
}
