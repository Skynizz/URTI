// URTI en direct : planéité de chaque objet capté (scène de l'image et objets captés à leur tour), un groupe par objet :
// normale moyenne, cohérence des facettes, écart des sommets au plan moyen, taille, distance de la caméra au plan. Sert à
// reconnaître une surface d'eau parmi les objets opaques. Espace caméra de cette image.

StructuredBuffer<float4> clip : register(t0);   // flux de l'écran (début de son emplacement)
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin
StructuredBuffer<uint4> regions : register(t2); // par objet : début (octets), taille, compteur, identité
RWByteAddressBuffer result : register(u0);      // 32 octets par objet : triangles, cohérence, écart, taille, normale, distance

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint regionCount;
    uint pad;
};

#define T 64
groupshared float3 gN[T], gC[T], gLo[T], gHi[T];
groupshared float gArea[T], gE[T];

float3 At(uint i)
{
    const float4 c = clip[i];
    return float3(c.x / p00, c.y / p11, c.w);
}

[numthreads(T, 1, 1)]
void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex)
{
    if (group.x >= regionCount)
        return;
    const uint4 r = regions[group.x];
    const uint triangles = min(counters.Load(r.z * 8), r.y) / 48, first = r.x / 16;
    const uint skip = max(1u, triangles / 4096); // gros objet : un triangle sur skip
    float3 n = 0, c = 0, lo = 1e30, hi = -1e30;
    float area = 0;
    for (uint t = ti * skip; t < triangles; t += T * skip)
    {
        const float3 a = At(first + t * 3), b = At(first + t * 3 + 1), d = At(first + t * 3 + 2);
        if (!all(isfinite(a + b + d)) || any(abs(a) > 20000.0) || any(abs(b) > 20000.0) || any(abs(d) > 20000.0))
            continue;
        float3 m = cross(b - a, d - a);
        if (dot(m, a) > 0)
            m = -m;
        const float s = length(m);
        n += m, area += s, c += s * (a + b + d) / 3.0;
        lo = min(lo, min(a, min(b, d))), hi = max(hi, max(a, max(b, d)));
    }
    gN[ti] = n, gC[ti] = c, gArea[ti] = area, gLo[ti] = lo, gHi[ti] = hi;
    GroupMemoryBarrierWithGroupSync();
    for (uint s2 = T / 2; s2 > 0; s2 >>= 1)
    {
        if (ti < s2)
            gN[ti] += gN[ti + s2], gC[ti] += gC[ti + s2], gArea[ti] += gArea[ti + s2], gLo[ti] = min(gLo[ti], gLo[ti + s2]), gHi[ti] = max(gHi[ti], gHi[ti + s2]);
        GroupMemoryBarrierWithGroupSync();
    }
    const float A = gArea[0];
    const float3 N = A > 0 ? gN[0] / max(length(gN[0]), 1e-30) : float3(0, 0, 1);
    const float3 C = A > 0 ? gC[0] / A : 0;
    float e = 0;
    for (uint t2 = ti * skip; t2 < triangles; t2 += T * skip)
    {
        const float3 a = At(first + t2 * 3), b = At(first + t2 * 3 + 1), d = At(first + t2 * 3 + 2);
        if (all(isfinite(a + b + d)) && all(abs(a) < 20000.0) && all(abs(b) < 20000.0) && all(abs(d) < 20000.0))
            e = max(e, max(abs(dot(N, a - C)), max(abs(dot(N, b - C)), abs(dot(N, d - C)))));
    }
    gE[ti] = e;
    GroupMemoryBarrierWithGroupSync();
    for (uint s3 = T / 2; s3 > 0; s3 >>= 1)
    {
        if (ti < s3)
            gE[ti] = max(gE[ti], gE[ti + s3]);
        GroupMemoryBarrierWithGroupSync();
    }
    if (ti == 0)
    {
        const float3 box = gHi[0] - gLo[0];
        result.Store4(group.x * 32, uint4(triangles, asuint(A > 0 ? length(gN[0]) / A : 0.0), asuint(gE[0]), asuint(all(box >= 0) ? length(box) : 0.0)));
        result.Store4(group.x * 32 + 16, uint4(asuint(N.x), asuint(N.y), asuint(N.z), asuint(dot(N, C))));
    }
}
