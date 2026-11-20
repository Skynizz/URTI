// URTI en direct : ombres du soleil en ray tracing (requêtes en ligne). Un fil par pixel de la
// profondeur du jeu : point reconstruit (profondeur = A + B / w), rayon vers le soleil.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer depth : register(t1);   // copie de la profondeur (32 bits par texel, lignes alignées)
ByteAddressBuffer sunBuf : register(t2);  // soleil de cette image (live_sunvote), espace caméra, cascades
ByteAddressBuffer atlas : register(t3);   // copie de la carte d'ombres du jeu (32 bits par texel, lignes alignées)
ByteAddressBuffer prevShadow : register(t4); // masque de l'image précédente (accumulation)
ByteAddressBuffer depthPrev : register(t5);  // profondeur de l'image précédente
ByteAddressBuffer motion : register(t6);     // vecteurs de mouvement du jeu (R16G16 flottants)
RWByteAddressBuffer shadow : register(u0); // 1 éclairé, 0 à l'ombre (float, lignes alignées)

cbuffer Constants : register(b0)
{
    float sunSign;     // +1 : la profondeur de la carte croît en s'éloignant du soleil
    float gameBias;    // tolérance du test de profondeur de la carte du jeu
    float sunRadius;   // rayon apparent du soleil (radians) : pénombre
    float bias;
    uint width;
    uint height;
    uint depthPitch;   // octets par ligne
    uint shadowPitch;
    float A;
    float B;
    float p00;
    float p11;
    float clearDepth;
    uint depth24;      // 1 : D24S8 (24 bits de profondeur)
    uint atlasPitch;
    uint flags;        // 1 carte du jeu lue, 2 carte en D24, 4 combiner avec l'ombre du jeu, 8 afficher l'ombre du jeu, 16 pas de soleil,
                       // 32 carte en 16 bits (R16, RDR2), 64 pas de rayons d'ombre du soleil (le soleil reste connu :
                       // l'occultation ambiante s'en sert), 128 occultation au quart (un pixel sur quatre par image,
                       // les autres écrits -1 : live_shadowtemporal les reconstruit), bits 16 à 18 : rayons d'occultation
                       // par pixel calculé à l'arrêt (0 : règle fixe ; l'add-on les règle sur le temps de la passe),
                       // 2048 : les trous des plaques découpées (vus à travers à l'écran) laissent passer le soleil ;
                       // 4096 : sortie pour SIGMA (débruiteur d'ombres de NVIDIA) : pénombre dans penumbra, poids des
                       // corrections (ombre du jeu, N·L, brume) dans shadow ; le débruitage se fait après ;
                       // 1 << 20 : rebond de lumière (bounce)
    uint motionPitch;
    uint motionConvention; // 0..15 (live_mvcal), 255 : pas d'accumulation
    float temporalAlpha;   // part de l'image courante (0,3 : environ trois images accumulées)
    uint debugMode;        // 1 ombre brute (sans accumulation), 2 vecteurs de mouvement, 3 historique gardé
    uint frameIndex;       // fait tourner le motif des rayons d'une image à l'autre (l'accumulation le moyenne)
    float sunSky;          // rapport soleil / ciel mesuré dans l'image du jeu (live_sunlight) ; 0 : non mesuré
    uint gameDilate;       // voisinage lu dans la carte du jeu (rayon en texels)
    uint gameMin;          // 1 : part éclairée filtrée, visibilité la plus faible des deux ; 0 : ombre du jeu élargie, produit
    float aoAmount;        // occultation ambiante RT : part de la lumière du ciel retirée par un obstacle tout proche (0 : coupée)
    float aoRadius;        // portée des rayons d'occultation (mètres)
    uint aoMotion;         // convention des vecteurs de mouvement (live_mvcal) pour l'occultation ; 255 : inconnue
    uint cacheTriangles;   // triangles tenus par le cache de surfaces (rebond de lumière)
    uint regionCount;      // zones de la scène RT (regions)
    float aoMaxDist;       // budget de temps : occultation calculée jusqu'à cette distance (mètres)
    uint gnMode;           // normales du G-buffer : bits 0-1 encodage (1 octaèdre RG16 en repère caméra, 2 RGB8 * 2 - 1), bits 8-10 signes (x, y, z inversés),
                           // bit 12 : l'occultation les prend, bit 13 : comparaison à la normale de la profondeur (journal)
    uint gnPitch;          // octets par ligne de la copie
    float skyAmount;       // visibilité du ciel à grande portée : part de la lumière du ciel retirée quand un obstacle la cache (0 : coupée)
    float skyRange;        // portée de ces rayons (mètres)
};

uint AoHash(uint x)
{
    x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
    return x;
}

static float gA, gB, gClear;

// SIGMA : rayon de la pénombre au point (distance à l'obstacle × tangente du rayon apparent du soleil) ;
// NRD_FP16_MAX : rien ne bloque (même formule que SIGMA_FrontEnd_PackPenumbra de NRD)
static const float kFp16Max = 65504.0;
float PackPenumbra(float distanceToOccluder, float tanOfLightAngularRadius)
{
    return distanceToOccluder >= kFp16Max ? kFp16Max : min(distanceToOccluder * tanOfLightAngularRadius * 0.5, 32768.0);
}
RWByteAddressBuffer penumbra : register(u1); // float par pixel (pas : shadowPitch), mode SIGMA
ByteAddressBuffer gnormal : register(t9);     // copie de la cible des normales du G-buffer (gnMode)
RWByteAddressBuffer nstats : register(u3);    // comparaison : par combinaison de signes, somme des produits scalaires (×1000) et nombre de pixels

