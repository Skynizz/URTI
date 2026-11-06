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
RWByteAddressBuffer reflection : register(u0); // float4 par pixel : u, v, poids, lissé ; poids < 0 : surface
                                               // réfléchissante dont le point reflété est caché ou hors de
                                               // l'écran (couleur inconnue : la stabilisation garde l'historique) ;
                                               // u < -0,5 : couleur tirée du cache de surfaces (-1 - r, g, poids, b)
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
};

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
    const float3 n = water ? nWater : p.z > 0 ? NormalAt(int2(id), p) : float3(0, 1, 0);
    // reflets RT seulement sur une surface plane (sol, flaque, vitre, eau) : sur un feuillage, des cheveux, un
    // visage, la normale tirée de la profondeur est du bruit, le reflet partait au hasard et clignotait (points
    // gris) ; le jeu y garde ses propres reflets. Sol mouillé : en plus, tourné vers le haut
    const float flat = water ? 1.0 : p.z > 0 ? Flatness(int2(id), p, 2) : 0.0; // voisins proches : au loin, 4 pixels tombent sur un autre objet
    // miroir parfait d'après le matériau (eau : Horizon Forbidden West y écrit exactement 0 dans sa 5e cible) : la surface est
    // lisse par définition, ses vagues n'y changent rien
    const float material = Smoothness(int2(id));
    const float s = water ? 1.0 : max(material * ((flags & 4) || material > 0.98 ? 1.0 : flat), wet > 0 ? wet * saturate((n.y - 0.55) / 0.3) * flat : 0.0);
    float4 result = float4(0, 0, 0, (flags & 16) ? s : 0.0);
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
        RayDesc ray;
        ray.Origin = p + n * (bias * p.z);
        ray.Direction = r;
        ray.TMin = 0.05; // pas les décalques du jeu posés juste au-dessus de la surface
        ray.TMax = 500.0; // au-delà, la brume du jeu efface tout reflet
        // chaque triangle rencontré est examiné : gardé sauf si c'est un trou que la caméra voit à travers
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0x01, ray);
        while (q.Proceed())
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && ((flags & 32) == 0 || !SeenThrough(ray.Origin + r * q.CandidateTriangleRayT())))
                q.CommitNonOpaqueTriangleHit();
        // diagnostic (flags 8, vue 7) : pourquoi pas de reflet. 0,1 rien touché et vers l'arrière ; 0,2 rien touché, pas de
        // ciel à cet endroit de l'écran ; 0,4 touché hors de l'écran ; 0,6 touché, l'écran y montre plus près (caché) ; 0,8
        // touché, l'écran y montre plus loin
        float why = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.4 : (r.z > 1e-3 ? 0.2 : 0.1);
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT && r.z > 1e-3)
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
        else if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && r.z > -0.999)
        {
            const float3 h = ray.Origin + r * q.CommittedRayT();
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
                        const bool hitKnown = AnchorOf(q.CommittedInstanceID(), q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), hitId, prevHit, prevHitN);
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
                        result = float4(uv, weight * edge.x * edge.y, (flags & 8) ? saturate(q.CommittedRayT() / 50.0) : (flags & 16) ? s : q.CommittedRayT());
                        onScreen = true;
                    }
                }
            }
            // caché ou hors de l'écran : la couleur que le cache de surfaces a retenue pour ce triangle (pas dans
            // les vues de diagnostic, qui lisent la 4e composante)
            float3 cached;
            if (!onScreen && (flags & 24) == 0 && q.CommittedInstanceID() == 0 && Cached(q.CommittedPrimitiveIndex(), cached))
            {
                uint hitId;
                float3 prevHit, prevHitN;
                const bool hitKnown = Anchor(q.CommittedPrimitiveIndex(), q.CommittedTriangleBarycentrics(), hitId, prevHit, prevHitN);
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
    anchor.Store4(id.y * reflectPitch + id.x * 16, anchorOut);
}
