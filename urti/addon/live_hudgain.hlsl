// URTI en direct : rapport entre l'image finale et l'image du jeu sans son interface (live_hudless). Horizon Forbidden West code son image
// d'écran comme l'image sans interface multipliée par environ 2,2 (codes PQ ; le jeu applique ce facteur après avoir remis son image à
// la génération d'images). Appris sur les pixels immobiles hors interface : médiane de l'histogramme du rapport pixel par pixel (live_hudless,
// log2 de -1 à 3 en 256 cases : 1,1 % par case), moyenne glissante ; histogramme remis à zéro. La médiane ignore les pixels d'interface
// (menu de pause) et ceux qui ont bougé, tant qu'ils sont minoritaires.

RWByteAddressBuffer gain : register(u0); // 0 : rapport (float) ; 16 + 4 × case (256 cases) : pixels par rapport

cbuffer Constants : register(b0)
{
    uint minCount; // pixels au moins pour une mesure
    float blend;   // part de la nouvelle mesure
    uint pad0;
    uint pad1;
};

groupshared uint counts[256];

// 256 fils : chacun lit et remet à zéro sa case ; le premier fait la médiane dans la mémoire partagée (un seul fil en mémoire vidéo
// attendait chaque lecture l'une après l'autre)
[numthreads(256, 1, 1)]
void main(uint k : SV_GroupIndex)
{
    counts[k] = gain.Load(16 + k * 4);
    gain.Store(16 + k * 4, 0);
    GroupMemoryBarrierWithGroupSync();
    if (k != 0)
        return;
    // part des pixels pris pour de l'interface (octet 4, pour le journal)
    const uint2 uiCount = gain.Load2(16 + 256 * 4);
    gain.Store(4, asuint(uiCount.x ? float(uiCount.y) / float(uiCount.x) : 0.0));
    gain.Store2(16 + 256 * 4, uint2(0, 0));
    uint n = 0;
    for (uint i = 0; i < 256; i++)
        n += counts[i];
    const float was = asfloat(gain.Load(0));
    if (n < minCount)
        return;
    // médiane, interpolée dans sa case
    uint below = 0;
    float now = 0;
    for (uint b = 0; b < 256; b++)
    {
        const uint c = counts[b];
        if (c > 0 && (below + c) * 2 >= n)
        {
            const float within = saturate((float(n) * 0.5 - float(below)) / float(c));
            now = exp2((float(b) + within) / 64.0 - 1.0);
            break;
        }
        below += c;
    }
    if (now > 0.5 && now < 8.0)
        gain.Store(0, asuint(was > 0.25 && was < 8.0 ? lerp(was, now, blend) : now));
}
