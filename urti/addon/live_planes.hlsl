// URTI en direct : surfaces transparentes planes (eau, vitres) parmi les dessins mélangés à l'image du jeu. Ces
// dessins (profondeur testée, pas écrite, mélange) sont captés à part et convertis dans leur propre tampon de sommets
// (couche 2 de la scène RT) ; un groupe par dessin. Une surface d'eau ou une vitre est plane : normales de ses
// triangles alignées, sommets à quelques centimètres du plan moyen, d'une certaine étendue, et pas tournée vers la
// caméra comme une particule (fumée, étincelles : des carrés face à la caméra, alignés eux aussi, mais étalés en
// profondeur). Le reste (fumée, particules, feuillage translucide, décalques) est rendu invisible aux rayons
// (sommets NaN : triangles inactifs). Le reflet de l'eau part alors de la surface trouvée par le rayon de la caméra
// (live_reflect).

StructuredBuffer<uint4> regions : register(t0); // par dessin : début dans la couche (octets), taille, compteur, 0
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin
RWByteAddressBuffer view : register(u0);        // float3 par sommet (couche des surfaces transparentes)
RWByteAddressBuffer stats : register(u1);       // dessins gardés, triangles gardés, dessins reçus

cbuffer Constants : register(b0)
{
    uint regionCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

#define T 64
groupshared float3 gN[T], gC[T], gLo[T], gHi[T];
groupshared float gArea[T], gE[T];

float3 Vertex(uint i) { return asfloat(view.Load3(i * 12)); }

[numthreads(T, 1, 1)]
void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex)
{
    if (group.x >= regionCount)
        return;
    const uint4 r = regions[group.x];
    const uint first = r.x / 16, count = min(counters.Load(r.z * 8), r.y) / 16 / 3 * 3, total = r.y / 16;
    // normales des triangles tournées vers la caméra (à l'origine), pondérées par leur aire ; centre ; boîte
    float3 n = 0, c = 0, lo = 1e30, hi = -1e30;
    float area = 0;
    for (uint t = ti; t < count / 3; t += T)
    {
        const float3 a = Vertex(first + t * 3), b = Vertex(first + t * 3 + 1), d = Vertex(first + t * 3 + 2);
        if (!all(isfinite(a + b + d)))
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
    const float coherence = A > 0 ? length(gN[0]) / A : 0;
    const float size = length(gHi[0] - gLo[0]);
    // écart des sommets au plan moyen
    float e = 0;
    for (uint v = ti; v < count; v += T)
    {
        const float3 p = Vertex(first + v);
        if (all(isfinite(p)))
            e = max(e, abs(dot(N, p - C)));
    }
    gE[ti] = e;
    GroupMemoryBarrierWithGroupSync();
    for (uint s3 = T / 2; s3 > 0; s3 >>= 1)
    {
        if (ti < s3)
            gE[ti] = max(gE[ti], gE[ti + s3]);
        GroupMemoryBarrierWithGroupSync();
    }
    const bool facingCamera = abs(dot(N, normalize(C))) > 0.97;
    const bool flat = count >= 6 && A > 0 && coherence > 0.95 && gE[0] < 0.02 * size + 0.05 && size > 1.0 && !facingCamera;
    if (ti == 0)
    {
        uint dummy;
        stats.InterlockedAdd(8, 1, dummy);
        if (flat)
            stats.InterlockedAdd(0, 1, dummy), stats.InterlockedAdd(4, count / 3, dummy);
    }
    if (!flat)
    {
        // invisible aux rayons : sommets NaN (triangles inactifs), la place entière du dessin
        const float nan = asfloat(0x7FC00000);
        for (uint k = ti; k < total; k += T)
            view.Store3((first + k) * 12, asuint(float3(nan, nan, nan)));
    }
    else
    {
        // surface gardée : sommets ramenés sur son plan moyen. L'eau d'un jeu est un maillage à grands triangles
        // légèrement ondulé : avec la normale de chaque facette, le reflet se cassait en pavés de la taille des
        // triangles ; à plat, il est continu (les vagues restent dans la couleur que le jeu donne à son eau)
        for (uint k = ti; k < count; k += T)
        {
            const float3 p = Vertex(first + k);
            if (all(isfinite(p)))
                view.Store3((first + k) * 12, asuint(p - N * dot(N, p - C)));
        }
    }
}
