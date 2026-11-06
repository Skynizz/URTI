// URTI en direct : pose de la caméra recalée sur des points de référence. Chaque témoin (petit objet immobile, loin de la
// caméra) garde seize de ses sommets dans le repère fixe du cache de géométrie, relevés la première fois qu'il est vu. À
// chaque image, ces points posés par la dernière pose connue et leur position réelle dans cette image donnent les paires
// de live_camfit : la correction de pose. Rien ne s'accumule : une image sans mesure (fenêtre de capture sans témoin,
// image du jeu coupée entre deux présentations) ne fait perdre aucun déplacement, et le décalage sous-pixel du jeu (TAA)
// ne s'ajoute pas d'une image à l'autre.

StructuredBuffer<float4> clip : register(t0);   // flux de l'écran (début de son emplacement)
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin
StructuredBuffer<uint4> table : register(t2);   // par témoin : début dans le flux (octets), taille, compteur, place des
                                                // points (bit 31 : nouveau témoin)
RWByteAddressBuffer refs : register(u0);        // par témoin, 208 octets : 16 × float3 (repère du cache), nombre de sommets
RWByteAddressBuffer pairs : register(u1);       // nombre de paires, puis par paire : p.xyz, q.xyz
RWByteAddressBuffer motion : register(u2);      // octets 128 à 175 : dernière pose (R par lignes, t)

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint count;
    uint maxPairs;
    uint identityPose; // 1 : le repère repart de cette image (la pose lue ne vaut plus)
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
    // nouveau témoin, ou autre nombre de sommets (autre niveau de détail du même maillage) : points relevés
    if (ti == 0)
        gFresh = ((r.w >> 31) != 0 || refs.Load(base + 192) != n) ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    const float4 c = clip[r.x / 16 + ti * (n / S)];
    const float2 shear = asfloat(motion.Load2(176)); // décalage sous-pixel du jeu (caméra du jeu lue ; 0 sinon)
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
