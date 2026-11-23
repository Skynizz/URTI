// Live URTI: geometry cache. This frame's camera pose (live_camfit) written into every cache copy of
// the top-level structure: every kept object follows the camera without being rebuilt.

RWByteAddressBuffer instances : register(u0);   // D3D12_RAYTRACING_INSTANCE_DESC (64 bytes), the first 48: Transform
RWByteAddressBuffer motion : register(u1);      // bytes 128 to 175: pose (R by rows, t) from the cache's frame to the camera

cbuffer Constants : register(b0)
{
    uint first; // first cache copy
    uint count; // end of the copies
    uint pad0;
    uint pad1;
};

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const uint i = first + id;
    if (i >= count)
        return;
    const float3 r0 = asfloat(motion.Load3(128)), r1 = asfloat(motion.Load3(140)), r2 = asfloat(motion.Load3(152)), t = asfloat(motion.Load3(164));
    // the game's sub-pixel jitter in this frame: cache objects fall where this frame would have captured them
    const float2 shear = asfloat(motion.Load2(176));
    instances.Store4(i * 64, asuint(float4(r0 + shear.x * r2, t.x + shear.x * t.z)));
    instances.Store4(i * 64 + 16, asuint(float4(r1 + shear.y * r2, t.y + shear.y * t.z)));
    instances.Store4(i * 64 + 32, asuint(float4(r2, t.z)));
}
