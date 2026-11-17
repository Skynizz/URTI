// URTI en direct : reflets en ray tracing. Un rayon par pixel lisse (selon le matériau du G-buffer du
// jeu), réfléchi autour de la normale tirée de la profondeur, lancé dans la géométrie captée. Le point
// touché est reprojeté à l'écran : s'il y est visible (même profondeur), sa couleur sera lue dans
// l'image du jeu par la composition. Sortie par pixel : position à l'écran du point reflété (u, v),
// poids (Fresnel × lissé), 0 si rien d'utilisable (le reflet du jeu reste alors).
//
// Ancrage (mémoire géométrique) : la surface-miroir et le point reflété sont retrouvés dans la géométrie
// captée (même dessin, même triangle, mêmes coordonnées barycentriques) ; les sommets de l'image
// précédente donnent leur position à ce moment-là, déformations comprises. Le point virtuel (le point
// reflété symétrisé par le plan du miroir) de l'image précédente donne l'endroit exact où ce reflet se
// trouvait : la stabilisation y lit son historique, si les identités (objet reflété, miroir) concordent.

RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer depth : register(t1);    // profondeur du jeu (32 bits par texel, lignes alignées)
ByteAddressBuffer material : register(t2); // copie de l'image de matériau du G-buffer (32 bits par texel)
ByteAddressBuffer sunBuf : register(t3);   // octets 36 à 47 : loi de profondeur de cette image
StructuredBuffer<uint4> regions : register(t4); // par dessin de l'écran : début (octets), taille, compteur, identité
ByteAddressBuffer links : register(t5);    // par dessin : identité, début (sommets) du même objet dans l'image précédente + 1
ByteAddressBuffer viewCur : register(t6);  // sommets de cette image (float3, espace caméra)
ByteAddressBuffer viewPrev : register(t7); // sommets de l'image précédente (son espace caméra)
ByteAddressBuffer surfCache : register(t8); // par triangle : couleur vue à l'écran (RGB9E5), identité et mesures (live_surfcache)
ByteAddressBuffer viewT : register(t9);     // couche des surfaces transparentes planes (eau, vitres) : float3 par sommet
ByteAddressBuffer geoColors : register(t10); // flags 16384 : couleurs apprises des objets du cache de géométrie (live_surfcache)
RWByteAddressBuffer reflection : register(u0); // float4 par pixel : u, v, poids, lissé ; poids < 0 : surface
                                               // réfléchissante dont le point reflété est caché ou hors de
                                               // l'écran (couleur inconnue : la stabilisation garde l'historique) ;
                                               // u < -0,5 : couleur tirée du cache de surfaces (-1 - r, g, poids, b)
RWByteAddressBuffer rstats : register(u2);     // flags 131072 (reflect_stats=1) : pixels à reflet, dont l'eau, rayons lancés, pixels sautés, sauts au plafond
RWByteAddressBuffer anchor : register(u1);     // uint4 par pixel : position précédente du reflet (u, v ; NaN si
                                               // inconnue), identité de l'objet reflété, identité du miroir

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint materialPitch;
    float A;
    float B;
    float p00;
    float p11;
    float clearDepth;
    uint depth24;
    uint channel;      // canal du matériau qui porte le lissé (0 à 3)
    float smoothMin;   // en dessous : pas de reflet RT
    float bias;
    uint flags;        // 1 matériau lu, 2 inverser le canal (rugosité au lieu de lissé), 4 tout est lisse (contrôle),
                       // 8 diagnostic : distance de touche normalisée ; 16 diagnostic : lissé ;
                       // 32 : les trous des plaques découpées (vus à travers à l'écran) laissent passer le rayon ;
                       // 64 : cache de surfaces lisible ; 256 : couche des surfaces transparentes planes (eau, vitres)
                       // (sinon la 4e composante est la distance de touche, 1e4 pour le ciel)
    float f0;          // réflectance à incidence normale (diélectrique ~0,04)
    uint reflectPitch;
    uint regionCount;
    uint anchors;      // 1 : sommets de l'image précédente disponibles
    uint prevVertexCount; // sommets lisibles dans viewPrev (une lecture hors d'un tampon racine n'est pas bornée)
    uint cacheTriangles;  // triangles tenus par le cache de surfaces
    float3 worldUp;       // flags 8192 : le haut du monde dans l'espace caméra de cette image (normale exacte de l'eau)
    uint budget;          // budget de temps (add-on) : bits 0 à 3 un pixel sur 1, 2 ou 4 reçoit un rayon, bits 4 à 7 sauts de trou en trou, bits 8 et plus numéro d'image
    float tMaxNow;        // portée des rayons réfléchis (mètres)
};

