// URTI en direct : sommets captés (espace de découpage) vers l'espace caméra pour la structure
// d'accélération. Chaque dessin a sa zone du flux et son compteur (pas de barrière entre dessins) ; un
// sommet n'est valide que s'il tombe dans la partie remplie de sa zone, sinon il est à l'origine : triangle
// plat, jamais touché, mais actif (une mise à jour de la structure ne peut pas changer l'état actif / inactif
// d'un triangle, et les places fixes en changent le contenu d'une image à l'autre).

StructuredBuffer<float4> clip : register(t0);   // flux de l'écran (début de son emplacement)
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin (places fixes : par place, gardés)
StructuredBuffer<uint4> regions : register(t2); // par dessin, dans l'ordre : début (octets), taille, compteur
RWByteAddressBuffer view : register(u0);        // float3 par sommet

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint vertexCount; // sommets à convertir (zones de l'image)
    uint regionCount;
};

// zone qui contient l'octet (zones triées par début)
uint4 RegionOf(uint byte)
{
    uint lo = 0, hi = regionCount;
    while (lo + 1 < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (regions[mid].x <= byte)
            lo = mid;
        else
            hi = mid;
    }
    return regions[lo];
}

[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= vertexCount)
        return;
    float3 v = 0; // triangle plat
    if (regionCount > 0)
    {
        const uint byte = id * 16;
        const uint4 r = RegionOf(byte);
        if (byte >= r.x && byte - r.x < min(counters.Load(r.z * 8), r.y))
        {
            const float4 c = clip[id];
            v = float3(c.x / p00, c.y / p11, c.w);
        }
    }
    view.Store3(id * 12, asuint(v));
}