// normale du G-buffer en espace caméra d'URTI (x droite, y haut, z vers la scène), selon l'encodage et les signes ; 0 si absente
float3 GNormal(int2 t, uint signs)
{
    const uint enc = gnMode & 3;
    if (enc == 0 || gnPitch == 0)
        return 0;
    const uint v = gnormal.Load(t.y * gnPitch + t.x * 4);
    float3 n;
    if (enc == 1)
    {
        const float2 e = float2(v & 0xFFFF, v >> 16) / 65535.0 * 2.0 - 1.0;
        n = float3(e, 1.0 - abs(e.x) - abs(e.y));
        if (n.z < 0)
            n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0 ? 1.0 : -1.0, n.y >= 0 ? 1.0 : -1.0);
    }
    else
        n = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0 * 2.0 - 1.0;
    if (signs & 1)
        n.x = -n.x;
    if (signs & 2)
        n.y = -n.y;
    if (signs & 4)
        n.z = -n.z;
    const float l = length(n);
    return l > 1e-3 && all(isfinite(n)) ? n / l : 0;
}

// normale du G-buffer dans l'espace caméra d'URTI, prête pour les rayons : celle de Red Dead (repère du monde) tournée par l'orientation de la caméra
// que live_rotfit a résolue (image précédente : sunBuf 2048 à 2095, valide en 2092) ; 0 si absente ou pas encore résolue
float3 GNormalCam(int2 t)
{
    float3 n = GNormal(int2(t), (gnMode >> 8) & 7);
    if ((gnMode & 3) == 2)
    {
        if (sunBuf.Load(2092) == 0)
            return 0;
        n = float3(dot(asfloat(sunBuf.Load3(2048)), n), dot(asfloat(sunBuf.Load3(2060)), n), dot(asfloat(sunBuf.Load3(2072)), n));
        const float l = length(n);
        n = l > 1e-3 && all(isfinite(n)) ? n / l : 0;
    }
    return n;
}

// rebond de lumière (éclairage indirect, bit 20 de flags) : les rayons d'occultation qui touchent un triangle y
// prennent la couleur que le cache de surfaces a retenue (live_surfcache) : la lumière que cette surface renvoie.
// Sortie : trois plans R, G, B (float, pas shadowPitch, l'un après l'autre), -1 aux pixels non calculés (au quart)
ByteAddressBuffer surfCache : register(t7);     // par triangle : couleur (RGB9E5), identité de l'objet et mesures
StructuredBuffer<uint4> regions : register(t8); // zones de la scène RT : début (octets), taille, compteur, identité
RWByteAddressBuffer bounce : register(u2);

