// URTI en direct : cache de géométrie. Les objets captés à leur tour (zone de mesure du flux) sont convertis dans le
// repère fixe du cache (espace caméra de la dernière remise à zéro) : sommet = R^T (espace caméra - t), avec la pose de
// la caméra accumulée par live_camfit. Chaque objet a ensuite sa propre structure d'accélération, gardée d'une image à
// l'autre et posée dans la scène RT par cette même pose. Un groupe par objet. Sommets non écrits ou aberrants : NaN
// (triangle inactif).

StructuredBuffer<float4> clip : register(t0);   // flux de l'écran (début de son emplacement)
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin
StructuredBuffer<uint4> regions : register(t2); // par objet : début dans le flux (octets), taille (octets), compteur,
                                                // début dans view (octets)
RWByteAddressBuffer view : register(u0);        // float3 par sommet, repère du cache
RWByteAddressBuffer motion : register(u1);      // octets 128 à 175 : pose (R par lignes, t) du repère du cache vers la caméra

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
    const float2 shear = asfloat(motion.Load2(176)); // décalage sous-pixel du jeu dans cette image (caméra du jeu lue)
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
