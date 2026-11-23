// URTI: ray-traced sun shadows on the captured geometry (inline ray queries, SM 6.5).
// One thread per pixel: game point rebuilt from its depth, ray toward the sun.

RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float4> positions : register(t1); // camera space; w = 0: sky
RWStructuredBuffer<float> shadow : register(u0);   // 1 lit, 0 in shadow

cbuffer Constants : register(b0)
{
    float3 sun;    // toward the sun, camera space
    float bias;    // offset relative to the distance (against self-intersection)
    uint count;
    float maxDistance;
    float2 pad;
};

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= count)
        return;
    const float4 p = positions[id];
    if (p.w == 0)
    {
        shadow[id] = 1;
        return;
    }
    const float dist = length(p.xyz);
    RayDesc ray;
    ray.Origin = p.xyz * (1.0 - bias) + sun * bias * dist; // a bit toward the camera and toward the sun
    ray.Direction = sun;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    shadow[id] = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
