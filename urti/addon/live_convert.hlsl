// URTI en direct : sommets captés (espace de découpage) vers l'espace caméra pour la structure
// d'accélération. Chaque dessin a sa zone du flux et son compteur (pas de barrière entre dessins), écrite dans la
// partie du flux de la fenêtre où sa liste a été écrite ; sa place dans la scène RT (même place d'une image à
// l'autre) est donnée à l'exécution de sa liste. Un sommet n'est valide que s'il tombe dans la partie remplie
// de sa zone ; sinon il est posé sur le premier sommet de la zone : triangle plat, jamais touché, mais actif
// (une mise à jour de la structure ne peut pas changer l'état actif / inactif d'un triangle, et les places
// fixes en changent le contenu d'une image à l'autre). À l'origine (la caméra), une mise à jour qui le rendait
// réel étirait des boîtes de la caméra à l'objet, à travers toute la vue : rayons dix fois plus lents (v59).
// Place gardée sans être dessinée dans cette image : laissée telle quelle ; hors de toute place : triangle plat
// à l'origine (place rendue, ses anciens sommets ne doivent pas rester dans la scène).

StructuredBuffer<float4> clip : register(t0);   // flux de l'écran (début de son emplacement)
ByteAddressBuffer counters : register(t1);      // octets écrits par dessin
StructuredBuffer<uint4> regions : register(t2); // par place, triée : début dans la scène (octets), taille, compteur (~0 : gardée), identité
StructuredBuffer<uint> source : register(t3);   // par place : début de sa zone dans le flux (octets) ; bit 31 : dessinée à
                                                // la même place à l'image précédente (ses anciens sommets sont encore là)
RWByteAddressBuffer view : register(u0);        // float3 par sommet
RWByteAddressBuffer pairs : register(u1);       // paires de sommets (image précédente, celle-ci) pour live_camfit

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint vertexCount; // sommets à convertir (zones de l'image)
    uint regionCount;
    uint maxPairs;
    uint pad0;
    uint pad1;
    uint pad2;
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
        uint lo = 0, hi = regionCount;
        while (lo + 1 < hi)
        {
            const uint mid = (lo + hi) >> 1;
            if (regions[mid].x <= byte)
                lo = mid;
            else
                hi = mid;
        }
        const uint4 r = regions[lo];
        const bool inside = byte >= r.x && byte - r.x < r.y;
        if (inside && r.z == 0xFFFFFFFF)
            return; // gardée : sa géométrie de la dernière image où elle a été dessinée
        if (!inside)
        {
            view.Store3(id * 12, asuint(v));
            return;
        }
        const uint src = source[lo] & 0x7FFFFFFF;
        const uint filled = min(counters.Load(r.z * 8), r.y);
        const bool written = byte - r.x < filled;
        const float4 c = written ? clip[(src + byte - r.x) / 16] : clip[src / 16];
        const float3 cv = float3(c.x / p00, c.y / p11, c.w);
        // sommet aberrant (non fini, ou à plus de 20 km) : replié sur le premier sommet de sa zone, sinon à
        // l'origine. Un seul suffit à étirer la boîte de toute la scène : les boîtes intérieures, rangées avec
        // une précision relative à elle, ne valent plus rien et plus aucun rayon ne touche (v66-v67, places
        // fixes et reconstruction à chaque image)
        const float4 c0 = clip[src / 16];
        const float3 v0 = float3(c0.x / p00, c0.y / p11, c0.w);
        if (all(isfinite(cv)) && all(abs(cv) < 20000.0))
        {
            v = cv;
            // mouvement de la caméra : une quinzaine de sommets par objet resté à sa place (même poids pour chaque
            // objet : un personnage qui suit la caméra ne l'emporte pas sur le décor), avec leur position précédente
            const uint rel = (byte - r.x) / 16, stride = max(1u, filled / 16 / 16);
            if (maxPairs > 0 && (source[lo] & 0x80000000) && written && rel % stride == 0 && rel / stride < 16)
            {
                const float3 old = asfloat(view.Load3(id * 12));
                if (all(isfinite(old)) && any(old != 0) && all(abs(old) < 20000.0))
                {
                    uint k;
                    pairs.InterlockedAdd(0, 1, k);
                    if (k < maxPairs)
                        pairs.Store3(16 + k * 24, asuint(old)), pairs.Store3(16 + k * 24 + 12, asuint(cv));
                }
            }
        }
        else if (all(isfinite(v0)) && all(abs(v0) < 20000.0))
            v = v0;
    }
    view.Store3(id * 12, asuint(v));
}