// flags 262144 : cache de radiance du monde (caméra du jeu connue). Un point reflété caché ou hors de l'écran prend la couleur vue là
// (table de hachage spatiale), un rayon qui ne touche rien celle du ciel dans sa direction (carte du ciel) ; le rayon traverse les
// feuillages selon leur part pleine apprise. Sortie en plus (mixOut) : couleur déjà connue (rgb) et part à lire dans l'image en (u, v) (a)
ByteAddressBuffer radHash : register(t11);
ByteAddressBuffer envMap : register(t12);
ByteAddressBuffer radParams : register(t13);
RWByteAddressBuffer mixOut : register(u3);
#define RAD_BUF radHash
#define RAD_ENV envMap
#define RAD_PARAMS radParams
#include "live_radhash.hlsli"

static const float kSkipped = -0.25; // u d'un pixel à reflet qui n'a pas reçu de rayon à cette image (live_reflresolve prend la couleur de ses voisins)

void Count(uint at)
{
    uint unused;
    rstats.InterlockedAdd(at, 1, unused);
}

// dessin qui contient ce triangle (zones triées par début)
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

float3 Vertex(ByteAddressBuffer b, uint v) { return asfloat(b.Load3(v * 12)); }

// couleur retenue pour ce triangle par le cache de surfaces, si elle est à son objet actuel.
// Une entrée pour huit triangles consécutifs (voisins dans le maillage) : dans RDR2, la plupart des triangles
// sont plus petits qu'un pixel et une entrée par triangle n'était presque jamais remplie (un rayon d'occultation
// sur quatre trouvait sa couleur). Entrée vide ou d'un autre objet : les entrées voisines du même objet
bool Cached(uint prim, out float3 c)
{
    c = 0;
    if ((flags & 64) == 0 || prim >= cacheTriangles || regionCount == 0)
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

// objet du cache de géométrie (identité : bit 23, génération, place) : la couleur apprise de son groupe de triangles, ou d'un groupe voisin
bool CachedGeo(uint inst, uint prim, uint tris, out float3 c)
{
    c = 0;
    if ((flags & 16384) == 0 || (inst & 0x800000u) == 0)
        return false;
    const uint slot = inst & 0x3FFFu, gen = (inst >> 14) & 255u;
    const int bin = int(min(255u, uint(float(prim) / float(max(tris, 1u)) * 256.0)));
    static const int kNear[5] = { 0, -1, 1, -2, 2 };
    for (uint k = 0; k < 5; k++)
    {
        const int b = bin + kNear[k];
        if (b < 0 || b > 255)
            continue;
        const uint2 v = geoColors.Load2((slot * 256u + uint(b)) * 8u);
        if ((v.y & 255u) != 0 && (v.y >> 8) == gen)
        {
            c = float3(v.x & 511, (v.x >> 9) & 511, (v.x >> 18) & 511) * exp2(float(int(v.x >> 27) - 24));
            return all(isfinite(c));
        }
    }
    return false;
}

// couleur et part pleine d'un point touché que l'écran ne montre pas : ce que le cache de radiance du monde a vu là, sinon la couleur
// apprise de son objet (plus grossière)
// 0 : jamais vu ; 1 : couleur et part pleine ; 2 : que des trous à cet endroit (le rayon passe)
static uint gRng = 1u;
float Rand01()
{
    gRng = gRng * 747796405u + 2891336453u;
    uint x = ((gRng >> ((gRng >> 28u) + 4u)) ^ gRng) * 277803737u;
    return float((x >> 22u) ^ x) * (1.0 / 4294967296.0);
}

uint Remembered(float3 h, float path, uint inst, uint prim, uint contrib, out float3 c, out float alpha)
{
    const uint found = RadLookup(RadWorld(h), RadLevel(path), float3(Rand01(), Rand01(), Rand01()), c, alpha);
    if (found != 0)
        return found;
    float3 g;
    if (inst != 0 ? CachedGeo(inst, prim, contrib, g) : Cached(prim, g))
    {
        c = g;
        return 1;
    }
    return 0;
}

// identité de l'objet du triangle ; position et normale du même point à l'image précédente si connues
bool Anchor(uint prim, float2 bary, out uint id, out float3 prevPos, out float3 prevNormal)
{
    prevPos = prevNormal = 0;
    id = 0;
    if (regionCount == 0 || anchors == 0)
        return false;
    const uint k = RegionIndex(prim);
    const uint4 r = regions[k];
    const uint byte = prim * 48;
    id = r.w;
    if (anchors == 0 || byte < r.x || byte - r.x >= r.y)
        return false;
    const uint start = links.Load(k * 8 + 4);
    if (start == 0)
        return false;
    const uint v = start - 1 + (byte - r.x) / 16;
    if (v + 2 >= prevVertexCount)
        return false;
    const float3 a = Vertex(viewPrev, v), b = Vertex(viewPrev, v + 1), c = Vertex(viewPrev, v + 2);
    prevPos = a + bary.x * (b - a) + bary.y * (c - a);
    prevNormal = normalize(cross(b - a, c - a));
    return all(isfinite(prevPos)) && all(isfinite(prevNormal));
}

// même chose selon l'exemplaire touché : seule la scène de l'image (exemplaire 0) a des zones et des sommets de l'image
// précédente ; un objet du cache de géométrie n'a que sa structure
bool AnchorOf(uint instance, uint prim, float2 bary, out uint id, out float3 prevPos, out float3 prevNormal)
{
    id = 0, prevPos = 0, prevNormal = 0;
    if (instance != 0)
        return false;
    return Anchor(prim, bary, id, prevPos, prevNormal);
}

static float gA, gB, gClear;

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

// un point que la caméra voit à travers (elle voit plus loin que lui, ou le ciel) n'est pas solide : c'est
// un trou d'une plaque découpée par alpha test (feuillage, herbe, grillage, franges), dont la géométrie
// captée n'a que le rectangle. Hors de l'écran, on ne sait pas : on le suppose plein.
bool SeenThrough(float3 h)
{
    if (h.z <= 1e-3)
        return false;
    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv <= 0) || any(uv >= 1))
        return false;
    const float3 seen = PointAt(int2(uv * float2(width, height)));
    return seen.z <= 0 || seen.z > h.z * 1.03 + 0.05;
}

