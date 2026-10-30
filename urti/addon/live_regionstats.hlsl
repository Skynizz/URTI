// URTI en direct : mesure de chaque objet capté (un groupe par dessin) pour le budget de géométrie :
// distance au plus près de la caméra et taille (diagonale de sa boîte, espace caméra). Le rapport des deux
// (taille apparente) dit son importance : un gros objet proche compte, un petit objet lointain non.
// Lu par le processeur trois images plus tard ; les dessins hors budget sont re-mesurés dans la zone de
// mesure (fin de l'emplacement, hors de la structure d'accélération).

StructuredBuffer<float4> clip : register(t0);   // flux de l'écran (début de son emplacement)
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin
StructuredBuffer<uint4> regions : register(t2); // par dessin : début (octets), taille, compteur, identité
RWByteAddressBuffer stats : register(u0);       // float4 par dessin : distance minimale, taille, sommets, 0

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
    // très gros dessins : un sommet sur s suffit pour la boîte (au plus ~16 000 lus par dessin)
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
