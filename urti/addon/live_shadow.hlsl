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
    if ((flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
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
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
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
    if ((flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
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
    if (aoAmount > 0 && (flags & 4096) == 0 && d != gClear && w > 0 && w < 50.0 && Flatness(int2(id), p3) > 0.5 && !SeesThroughAround(int2(id), p3))
    {
        // au quart : le pixel du bloc 2×2 désigné pour cette image (les quatre en quatre images)
        static const uint2 kQuarter[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };
        aoSkipped = (flags & 128) && any((id & 1u) != kQuarter[frameIndex & 3]);
    }
    if (aoAmount > 0 && !aoSkipped && (flags & 4096) == 0 && d != gClear && w > 0 && w < 50.0 && Flatness(int2(id), p3) > 0.5 &&
        !SeesThroughAround(int2(id), p3))
    {
        const float3 n = NormalAt(int2(id), p3);
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
            ray.Origin = p3 + n * max(0.01, 0.002 * length(p3));
            ray.Direction = normalize(b1 * (r * cos(phi)) + b2 * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
            // obstacles à moins de 6 cm ignorés : les décalques du jeu (boue, marques, flaques), posés juste au-dessus
            // du sol sans écrire la profondeur, faisaient des taches sombres à leur forme exacte
            ray.TMin = 0.06;
            ray.TMax = aoRadius;
            RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
            while (q.Proceed())
                if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
                    ((flags & 2048) == 0 || !Hole(ray.Origin + ray.Direction * q.CandidateTriangleRayT(), p3)))
                    q.CommitNonOpaqueTriangleHit(); // le plus proche reste (les candidats plus loin sont écartés)
            // obstacle proche : occultation forte ; vers la portée, elle s'éteint en douceur
            const float t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() / aoRadius : 1.0;
            occlusion += (1.0 - t) * (1.0 - t) / float(aoRays);
            // la lumière du ciel retirée dans cette direction est remplacée par celle que renvoie l'obstacle (même
            // poids) ; obstacle jamais vu à l'écran : rien (le jeu garde son propre éclairage d'ambiance au loin)
            float3 hc;
            if ((flags & (1u << 20)) && q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && Cached(q.CommittedPrimitiveIndex(), hc))
                bounceSum += hc * ((1.0 - t) * (1.0 - t) / float(aoRays));
        }
        const float ndlSky = (flags & 16) ? 0.0 : saturate(dot(n, sun)) * gameVis;
        const float skyShare = sunSky > 0 ? 1.0 / (1.0 + sunSky * ndlSky) : 1.0 - 0.6 * ndlSky;
        lit = max(0.0, lit - aoAmount * occlusion * skyShare);
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