float3 NormalAt(int2 t, float3 p)
{
    // différence centrée sur quatre pixels de part et d'autre quand les deux côtés sont sur la même surface
    // (leur milieu retombe sur le point) : normale lissée, les vaguelettes de l'eau et le grain de la profondeur
    // ne font plus trembler le reflet ; sinon (bord d'objet) le côté le plus proche, sur deux pixels
    float3 axis[2];
    for (uint k = 0; k < 2; k++)
    {
        const int2 o = k == 0 ? int2(1, 0) : int2(0, 1);
        const float3 l4 = PointAt(t - o * 4), r4 = PointAt(t + o * 4);
        if (l4.z > 0 && r4.z > 0 && length(0.5 * (l4 + r4) - p) < 0.01 * p.z + 0.01)
            axis[k] = r4 - l4;
        else
        {
            const float3 l = PointAt(t - o * 2), r = PointAt(t + o * 2);
            axis[k] = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
        }
    }
    float3 n = normalize(cross(axis[0], axis[1]));
    return dot(n, p) > 0 ? -n : n;
}

// surface plane autour du pixel : les points à 4 pixels de part et d'autre, sur chaque axe, sont alignés
// avec lui (vrai pour un sol, un toit, une flaque, quelle que soit la perspective ; faux pour un feuillage,
// des branches, des câbles, où la profondeur saute). 1 : plane ; 0 : pas une surface continue.
float Flatness(int2 t, float3 p, int k = 4)
{
    const float3 l = PointAt(t - int2(k, 0)), r = PointAt(t + int2(k, 0)), u = PointAt(t - int2(0, k)), b = PointAt(t + int2(0, k));
    if (l.z <= 0 || r.z <= 0 || u.z <= 0 || b.z <= 0)
        return 0;
    const float3 h = r - l, v = b - u;
    const float eh = length(cross(h, p - l)) / max(length(h), 1e-6);
    const float ev = length(cross(v, p - u)) / max(length(v), 1e-6);
    // relief dans les deux directions (feuillage, cheveux) ; sur une seule (bord d'une flaque contre un mur, objet
    // au loin dans l'axe vertical), la surface reste plane
    return saturate(1.0 - (k == 4 ? max(eh, ev) : min(eh, ev)) / (0.004 * p.z + 0.002));
}

float Smoothness(int2 t)
{
    if (flags & 4) // contrôle : toutes les surfaces lisses
        return 1.0;
    if ((flags & 1) == 0)
        return 0;
    const uint raw = material.Load(t.y * materialPitch + t.x * 4);
    float s = float((raw >> (8 * channel)) & 255) / 255.0;
    return (flags & 2) ? 1.0 - s : s;
}