uint RegionIndex(uint prim)
{
    const uint byte = prim * 48;
    uint lo = 0, hi = regionCount;
    while (lo + 1 < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (regions[mid].x <= byte)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

// couleur retenue pour ce triangle, si elle est à son objet actuel.
// Une entrée pour huit triangles consécutifs (voisins dans le maillage) : dans RDR2, la plupart des triangles
// sont plus petits qu'un pixel et une entrée par triangle n'était presque jamais remplie (un rayon d'occultation
// sur quatre trouvait sa couleur). Entrée vide ou d'un autre objet : les entrées voisines du même objet
bool Cached(uint prim, out float3 c)
{
    c = 0;
    if (prim >= cacheTriangles || regionCount == 0)
        return false;
    const uint ident = regions[RegionIndex(prim)].w & 0xFFFFFF;
    const int g = int(prim >> 3), last = int((cacheTriangles - 1) >> 3);
    static const int kNear[5] = { 0, -1, 1, -2, 2 };
    for (uint k = 0; k < 5; k++)
    {
        const int e = g + kNear[k];
        if (e < 0 || e > last)
            continue;
        const uint2 v = surfCache.Load2(uint(e) * 8);
        if ((v.y & 255) != 0 && (v.y >> 8) == ident)
        {
            c = float3(v.x & 511, (v.x >> 9) & 511, (v.x >> 18) & 511) * exp2(float(int(v.x >> 27) - 24));
            return all(isfinite(c));
        }
    }
    return false;
}

// profondeur d'un texel de la carte d'ombres du jeu : 32 bits flottants, D24 (bit 2 de flags) ou 16 bits
// normalisés (bit 32 : RDR2, deux octets par texel)
float AtlasDepth(uint2 t)
{
    if (flags & 32)
    {
        const uint a = t.y * atlasPitch + t.x * 2;
        const uint raw = atlas.Load(a & ~3u);
        return float((a & 2) ? raw >> 16 : raw & 0xFFFF) / 65535.0;
    }
    const uint raw = atlas.Load(t.y * atlasPitch + t.x * 4);
    return (flags & 2) ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
}

// éclairement du point par le soleil selon la carte d'ombres du jeu (1 si aucune cascade ne le couvre)
float GameLit(float3 p)
{
    const uint count = min(sunBuf.Load(32), 8u);
    for (uint k = 0; k < count; k++)
    {
        const uint base = 48 + k * 64;
        const float4 m0 = asfloat(sunBuf.Load4(base)), m1 = asfloat(sunBuf.Load4(base + 16)), m2 = asfloat(sunBuf.Load4(base + 32));
        const float4 vp = asfloat(sunBuf.Load4(base + 48));
        const float M[12] = { m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w };
        float3 s;
        for (uint c = 0; c < 3; c++)
            s[c] = M[9 + c] + p.x * M[c] + p.y * M[3 + c] + p.z * M[6 + c];
        if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z <= 0 || s.z >= 1)
            continue;
        const float2 texel = vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw;
        // gameMin : part éclairée du voisinage (comme le filtrage du jeu : sa pénombre comprise) ; sinon
        // éclairé seulement si tout le voisinage l'est (ombre du jeu élargie de gameDilate texels)
        const int r = int(gameDilate);
        float litCount = 0;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
            {
                const float stored = AtlasDepth(uint2(int2(texel) + int2(dx, dy)));
                const bool texelLit = sunSign > 0 ? s.z <= stored + gameBias : s.z >= stored - gameBias;
                if (!texelLit && gameMin == 0)
                    return 0.0;
                litCount += texelLit ? 1.0 : 0.0;
            }
        return litCount / float((2 * r + 1) * (2 * r + 1));
    }
    return 1.0;
}

// le soleil voit-il à travers ce point h, sur le rayon qui part du récepteur ? La carte d'ombres du jeu
// est la vue du soleil, alpha test compris. Le jeu y note chaque obstacle un peu plus loin du soleil
// qu'il n'est (son décalage contre l'acné) : h n'est un trou que si le premier obstacle noté à son texel
// est au moins à mi-chemin entre h et le récepteur (feuillage découpé, le soleil passe jusqu'au sol).
// Trop près du récepteur pour trancher, ou hors des cascades : on ne sait pas.
bool SunSeesThrough(float3 h, float3 receiver)
{
    if ((flags & 1) == 0)
        return false;
    const uint count = min(sunBuf.Load(32), 8u);
    for (uint k = 0; k < count; k++)
    {
        const uint base = 48 + k * 64;
        const float4 m0 = asfloat(sunBuf.Load4(base)), m1 = asfloat(sunBuf.Load4(base + 16)), m2 = asfloat(sunBuf.Load4(base + 32));
        const float4 vp = asfloat(sunBuf.Load4(base + 48));
        const float M[12] = { m0.x, m0.y, m0.z, m0.w, m1.x, m1.y, m1.z, m1.w, m2.x, m2.y, m2.z, m2.w };
        float3 s, r;
        for (uint c = 0; c < 3; c++)
        {
            s[c] = M[9 + c] + h.x * M[c] + h.y * M[3 + c] + h.z * M[6 + c];
            r[c] = M[9 + c] + receiver.x * M[c] + receiver.y * M[3 + c] + receiver.z * M[6 + c];
        }
        if (abs(s.x) > 0.98 || abs(s.y) > 0.98 || s.z <= 0 || s.z >= 1)
            continue;
        const float gap = sunSign * (r.z - s.z); // > 0 : h entre le soleil et le récepteur
        if (gap <= 2.0 * gameBias)
            return false;
        const float stored = AtlasDepth(uint2(vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw));
        return sunSign * (stored - s.z) > 0.5 * gap;
    }
    return false;
}

// point de l'espace caméra du pixel (z = w ; w <= 0 si ciel ou hors image)
float3 PointAt(int2 t)
{
    if (t.x < 0 || t.y < 0 || t.x >= int(width) || t.y >= int(height))
        return float3(0, 0, -1);
    const uint raw = depth.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float w = gB / (d - gA);
    if (d == gClear || !(w > 0 && w < 1e6))
        return float3(0, 0, -1);
    const float2 ndc = float2((t.x + 0.5) / width * 2 - 1, 1 - (t.y + 0.5) / height * 2);
    return float3(ndc.x * w / p00, ndc.y * w / p11, w);
}

// normale de la surface tirée de la profondeur : côté le plus proche de chaque axe (bords d'objets)
// un point que la caméra voit à travers (elle voit plus loin que lui, ou le ciel) n'est pas solide : c'est
// un trou d'une plaque découpée par alpha test (feuillage, herbe, grillage, franges), dont la géométrie
// captée n'a que le rectangle. 1 : trou ; 0 : plein ; -1 : hors de l'écran, la caméra ne sait pas.
int ScreenSees(float3 h)
{
    if (h.z <= 1e-3)
        return -1;
    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv <= 0) || any(uv >= 1))
        return -1;
    const float3 seen = PointAt(int2(uv * float2(width, height)));
    return (seen.z <= 0 || seen.z > h.z * 1.03 + 0.05) ? 1 : 0;
}

// trou d'une plaque découpée : la caméra le voit à travers ; hors de l'écran, le soleil (sa carte)
bool Hole(float3 h, float3 receiver)
{
    const int screen = ScreenSees(h);
    return screen >= 0 ? screen == 1 : SunSeesThrough(h, receiver);
}

// surface plane autour du pixel (comme live_reflect) : les points à 2 pixels de part et d'autre sont alignés
// avec lui sur au moins un axe ; faux pour l'herbe, le feuillage, une grille (la profondeur saute)
float Flatness(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(2, 0)), r = PointAt(t + int2(2, 0)), u = PointAt(t - int2(0, 2)), b = PointAt(t + int2(0, 2));
    if (l.z <= 0 || r.z <= 0 || u.z <= 0 || b.z <= 0)
        return 0;
    const float3 h = r - l, v = b - u;
    const float eh = length(cross(h, p - l)) / max(length(h), 1e-6);
    const float ev = length(cross(v, p - u)) / max(length(v), 1e-6);
    return saturate(1.0 - min(eh, ev) / (0.004 * p.z + 0.002));
}

