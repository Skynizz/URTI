// Live URTI: motion vectors built from the camera motion measured on our geometry
// (live_camfit) and this frame's depth, when the game's stay unreadable (seen in one game:
// no convention could be told apart). For each pixel: its point in this frame's camera space, brought back
// into the previous frame's (p = R^T (q - t)), projected: where it was. Sky: rotation only. Correct
// for what is still; a moving character is rejected by the histories (depth difference).
// Written in the game format expected by the passes that read them: two half floats, convention 0 (previous
// position = uv + vector, screen units 0..1).

ByteAddressBuffer depth : register(t0);
ByteAddressBuffer sunBuf : register(t1); // depth law (bytes 36 to 47)
ByteAddressBuffer motion : register(t2); // R (by rows), t, validity (live_camfit)
RWByteAddressBuffer mv : register(u0);

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint depth24;
    uint mvPitch;
    float p00;
    float p11;
    uint pad0;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    float2 m = 0;
    if (asfloat(motion.Load(48)) > 0.5)
    {
        const float3 law = asfloat(sunBuf.Load3(36));
        const float3 r0 = asfloat(motion.Load3(0)), r1 = asfloat(motion.Load3(12)), r2 = asfloat(motion.Load3(24));
        const float3 t = asfloat(motion.Load3(36));
        const uint raw = depth.Load(id.y * depthPitch + id.x * 4);
        const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
        const float w = law.z > 0 ? law.y / (d - law.x) : -1.0;
        const float clearValue = law.y > 0 ? 0.0 : 1.0;
        const float2 uv = (float2(id) + 0.5) / float2(width, height);
        const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
        float3 q = float3(ndc.x / p00, ndc.y / p11, 1.0);
        const bool sky = !(d != clearValue && w > 0 && w < 1e6);
        if (!sky)
            q *= w;
        // R^T (q - t): the columns of R; the sky (at infinity) follows only the rotation
        const float3 a = sky ? q : q - t;
        const float3 p = float3(r0.x * a.x + r1.x * a.y + r2.x * a.z, r0.y * a.x + r1.y * a.y + r2.y * a.z, r0.z * a.x + r1.z * a.y + r2.z * a.z);
        if (p.z > 1e-3)
        {
            const float2 ndcPrev = float2(p.x * p00 / p.z, p.y * p11 / p.z);
            const float2 uvPrev = float2(ndcPrev.x * 0.5 + 0.5, 0.5 - ndcPrev.y * 0.5);
            m = uvPrev - uv;
        }
    }
    mv.Store(id.y * mvPitch + id.x * 4, f32tof16(m.x) | (f32tof16(m.y) << 16));
}