// eau d'après le matériau : au moins trois de ses quatre voisins aussi, à peu près à la même distance (pas un point isolé sur un feuillage)
bool WaterAround(int2 t, float3 p)
{
    uint count = 0;
    for (uint j = 0; j < 4; j++)
    {
        const int2 o = t + (j == 0 ? int2(1, 0) : j == 1 ? int2(-1, 0) : j == 2 ? int2(0, 1) : int2(0, -1));
        const float3 q = PointAt(o);
        if (q.z > 0 && abs(q.z - p.z) < 0.1 * p.z + 0.05 && Smoothness(o) > 0.98)
            count++;
    }
    return count >= 3;
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
    float3 p = PointAt(int2(id));
    float waterFade = 1.0; // eau : reflet effacé là où elle est trop mince (berges, fond qui affleure)
    // eau, vitre : surface transparente plane devant le point que montre la profondeur (couche 2 de la scène RT, triée par
    // live_planes, urti.ini water=1 ; pour un jeu qui ne l'écrit pas dans la profondeur) : le reflet part d'elle, avec la
    // normale de son triangle, lisse comme un miroir
    bool water = false;
    float3 nWater = 0;
    if ((flags & 256) && p.z > 0)
    {
        RayDesc ew;
        ew.Origin = 0;
        ew.Direction = normalize(p);
        ew.TMin = 0.05;
        ew.TMax = max(0.06, length(p) - 0.03);
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qw;
        qw.TraceRayInline(scene, RAY_FLAG_NONE, 0x02, ew);
        qw.Proceed();
        if (qw.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            const uint t3 = qw.CommittedPrimitiveIndex() * 36;
            const float3 a = asfloat(viewT.Load3(t3)), b = asfloat(viewT.Load3(t3 + 12)), c = asfloat(viewT.Load3(t3 + 24));
            float3 nw = cross(b - a, c - a);
            if (all(isfinite(nw)) && dot(nw, nw) > 1e-12)
            {
                nw = normalize(nw);
                water = true, nWater = dot(nw, ew.Direction) > 0 ? -nw : nw;
                // épaisseur d'eau le long du regard (de la surface au fond que montre la profondeur) : la surface du jeu
                // continue sous les berges, un reflet plein sur quelques millimètres d'eau y faisait des taches
                waterFade = saturate((length(p) - qw.CommittedRayT() - 0.05) / 0.3);
                p = ew.Direction * qw.CommittedRayT();
            }
        }
    }
    // sol mouillé (smoothMin < 0 : -smoothMin = quantité d'eau) : les surfaces tournées vers le haut
    // deviennent lisses comme une rue après la pluie
    const float wet = smoothMin < 0 ? -smoothMin : 0.0;
    const float sMin = smoothMin < 0 ? 0.35 : smoothMin;
    // matériau d'abord : hors de l'eau et des surfaces lisses (la plus grande partie de l'écran), plus rien à calculer (ni normale ni planéité)
    float material = Smoothness(int2(id));
    if (!water && wet <= 0 && (flags & 24) == 0 && (p.z <= 0 || material < sMin || ((flags & 65536) && material <= 0.98)))
    {
        reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(float4(0, 0, 0, 0)));
        if (anchors != 0)
            anchor.Store4(id.y * reflectPitch + id.x * 16, uint4(0x7FC00000, 0x7FC00000, 0, 0));
        return;
    }
    float3 n = water ? nWater : p.z > 0 ? NormalAt(int2(id), p) : float3(0, 1, 0);
    // reflets RT seulement sur une surface plane (sol, flaque, vitre, eau) : sur un feuillage, des cheveux, un
    // visage, la normale tirée de la profondeur est du bruit, le reflet partait au hasard et clignotait (points
    // gris) ; le jeu y garde ses propres reflets. Sol mouillé : en plus, tourné vers le haut
    const float flat = water ? 1.0 : p.z > 0 ? Flatness(int2(id), p, 2) : 0.0; // voisins proches : au loin, 4 pixels tombent sur un autre objet
    // miroir parfait d'après le matériau (eau : Horizon Forbidden West y écrit exactement 0 dans sa 5e cible) : la surface est
    // lisse par définition, ses vagues n'y changent rien
    // caméra du jeu connue : miroir parfait d'après le matériau seulement sur une surface horizontale et étendue (l'eau), et avec sa normale exacte :
    // tirée de la profondeur, elle change à chaque image avec le décalage sous-pixel, le reflet tremblait et ne se nettoyait qu'en s'accumulant, jeu en pause
    if ((flags & 8192) && !water && p.z > 0 && material > 0.98)
    {
        if (dot(n, worldUp) > 0.7 && WaterAround(int2(id), p))
            n = worldUp;
        else
            material = 0;
    }
    const float s = water ? 1.0 : max(material * ((flags & 4) || material > 0.98 ? 1.0 : flat), wet > 0 ? wet * saturate((n.y - 0.55) / 0.3) * flat : 0.0);
    float4 result = float4(0, 0, 0, (flags & 16) ? s : 0.0);
    float4 mixResult = float4(0, 0, 0, 1);
    const bool useRad = (flags & 262144) != 0 && (flags & 24) == 0;
    const float nan = asfloat(0x7FC00000);
    uint4 anchorOut = uint4(asuint(nan), asuint(nan), 0, 0);
    // pas de rayon là où le reflet serait presque invisible (surface vue de face : Fresnel ~4 %) : moins de 3 %
    const float fresnelEarly = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(-normalize(p), n)), 5.0);
    if (p.z > 0 && s >= sMin && fresnelEarly * saturate((s - sMin) / max(1e-3, 1.0 - sMin)) * waterFade >= 0.03)
    {
        // surface-miroir dans la géométrie captée : le premier triangle sur le rayon de la caméra, s'il est à
        // la profondeur que l'image montre
        bool mirrorKnown = false;
        float3 prevMirror = 0, prevMirrorN = 0;
        if (anchors != 0)
        {
            RayDesc eye;
            eye.Origin = 0;
            eye.Direction = normalize(p);
            eye.TMin = 0.0;
            eye.TMax = length(p) * 1.05 + 0.1;
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qe;
            qe.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, eye);
            qe.Proceed();
            if (qe.CommittedStatus() == COMMITTED_TRIANGLE_HIT && abs(qe.CommittedRayT() - length(p)) < 0.02 * length(p) + 0.05)
            {
                uint mirrorId;
                mirrorKnown = AnchorOf(qe.CommittedInstanceID(), qe.CommittedPrimitiveIndex(), qe.CommittedTriangleBarycentrics(), mirrorId, prevMirror, prevMirrorN);
                anchorOut.w = mirrorId;
            }
        }
        const float3 v = normalize(p);
        const float3 r = reflect(v, n);
        const float cosTheta = saturate(dot(-v, n));
        // Fresnel (Schlick) ; le lissé au-dessus du seuil module le poids
        const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosTheta, 5.0);
        const float weight = fresnel * saturate((s - sMin) / max(1e-3, 1.0 - sMin)) * waterFade;
        // par défaut : reflet attendu, couleur inconnue (remplacé ci-dessous si le point reflété est visible)
        if (weight > 0)
            result.z = -weight;
        if (flags & 131072)
        {
            Count(0);
            if (material > 0.98 || water)
                Count(4);
        }
        // budget de temps dépassé : un pixel sur deux ou sur quatre reçoit un rayon à cette image (le motif tourne), les autres prennent la couleur de leurs voisins
        {
            const uint stride = budget & 15, frameNo = budget >> 8;
            static const uint2 kFour[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
            const bool skipNow = stride == 2 ? ((id.x + id.y + frameNo) & 1) != 0 : stride >= 4 ? any((id & 1u) != kFour[frameNo & 3]) : false;
            if (skipNow)
            {
                if (flags & 131072)
                    Count(12);
                reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(float4(kSkipped, 0, max(weight, 0.0), 0)));
                if (anchors != 0)
                    anchor.Store4(id.y * reflectPitch + id.x * 16, uint4(0x7FC00000, 0x7FC00000, 0, 0));
                return;
            }
        }
        RayDesc ray;
        ray.Origin = p + n * (bias * p.z);
        ray.Direction = r;
        ray.TMin = 0.05; // pas les décalques du jeu posés juste au-dessus de la surface
        ray.TMax = tMaxNow > 0 ? tMaxNow : 500.0; // au-delà, la brume du jeu efface tout reflet
        if (useRad)
        {
            // le rayon avance de triangle en triangle (au plus quelques-uns : budget). À l'écran, la profondeur de l'image tranche : point visible
            // (sa couleur est dans l'image), trou d'une plaque découpée (la caméra voit plus loin : le rayon continue), ou caché. Caché ou hors de
            // l'écran : la couleur et la part pleine que le cache du monde a retenues là ; le reste du rayon continue derrière. Rien touché : le ciel
            RadLoadParams();
            gRng = (id.x * 1973u + id.y * 9277u) ^ ((budget >> 8) * 26699u) ^ 0x9E3779B9u;
            const float eyeDist = length(p);
            float3 acc = 0;
            float T = 1.0, share = 0, tOut = -1.0;
            float2 uvOut = 0;
            bool unknown = false;
            RayDesc stepRay = ray;
            const uint maxHops = (budget >> 4) & 15;
            for (uint hop = 0; hop <= maxHops; hop++)
            {
                if (flags & 131072)
                    Count(8);
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
                qf.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, stepRay);
                qf.Proceed();
                if (qf.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
                    break;
                const float th = qf.CommittedRayT();
                const uint inst = qf.CommittedInstanceID(), prim = qf.CommittedPrimitiveIndex(), contrib = qf.CommittedInstanceContributionToHitGroupIndex();
                const float3 h = ray.Origin + r * th;
                const bool last = hop == maxHops;
                uint state = 0; // 0 hors de l'écran, 1 visible, 2 trou (vu à travers), 3 caché
                float2 uv = 0;
                float seenZ = 0;
                if (h.z > 1e-3)
                {
                    const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
                    uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if (all(uv > 0.001) && all(uv < 0.999))
                    {
                        seenZ = PointAt(int2(uv * float2(width, height))).z;
                        state = seenZ > 0 && abs(seenZ - h.z) < 0.03 * h.z + 0.05 ? 1u : (seenZ <= 0 || seenZ > h.z) ? 2u : 3u;
                    }
                }
                if (state == 1)
                {
                    // vers les bords de l'écran, la couleur de l'image laisse la place à celle du cache (pas de coupure au bord)
                    const float2 e2 = saturate(min(uv, 1.0 - uv) * 12.0);
                    float edge = e2.x * e2.y;
                    float3 c = 0;
                    float a1 = 1;
                    if (edge < 1.0 && Remembered(h, eyeDist + th, inst, prim, contrib, c, a1) != 1)
                        edge = 1.0;
                    acc += T * (1.0 - edge) * c;
                    share = T * edge, uvOut = uv, T = 0;
                    if (tOut < 0)
                        tOut = th;
                    if (hop == 0)
                    {
                        // ancrage : point reflété à l'image précédente, symétrisé par le plan du miroir d'alors
                        uint hitId;
                        float3 prevHit, prevHitN;
                        const bool hitKnown = AnchorOf(inst, prim, qf.CommittedTriangleBarycentrics(), hitId, prevHit, prevHitN);
                        anchorOut.z = hitId;
                        if (hitKnown && mirrorKnown)
                        {
                            const float3 virt = prevHit - 2.0 * dot(prevHit - prevMirror, prevMirrorN) * prevMirrorN;
                            if (virt.z > 1e-3)
                            {
                                const float2 pn = float2(virt.x * p00 / virt.z, virt.y * p11 / virt.z);
                                anchorOut.xy = asuint(float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5));
                            }
                        }
                    }
                    break;
                }
                if (state == 2 && (flags & 32) && !last)
                {
                    stepRay.TMin = th + max(1e-3, 1e-4 * th);
                    continue;
                }
                float3 c;
                float alpha;
                const uint found = Remembered(h, eyeDist + th, inst, prim, contrib, c, alpha);
                if (found == 2)
                {
                    // que des trous ici : le rayon passe (au dernier saut : le ciel derrière)
                    stepRay.TMin = th + max(1e-3, 1e-4 * th);
                    continue;
                }
                if (found == 0)
                {
                    if (state == 2)
                        break; // trou jamais vu de près : le ciel derrière
                    // jamais vu, loin derrière d'autres objets lointains : même matière, même brume, la couleur de l'écran vaut pour lui
                    if (state == 3 && h.z > 8.0 && seenZ > 0.35 * h.z)
                    {
                        share = T, uvOut = uv, T = 0;
                        if (tOut < 0)
                            tOut = th;
                        break;
                    }
                    unknown = true;
                    break;
                }
                if (flags & 131072)
                    Count(20);
                if (last)
                    alpha = 1.0;
                acc += T * alpha * c;
                T *= 1.0 - alpha;
                if (tOut < 0 && alpha > 0.3)
                    tOut = th;
                if (T < 0.04)
                {
                    T = 0;
                    break;
                }
                stepRay.TMin = th + max(1e-3, 1e-4 * th);
            }
            if (!unknown && T > 0)
            {
                // ce qui reste du rayon part au loin : le ciel, ou le lointain, tel que l'écran le montre dans cette direction (plus net) quand il
                // y est, sinon tel que la carte du ciel l'a retenu
                float3 env;
                const bool envKnown = RadEnv(RadWorldDir(r), env);
                bool onScreen = false;
                float2 uv = 0;
                if (r.z > 1e-3)
                {
                    const float2 ndc = float2(r.x * p00 / r.z, r.y * p11 / r.z);
                    uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                    if (all(uv > 0.001) && all(uv < 0.999))
                    {
                        const float seenFar = PointAt(int2(uv * float2(width, height))).z;
                        onScreen = seenFar <= 0 || seenFar > 20.0;
                    }
                }
                if (onScreen)
                {
                    const float2 e2 = saturate(min(uv, 1.0 - uv) * 12.0);
                    const float edge = envKnown ? e2.x * e2.y : 1.0;
                    acc += T * (1.0 - edge) * env;
                    share = T * edge, uvOut = uv, T = 0;
                }
                else if (envKnown)
                {
                    acc += T * env, T = 0;
                    if (flags & 131072)
                        Count(24);
                }
                else
                    unknown = true;
                if (tOut < 0)
                    tOut = 1e4;
            }
            // une petite part inconnue : le reste du rayon vaut pour elle ; presque tout inconnu : reflet attendu, couleur inconnue (comme avant)
            if (!(unknown && T > 0.5))
            {
                const float k = 1.0 / max(1.0 - T, 1e-3);
                result = float4(uvOut, weight, tOut < 0 ? 20.0 : tOut);
                mixResult = float4(acc * k, share * k);
            }
            else if (flags & 131072)
                Count(28);
        }
        else
        {
        // d'abord un rayon opaque (le matériel trouve seul le plus proche) ; seulement si ce point est un trou que la caméra voit à travers
        // (plaque découpée du feuillage), la recherche lente où chaque triangle rencontré est examiné. Même résultat : avant, chaque rayon
        // examinait chaque triangle (sol, rochers, troncs compris)
        bool hitNow = false;
        float hitT = 0;
        uint hitInst = 0, hitPrim = 0, hitContrib = 0;
        float2 hitBary = 0;
        // trou d'une feuille : rayon opaque suivant, juste après lui, au plus quelques fois (budget) ; le dernier point touché est gardé (feuillage, qu'on le voie
        // à travers ou non). Le coût d'un rayon est borné : six sauts suivis d'une recherche sans limite faisaient 20 ms de reflets dans la végétation dense
        bool holeLeft = false;
        RayDesc stepRay = ray;
        const uint maxHops = (budget >> 4) & 15;
        for (uint hop = 0; hop <= maxHops; hop++)
        {
            if (flags & 131072)
                Count(8);
            RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qf;
            qf.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, stepRay);
            qf.Proceed();
            hitNow = qf.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
            if (!hitNow)
                break;
            hitT = qf.CommittedRayT(), hitInst = qf.CommittedInstanceID(), hitPrim = qf.CommittedPrimitiveIndex(),
            hitContrib = qf.CommittedInstanceContributionToHitGroupIndex(), hitBary = qf.CommittedTriangleBarycentrics();
            holeLeft = (flags & 32) && SeenThrough(ray.Origin + r * hitT);
            if (!holeLeft)
                break;
            if (hop == maxHops && (flags & 131072))
                Count(16);
            stepRay.TMin = hitT + max(1e-3, 1e-4 * hitT);
        }
        // (flags 32768 : ancienne règle, chaque triangle examiné, pour comparer)
        if ((flags & 32) && (flags & 32768))
        {
            RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
            while (q.Proceed())
                if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && !SeenThrough(ray.Origin + r * q.CandidateTriangleRayT()))
                    q.CommitNonOpaqueTriangleHit();
            hitNow = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
            if (hitNow)
                hitT = q.CommittedRayT(), hitInst = q.CommittedInstanceID(), hitPrim = q.CommittedPrimitiveIndex(),
                hitContrib = q.CommittedInstanceContributionToHitGroupIndex(), hitBary = q.CommittedTriangleBarycentrics();
        }
        // diagnostic (flags 8, vue 7) : pourquoi pas de reflet. 0,1 rien touché et vers l'arrière ; 0,2 rien touché, pas de
        // ciel à cet endroit de l'écran ; 0,4 touché hors de l'écran ; 0,6 touché, l'écran y montre plus près (caché) ; 0,8
        // touché, l'écran y montre plus loin
        float why = hitNow ? 0.4 : (r.z > 1e-3 ? 0.2 : 0.1);
        if (!hitNow && r.z > 1e-3)
        {
            // rien touché : le rayon part au loin. Ce qui est à l'infini se reflète tel qu'il est vu dans cette direction : le
            // ciel, ou le lointain que l'écran y montre (forêt noyée de brume au-delà de la scène RT : presque tout le reflet
            // d'un lac vu en rasant)
            const float2 ndc = float2(r.x * p00 / r.z, r.y * p11 / r.z);
            const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            const float seenFar = PointAt(int2(clamp(uv, 0.0, 0.999) * float2(width, height))).z;
            if (all(uv > 0.001) && all(uv < 0.999) && (seenFar <= 0 || seenFar > 20.0))
            {
                const float2 edge = saturate(min(uv, 1.0 - uv) * 12.0);
                result = float4(uv, weight * edge.x * edge.y, (flags & 16) ? s : 1e4);
            }
        }
        else if (hitNow && r.z > -0.999)
        {
            const float3 h = ray.Origin + r * hitT;
            bool onScreen = false;
            if (h.z > 1e-3)
            {
                const float2 ndc = float2(h.x * p00 / h.z, h.y * p11 / h.z);
                const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
                if (all(uv > 0.001) && all(uv < 0.999))
                {
                    // le point touché doit être celui que l'image montre à cet endroit (sinon il est caché)
                    const float3 seen = PointAt(int2(uv * float2(width, height)));
                    why = seen.z > 0 && seen.z < h.z ? 0.6 : 0.8;
                    // point lointain derrière d'autres objets lointains : le rayon réfléchi touche le feuillage en profondeur,
                    // la caméra y voit celui de devant : même matière, même brume, la couleur de l'écran vaut pour lui. Sauf
                    // derrière un objet nettement plus proche (personnage, herbes du premier plan)
                    const bool behindFar = h.z > 8.0 && seen.z > 0.35 * h.z && seen.z < h.z;
                    if (seen.z > 0 && (abs(seen.z - h.z) < 0.03 * h.z + 0.05 || behindFar))
                    {
                        // ancrage : point reflété à l'image précédente, symétrisé par le plan du miroir d'alors
                        uint hitId;
                        float3 prevHit, prevHitN;
                        const bool hitKnown = AnchorOf(hitInst, hitPrim, hitBary, hitId, prevHit, prevHitN);
                        anchorOut.z = hitId;
                        if (hitKnown && mirrorKnown)
                        {
                            const float3 virt = prevHit - 2.0 * dot(prevHit - prevMirror, prevMirrorN) * prevMirrorN;
                            if (virt.z > 1e-3)
                            {
                                const float2 pn = float2(virt.x * p00 / virt.z, virt.y * p11 / virt.z);
                                anchorOut.xy = asuint(float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5));
                            }
                        }
                        // fondu vers les bords de l'écran (couleur inconnue au-delà)
                        const float2 edge = saturate(min(uv, 1.0 - uv) * 12.0);
                        result = float4(uv, weight * edge.x * edge.y, (flags & 8) ? saturate(hitT / 50.0) : (flags & 16) ? s : hitT);
                        onScreen = true;
                    }
                }
            }
            // caché ou hors de l'écran : la couleur que le cache de surfaces a retenue pour ce triangle (pas dans
            // les vues de diagnostic, qui lisent la 4e composante)
            float3 cached;
            if (!onScreen && (flags & 24) == 0 && hitInst != 0 && CachedGeo(hitInst, hitPrim, hitContrib, cached))
                result = float4(-1.0 - cached.r, cached.g, weight, cached.b); // objet du cache : sa couleur apprise
            else if (!onScreen && (flags & 24) == 0 && hitInst == 0 && Cached(hitPrim, cached))
            {
                uint hitId;
                float3 prevHit, prevHitN;
                const bool hitKnown = Anchor(hitPrim, hitBary, hitId, prevHit, prevHitN);
                anchorOut.z = hitId;
                if (hitKnown && mirrorKnown)
                {
                    const float3 virt = prevHit - 2.0 * dot(prevHit - prevMirror, prevMirrorN) * prevMirrorN;
                    if (virt.z > 1e-3)
                    {
                        const float2 pn = float2(virt.x * p00 / virt.z, virt.y * p11 / virt.z);
                        anchorOut.xy = asuint(float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5));
                    }
                }
                result = float4(-1.0 - cached.r, cached.g, weight, cached.b);
            }
        }
        if ((flags & 8) && result.z <= 0)
            result.w = why;
        }
        // diagnostic (reflect_diag=) : direction du rayon réfléchi, composante 1 : y, 2 : z, 3 : normale y, 4 : normale z
        if (flags & 512)
            result.w = 0.5 + 0.5 * r.y;
        if (flags & 1024)
            result.w = 0.5 + 0.5 * r.z;
        if (flags & 2048)
            result.w = 0.5 + 0.5 * n.y;
        if (flags & 4096)
            result.w = 0.5 + 0.5 * n.z;
    }
    reflection.Store4(id.y * reflectPitch + id.x * 16, asuint(result));
    if (useRad)
        mixOut.Store4(id.y * reflectPitch + id.x * 16, asuint(mixResult));
    if (anchors != 0)
        anchor.Store4(id.y * reflectPitch + id.x * 16, anchorOut);
}