// surface continue autour du pixel (tronc, rocher, personnage, mur) : à 3 pixels dans les quatre directions, la profondeur reste proche ;
// faux pour une branche, une brindille, un bord de feuillage, qui bougent au vent et où une occultation tremblait
bool DepthContinuous(int2 t, float3 p)
{
    static const int2 kSide[4] = { int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3) };
    for (uint k = 0; k < 4; k++)
    {
        const float3 q = PointAt(t + kSide[k]);
        if (q.z <= 0 || abs(q.z - p.z) > 0.04 * p.z + 0.05)
            return false;
    }
    return true;
}

// feuillage (plaques découpées, planes donc acceptées par Flatness) : autour du pixel, à 6 pixels, la caméra
// voit nettement plus loin (le fond à travers les trous des feuilles)
bool SeesThroughAround(int2 t, float3 p)
{
    static const int2 kAround[8] = { int2(6, 0), int2(-6, 0), int2(0, 6), int2(0, -6), int2(4, 4), int2(-4, 4), int2(4, -4), int2(-4, -4) };
    for (uint k = 0; k < 8; k++)
    {
        const float3 q = PointAt(t + kAround[k]);
        if (q.z <= 0 || q.z > p.z * 1.3 + 0.5)
            return true;
    }
    return false;
}

// normale d'ensemble d'une surface rugueuse (herbe, fougères sur le sol) : pente du terrain prise à 8 pixels de part
// et d'autre ; 0 si un voisin manque ou saute trop (bord d'objet)
float3 MacroNormal(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(8, 0)), r = PointAt(t + int2(8, 0)), u = PointAt(t - int2(0, 8)), b = PointAt(t + int2(0, 8));
    if (l.z <= 0 || r.z <= 0 || u.z <= 0 || b.z <= 0)
        return 0;
    if (max(max(abs(l.z - p.z), abs(r.z - p.z)), max(abs(u.z - p.z), abs(b.z - p.z))) > 0.3 * p.z + 0.5)
        return 0;
    const float3 n = normalize(cross(r - l, b - u));
    return dot(n, p) > 0 ? -n : n;
}

