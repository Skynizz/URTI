// URTI en direct : loi entre l'image finale et l'image du jeu sans son interface (live_hudless). Horizon Forbidden West retouche son image
// après l'avoir remise à la génération d'images : en codes PQ, par canal, log2(final / sans interface) = L + b × (log2(sans interface) + 1,75)
// (mesuré : rapport de 1,3 à 2,2 selon la scène, pente b d'environ 0,15). Apprise sur un pixel sur 64 (live_hudless) : médiane de
// l'histogramme du rapport des pixels sombres, et des pixels clairs, chacune à son code moyen : deux points, une droite ; moyenne
// glissante ; histogrammes remis à zéro. La médiane ignore les pixels d'interface et ceux qui ont bougé, tant qu'ils sont minoritaires.
// Aussi : laquelle des trois dernières copies concorde avec l'image finale de cette présentation, et à quel point.

RWByteAddressBuffer gain : register(u0); // 0 : rapport au pivot exp2(L) (float) ; 4 : part d'interface (journal) ; 8 : part des pixels qui concordent ;
                                         // 12 : pente b ; 16 + 4 × case : histogrammes (128 cases sombres, 128 claires) ; puis les comptes

static const uint kCounts = 16 + 256 * 4;
static const uint kLag = kCounts + 32;
static const uint kShown = kCounts + 36;   // numéro de la copie que montrait la dernière présentation qui concordait, et numéro de cette présentation
static const uint kLead = kCounts + 44;    // avance du dernier calcul sur l'image de cette présentation, en images du jeu (float ; live_composite)

cbuffer Constants : register(b0)
{
    uint minCount; // pixels au moins pour une mesure
    float blend;   // part de la nouvelle mesure
    uint serialNew;   // numéro de la dernière copie
    uint presentIdx;  // numéro de cette présentation
    uint layerSerial; // numéro de la dernière copie au moment du dernier calcul (le calque est de cette image du jeu)
    float leadBias;   // correction (images du jeu)
    uint pad0;
    uint pad1;
};

groupshared uint counts[256];

// médiane (log2 du rapport) des cases first à first + 127, interpolée dans sa case
float Median(uint first, uint n)
{
    uint below = 0;
    for (uint b = 0; b < 128; b++)
    {
        const uint c = counts[first + b];
        if (c > 0 && (below + c) * 2 >= n)
            return (float(b) + saturate((float(n) * 0.5 - float(below)) / float(c))) / 64.0 - 0.5;
        below += c;
    }
    return 0;
}

// 256 fils : chacun lit et remet à zéro sa case ; le premier fait les médianes dans la mémoire partagée (un seul fil en mémoire vidéo
// attendait chaque lecture l'une après l'autre)
[numthreads(256, 1, 1)]
void main(uint k : SV_GroupIndex)
{
    counts[k] = gain.Load(16 + k * 4);
    gain.Store(16 + k * 4, 0);
    GroupMemoryBarrierWithGroupSync();
    if (k != 0)
        return;
    const uint4 c0 = gain.Load4(kCounts), c1 = gain.Load4(kCounts + 16);
    gain.Store4(kCounts, uint4(0, 0, 0, 0));
    gain.Store4(kCounts + 16, uint4(0, 0, 0, 0));
    // part des pixels pris pour de l'interface (journal)
    gain.Store(4, asuint(c0.x ? float(c0.y) / float(c0.x) : 0.0));
    // copie qui concorde le mieux avec l'image finale de cette présentation
    const uint m0 = c0.w, m1 = c1.x, m2 = c1.y;
    const uint best = max(m0, max(m1, m2));
    const float share = c0.z >= 1000 ? float(best) / float(c0.z) : 0.0;
    gain.Store(8, asuint(share));
    // scène immobile : toutes les copies concordent ; la copie d'avant reste (le retard ne se mesure qu'en mouvement)
    const uint lagWas = min(gain.Load(kLag), 2u);
    const uint mWas = lagWas == 0 ? m0 : lagWas == 1 ? m1 : m2;
    const uint lag = mWas + c0.z / 200 >= best ? lagWas : best == m0 ? 0u : best == m1 ? 1u : 2u;
    gain.Store(kLag, lag);
    if (share >= 0.96)
        gain.Store2(kShown, uint2(serialNew - lag, presentIdx));
    // avance du calque sur l'image montrée ; présentation qui ne concorde pas (image fabriquée entre deux) : une demi-image du jeu de plus
    // que la précédente
    const uint2 shown = gain.Load2(kShown);
    const uint age = presentIdx - shown.y;
    gain.Store(kLead, asuint(age <= 6 ? float(int(layerSerial - shown.x)) - 0.5 * float(age) + leadBias : 0.0));
    uint n0 = 0, n1 = 0;
    for (uint i = 0; i < 128; i++)
        n0 += counts[i], n1 += counts[128 + i];
    if (n0 + n1 < minCount)
        return;
    // deux points (code moyen, rapport médian) en log2 ; un seul groupe garni : la pente d'avant
    const float wasG = asfloat(gain.Load(0)), wasB = asfloat(gain.Load(12));
    const bool known = wasG > 0.25 && wasG < 8.0;
    const float bOld = known && abs(wasB) < 0.6 ? wasB : 0.0;
    const float x0 = n0 ? float(c1.z) / float(n0) / 256.0 - 8.0 : 0.0, x1 = n1 ? float(c1.w) / float(n1) / 256.0 - 8.0 : 0.0;
    float L, b;
    if (n0 >= 300 && n1 >= 300 && x1 - x0 > 0.3)
    {
        const float r0 = Median(0, n0), r1 = Median(128, n1);
        b = clamp((r1 - r0) / (x1 - x0), -0.5, 0.5);
        L = r0 + b * (-1.75 - x0);
    }
    else
    {
        const bool low = n0 >= n1;
        b = bOld;
        L = (low ? Median(0, n0) : Median(128, n1)) + b * (-1.75 - (low ? x0 : x1));
    }
    if (!(L > -1.5 && L < 2.5))
        return;
    if (known)
        L = lerp(log2(wasG), L, blend), b = lerp(bOld, b, blend);
    gain.Store(0, asuint(exp2(L)));
    gain.Store(12, asuint(b));
}
