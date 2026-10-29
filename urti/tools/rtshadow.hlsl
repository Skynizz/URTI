// URTI : ombres du soleil en ray tracing sur la géométrie captée (requêtes de rayons en ligne, SM 6.5).
// Un fil par pixel : point du jeu reconstruit depuis sa profondeur, rayon vers le soleil.

RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float4> positions : register(t1); // espace caméra ; w = 0 : ciel
RWStructuredBuffer<float> shadow : register(u0);   // 1 éclairé, 0 à l'ombre

cbuffer Constants : register(b0)
{
    float3 sun;    // vers le soleil, espace caméra
    float bias;    // décalage relatif à la distance (contre l'auto-intersection)
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
    ray.Origin = p.xyz * (1.0 - bias) + sun * bias * dist; // un peu vers la caméra et vers le soleil
    ray.Direction = sun;
    ray.TMin = 0.0;
    ray.TMax = maxDistance;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    shadow[id] = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