float3 NormalAt(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(1, 0)), r = PointAt(t + int2(1, 0)), u = PointAt(t - int2(0, 1)), b = PointAt(t + int2(0, 1));
    const float3 dx = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
    const float3 dy = (b.z > 0 && (u.z <= 0 || abs(b.z - p.z) < abs(u.z - p.z))) ? b - p : p - u;
    float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n; // tournée vers la caméra
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    // loi de profondeur mesurée dans cette image (live_depthfit), sinon celle des constantes
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0; // profondeur inversée : le ciel à 0
    const uint raw = depth.Load(id.y * depthPitch + id.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float3 sun = asfloat(sunBuf.Load3(0));
    float lit = 1.0;
    float hitDistance = kFp16Max; // mode SIGMA : distance de l'obstacle du premier rayon
    float weight = 0.0;           // mode SIGMA : part de l'ombre gardée par les corrections (0 : aucune ombre RT)
    const float w = gB / (d - gA);
    float gameVis = 1.0; // soleil reçu selon la carte du jeu (occultation ambiante : part du ciel dans la lumière)
    // occultation seule (ombres du soleil coupées, flags 64) : un pixel sans rayon à cette image (3 sur 4), le ciel ou ce qui est au-delà de la portée sortent ici, sans normale,
    // planéité ni carte du jeu (toute cette préparation coûtait plusieurs millisecondes, quel que soit le nombre de rayons)
    const bool aoOnly = (flags & 64) != 0 && (flags & (8u | 4096u)) == 0 && debugMode == 0;
    static const uint2 kQuarter[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
    const bool notThisFrame = (flags & 128) != 0 && any((id & 1u) != kQuarter[frameIndex & 3]);
    if (aoOnly && (d == gClear || !(w > 0) || w >= aoMaxDist || aoAmount <= 0 || notThisFrame))
    {
        const bool later = notThisFrame && d != gClear && w > 0 && w < aoMaxDist && aoAmount > 0; // calculé à une autre image : -1, reconstruit par live_shadowtemporal
        shadow.Store(id.y * shadowPitch + id.x * 4, asuint(later ? -1.0 : 1.0));
        if (flags & (1u << 20))
        {
            const uint plane = shadowPitch * height, a0 = id.y * shadowPitch + id.x * 4;
            const float v = later ? -1.0 : 0.0;
            bounce.Store(a0, asuint(v)), bounce.Store(plane + a0, asuint(v)), bounce.Store(2 * plane + a0, asuint(v));
        }
        return;
    }
    if (!aoOnly && (flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
    {
        const float2 ndc = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p = float3(ndc.x * w / p00, ndc.y * w / p11, w);
        const float dist = length(p);
        // quatre rayons répartis dans le disque solaire (strates 2×2 tournées par pixel) : pénombre lisse
        const float3 t1 = normalize(abs(sun.y) < 0.99 ? cross(sun, float3(0, 1, 0)) : cross(sun, float3(1, 0, 0)));
        const float3 t2 = cross(sun, t1);
        const float rot = 6.2831853 * frac(52.9829189 * frac(dot(float2(id), float2(0.06711056, 0.00583715))) + 0.61803399 * float(frameIndex & 1023));
        hitDistance = kFp16Max;
        lit = (flags & 64) ? 1.0 : 0.0;
        // SIGMA veut un rayon par pixel (il débruite lui-même) et la distance de l'obstacle
        const uint rays = (flags & 64) ? 0u : (flags & 4096) ? 1u : clamp((flags >> 8) & 7, 1u, 4u); // 1 à 4 rayons (réglage shadow_rays)
        for (uint k = 0; k < rays; k++)
        {
            const float u = rays == 1 ? 0.0 : (float(k & 1) + 0.5) * 0.5, v = (float(k >> 1) + 0.5) * 0.5;
            const float r = sunRadius * sqrt(u), a = 6.2831853 * v + rot;
            RayDesc ray;
            ray.Origin = p * (1.0 - bias) + sun * bias * dist;
            ray.Direction = normalize(sun + t1 * (r * cos(a)) + t2 * (r * sin(a)));
            ray.TMin = 0.0;
            ray.TMax = 1e5;
            // premier triangle qui n'est pas un trou vu à travers par la caméra ou par le soleil : ombre
            RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
            bool blocked = false;
            while (q.Proceed())
                if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                    ((flags & 2048) == 0 || !Hole(ray.Origin + ray.Direction * q.CandidateTriangleRayT(), p)))
                {
                    blocked = true;
                    if (k == 0)
                        hitDistance = q.CandidateTriangleRayT();
                    q.Abort();
                }
            lit += blocked ? 0.0 : 1.0 / rays;
        }
        // face tournée à l'opposé du soleil : le jeu l'assombrit déjà par son éclairage ; une ombre RT en
        // plus y ferait des taches (le test de sa carte la voit éclairée, la surface étant son propre écran)
        const float ndl = dot(NormalAt(int2(id), p), sun);
        const bool facesSun = ndl > 0.08;
        weight = 1.0;
        if (!facesSun && (flags & 4))
            lit = 1.0, weight = 0.0;
        else if (flags & 1)
        {
            const float game = GameLit(p);
            gameVis = facesSun ? game : 0.0;
            if (flags & 8)
                lit = game; // contrôle : l'ombre du jeu telle que recalculée
            else if (flags & 4)
            {
                // n'ajouter de l'ombre que là où le jeu éclaire : le soleil restant est sa visibilité « game » ;
                // la visibilité voulue est la plus faible des deux, URTI retire la différence
                lit = gameMin ? 1.0 - max(0.0, game - lit) : 1.0 - game * (1.0 - lit);
                weight *= game;
            }
        }
        if (ndl <= 0)
            hitDistance = 0; // SIGMA : surface dos au soleil, à l'ombre d'elle-même
    }
    // une ombre ne retire que la lumière directe du soleil, proportionnelle à l'angle de la surface face à lui
    // (N·L) ; le reste (lumière du ciel, rebonds) demeure. Le point éclairé reçoit ciel + soleil × N·L : la
    // part du soleil y est x N·L / (1 + x N·L), x = soleil / ciel mesuré dans l'image (live_sunlight) ;
    // midi : ombres franches, temps couvert : presque rien. Non mesuré : pente fixe (ancienne règle).
    if (!aoOnly && (flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
    {
        const float2 ndc2 = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p2 = float3(ndc2.x * w / p00, ndc2.y * w / p11, w);
        const float ndl2 = saturate(dot(NormalAt(int2(id), p2), sun));
        const float ndlWeight = sunSky > 0 ? sunSky * ndl2 / (1.0 + sunSky * ndl2) : saturate(ndl2 * 2.5);
        lit = 1.0 - (1.0 - lit) * ndlWeight;
        weight *= ndlWeight;
    }
    // occultation ambiante RT : un rayon court par pixel et par image (hémisphère pondéré par le cosinus, motif
    // qui tourne d'une image à l'autre, moyenné par le débruitage), dans la vraie géométrie de la scène (hors
    // de l'écran comprise) ; obstacle à la distance t : occultation 1 - t / portée. Elle ne retire que la
    // lumière du ciel : toute celle d'un point à l'ombre, 1 / (1 + x N·L) de celle d'un point au soleil.
    // Seulement sur les surfaces planes (sol, murs, routes) : l'herbe et le feuillage, où chaque brin est de la
    // géométrie, s'assombrissaient en entier (le jeu y a déjà son occultation) et tremblaient au vent
    const float2 ndc3 = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
    const float3 p3 = float3(ndc3.x * w / p00, ndc3.y * w / p11, w);
    bool aoSkipped = false; // occultation au quart : ce pixel n'est pas calculé à cette image
    float3 bounceSum = 0;   // rebond de lumière : couleur des surfaces touchées, pondérée comme l'occultation
    // surface rugueuse (bit 21) : herbe, fougères qui couvrent le sol ; l'occultation y part de la pente d'ensemble,
    // au-dessus des brins (sinon le pied des troncs et des rochers posés dans l'herbe n'en recevait aucune)
    const bool flatHere = Flatness(int2(id), p3) > 0.5;
    // (à moins de 20 m : au-delà, c'est du feuillage lointain, derrière la brume du jeu ; l'occultation y faisait un grain sombre
    // par-dessus le brouillard)
    const float3 macroN = (!flatHere && (flags & (1u << 21)) && w < 20.0) ? MacroNormal(int2(id), p3) : float3(0, 0, 0);
    const bool rough = any(macroN != 0);
    // normale exacte du jeu (gnMode bit 12) : l'occultation vaut aussi pour les surfaces qui ne sont pas planes (troncs, rochers, objets,
    // personnages), où celle de la profondeur n'était que du bruit ; elle regarde la caméra
    float3 gnHere = 0;
    if ((gnMode & 0x1000) && !rough && w > 0 && w < aoMaxDist)
    {
        gnHere = GNormalCam(int2(id));
        if (dot(gnHere, -normalize(p3)) <= 0.05 || !(flatHere || DepthContinuous(int2(id), p3)))
            gnHere = 0;
    }
    const bool gnOk = any(gnHere != 0);
    if (!aoOnly && aoAmount > 0 && (flags & 4096) == 0 && d != gClear && w > 0 && w < aoMaxDist && (flatHere || rough || gnOk) && !SeesThroughAround(int2(id), p3))
        aoSkipped = notThisFrame; // au quart : le pixel du bloc 2×2 désigné pour cette image (les quatre en quatre images)
    if (aoAmount > 0 && !aoSkipped && (flags & 4096) == 0 && d != gClear && w > 0 && w < aoMaxDist && (flatHere || rough || gnOk) &&
        !SeesThroughAround(int2(id), p3))
    {
        float3 n = rough ? macroN : NormalAt(int2(id), p3);
        // comparaison des normales du G-buffer à celle de la profondeur (surface plane, un pixel sur 64) : le bon décodage est celui qui colle
        if ((gnMode & 0x2000) && !rough && flatHere && (id.x & 7) == 0 && (id.y & 7) == 0)
            for (uint sv = 0; sv < 8; sv++)
            {
                const float3 g = GNormal(int2(id), sv);
                if (all(g == 0))
                    continue;
                uint unused;
                nstats.InterlockedAdd(sv * 8, uint(int(round(dot(g, n) * 1000.0))), unused);
                nstats.InterlockedAdd(sv * 8 + 4, 1, unused);
            }
        // orientation de la caméra (normales du monde, gnMode bit 14) : sommes de la normale du monde par celle de la caméra (live_rotfit)
        if ((gnMode & 0x4000) && !rough && flatHere && (id.x & 6) == 0 && (id.y & 6) == 0) // (au quart : un des quatre pixels du bloc à chaque image)
        {
            const float3 a = GNormal(int2(id), 0);
            if (any(a != 0))
            {
                uint unused;
                for (uint ai = 0; ai < 3; ai++)
                    for (uint bj = 0; bj < 3; bj++)
                        nstats.InterlockedAdd(64 + (ai * 3 + bj) * 4, uint(int(round(a[ai] * n[bj] * 1000.0))), unused);
                nstats.InterlockedAdd(100, 1, unused);
            }
        }
        // normale exacte du jeu (surface plane : la profondeur ne fait que la deviner, avec le grain du décalage sous-pixel)
        if (gnOk)
            n = gnHere;
        const float3 b1 = normalize(abs(n.y) < 0.99 ? cross(n, float3(0, 1, 0)) : cross(n, float3(1, 0, 0)));
        const float3 b2 = cross(n, b1);
        // deux rayons (strates en azimut) ; quatre là où l'image bouge de plus d'un pixel et demi : l'historique y
        // est rejeté ou à peine gardé, le bruit de deux rayons tremblerait. Au quart : deux fois plus (quatre fois
        // moins de pixels : moitié moins de rayons au total)
        const uint aoBase = (flags & 128) ? 2u : 1u;
        // nombre réglé par l'add-on (bits 16 à 18) : sous des arcades, presque tout l'écran reçoit de l'occultation
        // et le coût de la passe, proportionnel aux rayons, triplait (5,5 ms dans RDR2)
        const uint aoRest = (flags >> 16) & 7;
        uint aoRays = aoRest ? aoRest : 2 * aoBase;
        // far away the occlusion is smooth and accumulated over many frames: fewer rays cost nothing visible
        if (w > 25.0)
            aoRays = 1;
        else if (w > 12.0)
            aoRays = min(aoRays, 2u);
        if (aoMotion < 16)
        {
            const uint raw = motion.Load(id.y * motionPitch + id.x * 4);
            float2 mv = float2(f16tof32(raw & 0xFFFF), f16tof32(raw >> 16));
            mv = (aoMotion & 3) == 0 ? mv : (aoMotion & 3) == 1 ? mv * 0.5 : (aoMotion & 3) == 2 ? mv / float2(width, height) : mv * 2.0;
            if (all(isfinite(mv)) && length(mv * float2(width, height)) > 1.5)
                aoRays = aoRest ? aoRest + max(1u, aoRest / 2) : (flags & 128) ? 6u : 4u;
        }
        uint h = AoHash(id.x * 1973u + id.y * 9277u + frameIndex * 26699u);
        float occlusion = 0;
        for (uint k = 0; k < aoRays; k++, h = AoHash(h))
        {
            const float u1 = float(h & 0xFFFF) / 65536.0, u2 = (float(h >> 16) / 65536.0 + float(k)) / float(aoRays);
            const float r = sqrt(u1), phi = 6.2831853 * u2;
            RayDesc ray;
            ray.Origin = p3 + n * (rough ? max(0.08, 0.004 * length(p3)) : max(0.01, 0.002 * length(p3)));
            ray.Direction = normalize(b1 * (r * cos(phi)) + b2 * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
            // obstacles à moins de 6 cm ignorés : les décalques du jeu (boue, marques, flaques), posés juste au-dessus
            // du sol sans écrire la profondeur, faisaient des taches sombres à leur forme exacte
            ray.TMin = rough ? 0.25 : 0.06; // surface rugueuse : pas les brins eux-mêmes
            ray.TMax = aoRadius;
            // rayon opaque d'abord (le matériel trouve seul le plus proche) ; recherche lente, triangle par triangle, seulement si ce point
            // est un trou d'une plaque découpée (même résultat, sans examiner chaque triangle du sol et des rochers)
            bool aoHit = false;
            float aoT = 0;
            uint aoInst = 0, aoPrim = 0;
            // trou d'une feuille : rayon opaque suivant, juste après lui, au plus quelques fois (budget, bits 24 à 26), puis pas d'obstacle. Le coût d'un rayon est borné :
            // six sauts suivis d'une recherche sans limite faisaient 45 ms d'occultation dans l'herbe dense
            bool aoHole = false;
            RayDesc stepRay = ray;
            const uint maxHops = (flags >> 24) & 7;
            for (uint hop = 0; hop <= maxHops; hop++)
            {
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
                qf.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, stepRay);
                qf.Proceed();
                aoHit = qf.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                if (!aoHit)
                    break;
                aoT = qf.CommittedRayT(), aoInst = qf.CommittedInstanceID(), aoPrim = qf.CommittedPrimitiveIndex();
                aoHole = (flags & 2048) && Hole(ray.Origin + ray.Direction * aoT, p3);
                if (!aoHole)
                    break;
                stepRay.TMin = aoT + max(1e-3, 1e-4 * aoT);
            }
            // (flags bit 23 : ancienne règle, chaque triangle examiné, pour comparer)
            if (aoHit && aoHole && (flags & (1u << 23)) == 0)
                aoHit = false;
            if ((flags & 2048) && (flags & (1u << 23)))
            {
                RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
                q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
                while (q.Proceed())
                    if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && !Hole(ray.Origin + ray.Direction * q.CandidateTriangleRayT(), p3))
                        q.CommitNonOpaqueTriangleHit(); // le plus proche reste (les candidats plus loin sont écartés)
                aoHit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                if (aoHit)
                    aoT = q.CommittedRayT(), aoInst = q.CommittedInstanceID(), aoPrim = q.CommittedPrimitiveIndex();
            }
            // obstacle proche : occultation forte ; vers la portée, elle s'éteint en douceur
            const float t = aoHit ? aoT / aoRadius : 1.0;
            occlusion += (1.0 - t) * (1.0 - t) / float(aoRays) * (rough ? 0.75 : 1.0);
            // la lumière du ciel retirée dans cette direction est remplacée par celle que renvoie l'obstacle (même
            // poids) ; obstacle jamais vu à l'écran : rien (le jeu garde son propre éclairage d'ambiance au loin)
            float3 hc;
            // (exemplaire 0 : scène de l'image ; le cache de géométrie n'a pas de couleurs par triangle)
            if ((flags & (1u << 20)) && aoHit && aoInst == 0 && Cached(aoPrim, hc))
                bounceSum += hc * ((1.0 - t) * (1.0 - t) / float(aoRays));
        }
        // visibilité du ciel à grande portée : un ou deux rayons longs (skyRange, 12 m par défaut) disent si un obstacle cache le ciel dans
        // cette direction : canopée au-dessus du sous-bois, auvent profond, falaise, ruelle étroite. Ils ne retirent que la lumière du ciel,
        // comme l'occultation de contact ; un obstacle lointain compte moins (1 - t / portée). Les trous des feuilles laissent passer le rayon
        // (même règle que l'occultation). Approximation nommée : le ciel est supposé uniforme (pas de direction privilégiée de la lumière d'ambiance)
        // Seules les directions au-dessus de l'horizon portent la lumière du ciel : un rayon qui descend touche le sol, ce qui n'en cache aucune
        // (le jeu éclaire déjà moins une face verticale). Il est renvoyé au-dessus de l'horizon (symétrie par le plan horizontal) et pondéré par sa
        // hauteur dans le ciel. Le haut du monde vient de l'orientation de la caméra (Red Dead : normales du monde, live_rotfit) ; inconnu : pas de rayons longs
        float skyOcc = 0, skyW = 0;
        float3 upCam = 0;
        if ((gnMode & 3) == 2 && sunBuf.Load(2092) != 0)
            upCam = float3(asfloat(sunBuf.Load(2056)), asfloat(sunBuf.Load(2068)), asfloat(sunBuf.Load(2080)));
        if (skyAmount > 0 && skyRange > aoRadius && !rough && dot(upCam, upCam) > 0.5 && (flatHere || DepthContinuous(int2(id), p3)))
        {
            upCam = normalize(upCam);
            const uint skyRays = w > 25.0 ? 1u : 2u;
            uint hs = AoHash(id.x * 7919u + id.y * 104729u + frameIndex * 15485863u + 17u);
            for (uint ks = 0; ks < skyRays; ks++, hs = AoHash(hs))
            {
                const float s1 = float(hs & 0xFFFF) / 65536.0, s2 = (float(hs >> 16) / 65536.0 + float(ks)) / float(skyRays);
                const float rr = sqrt(s1), ph = 6.2831853 * s2;
                RayDesc sr;
                sr.Origin = p3 + n * max(0.01, 0.002 * length(p3));
                sr.Direction = normalize(b1 * (rr * cos(ph)) + b2 * (rr * sin(ph)) + n * sqrt(max(0.0, 1.0 - s1)));
                float upDot = dot(sr.Direction, upCam);
                if (upDot < 0)
                    sr.Direction -= 2.0 * upDot * upCam, upDot = -upDot;
                if (upDot < 0.02 || dot(sr.Direction, n) < 0.02)
                    continue;
                skyW += upDot;
                sr.TMin = 0.06;
                sr.TMax = skyRange;
                bool skyHit = false;
                float skyT = 0;
                const uint maxHopsSky = (flags >> 24) & 7;
                for (uint hop2 = 0; hop2 <= maxHopsSky; hop2++)
                {
                    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qs;
                    qs.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, sr);
                    qs.Proceed();
                    skyHit = qs.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
                    if (!skyHit)
                        break;
                    skyT = qs.CommittedRayT();
                    if (!((flags & 2048) && Hole(sr.Origin + sr.Direction * skyT, p3)))
                        break;
                    skyHit = false;
                    sr.TMin = skyT + max(1e-3, 1e-4 * skyT);
                }
                if (skyHit)
                    skyOcc += upDot * (1.0 - skyT / skyRange);
            }
            skyOcc = skyW > 0 ? skyOcc / skyW : 0.0;
            occlusion = 1.0 - (1.0 - saturate(occlusion)) * (1.0 - skyAmount * skyOcc);
        }
        if (aoOnly && (flags & 17) == 1)
            gameVis = dot(n, sun) > 0.08 ? GameLit(p3) : 0.0;
        const float ndlSky = (flags & 16) ? 0.0 : saturate(dot(n, sun)) * gameVis;
        const float skyShare = sunSky > 0 ? 1.0 / (1.0 + sunSky * ndlSky) : 1.0 - 0.6 * ndlSky;
        // (le dernier tiers de la portée : effacée en douceur, la portée change avec le budget de temps)
        lit = max(0.0, lit - aoAmount * occlusion * skyShare * saturate((aoMaxDist - w) / (0.3 * aoMaxDist)));
    }
    // au loin, le jeu noie le paysage dans la brume : une ombre RT pleine y fait des taches noires ; elle
    // s'efface entre 120 et 300 unités (mètres dans RDR2)
    if (w > 0)
        lit = lerp(lit, 1.0, saturate((w - 120.0) / 180.0)), weight *= 1.0 - saturate((w - 120.0) / 180.0);
    if (flags & 4096)
    {
        // SIGMA débruite la visibilité seule ; les corrections s'appliquent après (live_nrdpost)
        penumbra.Store(id.y * shadowPitch + id.x * 4, asuint(PackPenumbra(hitDistance, tan(sunRadius))));
        shadow.Store(id.y * shadowPitch + id.x * 4, asuint(weight));
        return;
    }
    const float rawLit = lit;
    float motionPixels = 0;
    bool historyUsed = false;
    // accumulation dans le temps : le masque de l'image précédente, lu là où le vecteur de mouvement du jeu
    // dit que ce point se trouvait, si la profondeur y correspond (sinon : point découvert, rien à reprendre)
    if (motionConvention < 16 && w > 0 && w < 1e5)
    {
        const uint raw = motion.Load(id.y * motionPitch + id.x * 4);
        float2 mv = float2(f16tof32(raw & 0xFFFF), f16tof32(raw >> 16));
        const uint k = motionConvention;
        mv = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / float2(width, height) : mv * 2.0;
        if (k & 4)
            mv = -mv;
        if (k & 8)
            mv.y = -mv.y;
        const float2 prevUv = (float2(id) + 0.5) / float2(width, height) + mv;
        motionPixels = length(mv * float2(width, height));
        if (all(isfinite(mv)) && all(prevUv > 0) && all(prevUv < 1))
        {
            const float2 q = prevUv * float2(width, height) - 0.5;
            const int2 b = int2(floor(q));
            const float2 f = q - b;
            float hist = 0, wsum = 0;
            for (int j = 0; j < 4; j++)
            {
                const int2 t = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(width - 1, height - 1));
                const uint rawD = depthPrev.Load(t.y * depthPitch + t.x * 4);
                const float dp = depth24 != 0 ? float(rawD & 0xFFFFFF) / 16777215.0 : asfloat(rawD);
                const float wp = gB / (dp - gA);
                const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
                if (wp > 0 && abs(wp - w) < 0.04 * w + 0.05)
                    hist += asfloat(prevShadow.Load(t.y * shadowPitch + t.x * 4)) * bw, wsum += bw;
            }
            if (wsum > 0.25)
                lit = lerp(hist / wsum, lit, temporalAlpha), historyUsed = true;
        }
    }
    // vues de diagnostic (affichées par la vue « masque »)
    if (debugMode == 1)
        lit = rawLit;
    else if (debugMode == 2)
        lit = saturate(motionPixels / 20.0); // blanc : 20 pixels de mouvement ou plus
    else if (debugMode == 3)
        lit = historyUsed ? 1.0 : 0.0;       // noir : historique rejeté (point découvert, profondeur changée)
    shadow.Store(id.y * shadowPitch + id.x * 4, asuint(aoSkipped && debugMode == 0 ? -1.0 : lit));
    if (flags & (1u << 20))
    {
        const uint plane = shadowPitch * height, a = id.y * shadowPitch + id.x * 4;
        const float3 b = aoSkipped ? float3(-1, -1, -1) : bounceSum;
        bounce.Store(a, asuint(b.r)), bounce.Store(plane + a, asuint(b.g)), bounce.Store(2 * plane + a, asuint(b.b));
    }
}
