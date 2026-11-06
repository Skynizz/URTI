// URTI en direct : cache de géométrie. Pose de la caméra de cette image (live_camfit) écrite dans chaque exemplaire du
// cache de la structure de tête : tous les objets gardés suivent la caméra sans être reconstruits.

RWByteAddressBuffer instances : register(u0);   // D3D12_RAYTRACING_INSTANCE_DESC (64 octets), les 48 premiers : Transform
RWByteAddressBuffer motion : register(u1);      // octets 128 à 175 : pose (R par lignes, t) du repère du cache vers la caméra

cbuffer Constants : register(b0)
{
    uint first; // premier exemplaire du cache
    uint count; // fin des exemplaires
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
    instances.Store4(i * 64, asuint(float4(r0, t.x)));
    instances.Store4(i * 64 + 16, asuint(float4(r1, t.y)));
    instances.Store4(i * 64 + 32, asuint(float4(r2, t.z)));
}
