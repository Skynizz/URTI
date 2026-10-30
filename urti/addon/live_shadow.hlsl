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
    uint flags;        // 1 carte du jeu lue, 2 carte en D24, 4 combiner avec l'ombre du jeu, 8 afficher l'ombre du jeu, 16 pas de soleil
    uint motionPitch;
    uint motionConvention; // 0..15 (live_mvcal), 255 : pas d'accumulation
    float temporalAlpha;   // part de l'image courante (0,3 : environ trois images accumulées)
    uint debugMode;        // 1 ombre brute (sans accumulation), 2 vecteurs de mouvement, 3 historique gardé
    uint frameIndex;       // fait tourner le motif des rayons d'une image à l'autre (l'accumulation le moyenne)
    uint pad4;
    uint pad5;
    uint pad6;
};

static float gA, gB, gClear;

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
        // éclairé seulement si tout le voisinage 5×5 l'est : l'ombre du jeu est élargie de deux texels,
        // ce qui absorbe le flou de ses bords (sinon un liseré d'ombre RT double chaque contour)
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
            {
                const uint2 t = uint2(int2(texel) + int2(dx, dy));
                const uint raw = atlas.Load(t.y * atlasPitch + t.x * 4);
                const float stored = (flags & 2) ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
                if (!(sunSign > 0 ? s.z <= stored + gameBias : s.z >= stored - gameBias))
                    return 0.0;
            }
        return 1.0;
    }
    return 1.0;
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
    const float w = gB / (d - gA);
    if ((flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
    {
        const float2 ndc = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p = float3(ndc.x * w / p00, ndc.y * w / p11, w);
        const float dist = length(p);
        // quatre rayons répartis dans le disque solaire (strates 2×2 tournées par pixel) : pénombre lisse
        const float3 t1 = normalize(abs(sun.y) < 0.99 ? cross(sun, float3(0, 1, 0)) : cross(sun, float3(1, 0, 0)));
        const float3 t2 = cross(sun, t1);
        const float rot = 6.2831853 * frac(52.9829189 * frac(dot(float2(id), float2(0.06711056, 0.00583715))) + 0.61803399 * float(frameIndex & 1023));
        lit = 0.0;
        const uint rays = clamp((flags >> 8) & 7, 1u, 4u); // 1 à 4 rayons (réglage shadow_rays)
        for (uint k = 0; k < rays; k++)
        {
            const float u = rays == 1 ? 0.0 : (float(k & 1) + 0.5) * 0.5, v = (float(k >> 1) + 0.5) * 0.5;
            const float r = sunRadius * sqrt(u), a = 6.2831853 * v + rot;
            RayDesc ray;
            ray.Origin = p * (1.0 - bias) + sun * bias * dist;
            ray.Direction = normalize(sun + t1 * (r * cos(a)) + t2 * (r * sin(a)));
            ray.TMin = 0.0;
            ray.TMax = 1e5;
            RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
            q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
            q.Proceed();
            lit += q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0 / rays;
        }
        // face tournée à l'opposé du soleil : le jeu l'assombrit déjà par son éclairage ; une ombre RT en
        // plus y ferait des taches (le test de sa carte la voit éclairée, la surface étant son propre écran)
        const float ndl = dot(NormalAt(int2(id), p), sun);
        const bool facesSun = ndl > 0.08;
        if (!facesSun && (flags & 4))
            lit = 1.0;
        else if (flags & 1)
        {
            const float game = GameLit(p);
            if (flags & 8)
                lit = game; // contrôle : l'ombre du jeu telle que recalculée
            else if (flags & 4)
                lit = 1.0 - game * (1.0 - lit); // n'ajouter de l'ombre que là où le jeu éclaire
        }
    }
    // une ombre ne retire que la lumière directe du soleil, proportionnelle à l'angle de la surface face à lui
    // (N·L) : une surface rasante perd peu, une surface face au soleil perd tout ; le reste (lumière du
    // ciel, rebonds) demeure. Sans cela, l'ombre RT assombrit aussi l'éclairage indirect : trop noire.
    if ((flags & 16) == 0 && d != gClear && w > 0 && w < 1e6)
    {
        const float2 ndc2 = float2((id.x + 0.5) / width * 2 - 1, 1 - (id.y + 0.5) / height * 2);
        const float3 p2 = float3(ndc2.x * w / p00, ndc2.y * w / p11, w);
        lit = 1.0 - (1.0 - lit) * saturate(dot(NormalAt(int2(id), p2), sun) * 2.5);
    }
    // au loin, le jeu noie le paysage dans la brume : une ombre RT pleine y fait des taches noires ; elle
    // s'efface entre 120 et 300 unités (mètres dans RDR2)
    if (w > 0)
        lit = lerp(lit, 1.0, saturate((w - 120.0) / 180.0));
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
    shadow.Store(id.y * shadowPitch + id.x * 4, asuint(lit));
}
