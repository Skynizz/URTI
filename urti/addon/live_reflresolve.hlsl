// URTI en direct : reflets stabilisés dans le temps (comme les débruiteurs des jeux en RT). À la
// résolution de rendu : couleur du point reflété lue dans l'image présentée, plafonnée (pas de points
// blancs isolés), puis mélangée avec l'historique reprojeté par les vecteurs de mouvement du jeu ;
// l'historique est borné par le voisinage de l'image courante (pas de traînées) et rejeté si la
// profondeur ne correspond plus (point découvert).
// Historique ancré (prioritaire) : la position précédente exacte du reflet, retrouvée par la géométrie
// captée (live_reflect) ; gardé seulement si, à cet endroit, l'image précédente reflétait le même objet
// dans le même miroir. Fiable même en mouvement : on y garde beaucoup d'historique.

ByteAddressBuffer image : register(t0);       // image présentée (lignes alignées)
ByteAddressBuffer reflection : register(t1);  // float4 par pixel de rendu : u, v, poids, lissé
ByteAddressBuffer history : register(t2);     // float4 par pixel de rendu : couleur linéaire, poids (image précédente)
ByteAddressBuffer motion : register(t3);      // vecteurs de mouvement du jeu (R16G16 flottants)
ByteAddressBuffer depth : register(t4);       // profondeur de rendu (cette image)
ByteAddressBuffer depthPrev : register(t5);   // profondeur de rendu (image précédente)
ByteAddressBuffer sunBuf : register(t6);      // loi de profondeur (36 à 47), rotation de la caméra (576 à 627)
ByteAddressBuffer anchorCur : register(t7);   // uint4 par pixel de rendu : position précédente du reflet, identités
ByteAddressBuffer anchorPrev : register(t8);  // le même, de l'image précédente
RWByteAddressBuffer resolved : register(u0);  // float4 par pixel de rendu : couleur linéaire, poids

cbuffer Constants : register(b0)
{
    uint width;          // image présentée
    uint height;
    uint pitch;
    uint format;         // 0 RGBA8, 1 BGRA8, 2 HDR10 PQ, 3 RGBA16F
    uint renderWidth;
    uint renderHeight;
    uint motionPitch;
    uint motionConvention; // 255 : pas d'historique
    uint depthPitch;
    uint depth24;
    float alpha;         // part de l'image courante à l'arrêt
    float maxLuminance;  // plafond de la couleur reflétée (lumière linéaire, 1 = blanc de référence)
    uint flags;          // 1 : historique valide ; 2 : image générée (pas de mise à jour) ; 4 : diagnostic, confiance ;
                         // 8 : ancrages de cette image et de la précédente disponibles
    float p00;           // projection supposée (espace caméra de l'add-on)
    float p11;
    float alphaMax;      // part de l'image courante en mouvement rapide
    float gamma;         // bornage : moyenne ± gamma écarts-types du voisinage
    float depthTol;      // historique gardé si la profondeur diffère de moins de depthTol × profondeur
    float anchorAlpha;   // historique ancré : part de l'image courante (0 : ancrage coupé)
    float anchorGamma;   // historique ancré : bornage plus large (moyenne ± anchorGamma σ)
};

float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

float3 LoadLinear(int2 px)
{
    px = clamp(px, int2(0, 0), int2(width - 1, height - 1));
    const uint a = uint(px.y) * pitch + uint(px.x) * (format == 3 ? 8 : 4);
    if (format == 3)
    {
        const uint2 raw = image.Load2(a);
        return float3(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF));
    }
    const uint raw = image.Load(a);
    if (format == 2)
        return PqToNits(float3(raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023) / 1023.0) / 203.0;
    const float3 c = float3(raw & 255, (raw >> 8) & 255, (raw >> 16) & 255) / 255.0;
    return SrgbToLinear(format == 1 ? c.bgr : c);
}

float3 SampleLinear(float2 uv)
{
    const float2 q = uv * float2(width, height) - 0.5;
    const int2 b = int2(floor(q));
    const float2 f = q - b;
    return lerp(lerp(LoadLinear(b), LoadLinear(b + int2(1, 0)), f.x), lerp(LoadLinear(b + int2(0, 1)), LoadLinear(b + int2(1, 1)), f.x), f.y);
}

// décalage sous-pixel du jeu (jitter, DLSS / TAA), mesuré par live_sunvote dans l'espace caméra d'URTI
// (cisaillement x' = x + a w, y' = y + b w) : en pixels de rendu, le contenu d'un pixel est celui du point
// situé à « pixel - jitter » ; écart d'une image à l'autre (précédente - courante) à 628, absolu à 1184
float2 JitterPixels(float2 ab, float p00_, float p11_, float2 size) { return float2(ab.x * p00_, -ab.y * p11_) * 0.5 * size; }
static float2 gJitterUv; // décalage absolu de cette image, en uv (le point reflété projeté dans l'espace décalé)

// reflet courant d'un pixel de rendu : couleur plafonnée × poids, poids ; known = false si la surface
// réfléchit mais que le point reflété est caché ou hors de l'écran (donnée manquante, pas un zéro)
float4 Current(int2 t, out bool known)
{
    t = clamp(t, int2(0, 0), int2(renderWidth - 1, renderHeight - 1));
    const float4 rf = asfloat(reflection.Load4((uint(t.y) * renderWidth + uint(t.x)) * 16));
    known = rf.z >= 0;
    if (rf.z <= 0)
        return float4(0, 0, 0, 0);
    // u < -0,5 : couleur du cache de surfaces (point reflété caché ou hors de l'écran) ; sinon lue dans l'image
    float3 c = rf.x < -0.5 ? float3(-rf.x - 1.0, rf.y, rf.w) : SampleLinear(rf.xy - gJitterUv); // l'image présentée n'est pas décalée (TAA)
    const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (lum > maxLuminance)
        c *= maxLuminance / lum;
    return float4(c * rf.z, rf.z);
}

// reflet caché : part de l'historique gardée à chaque image (0,9 : la moitié après 7 images)
static const float holdDecay = 0.9;

float ViewDepth(ByteAddressBuffer b, int2 t, float A, float B)
{
    t = clamp(t, int2(0, 0), int2(renderWidth - 1, renderHeight - 1));
    const uint raw = b.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    return B / (d - A);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= renderWidth || id.y >= renderHeight)
        return;
    const int2 t = int2(id);
    const uint dst = (id.y * renderWidth + id.x) * 16;
    gJitterUv = JitterPixels(asfloat(sunBuf.Load2(1184)), p00, p11, float2(renderWidth, renderHeight)) / float2(renderWidth, renderHeight);
    if (flags & 2)
        return; // image générée : le résultat de la dernière image réelle reste

    // courant (couleur prémultipliée par le poids) et statistiques du voisinage 3×3 (moyenne ± γ σ), sur les
    // pixels dont le reflet est connu. Reflet attendu mais point reflété caché (vitre, feuillage, bord de
    // l'écran) : l'historique est gardé et s'estompe doucement au lieu de tomber à zéro d'une image à
    // l'autre (c'était le « pop / dépop » : visible, caché, visible…)
    bool known;
    const float4 cur = Current(t, known);
    float4 m1 = 0, m2 = 0;
    float count = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
            bool nk = known;
            const float4 n = (dx == 0 && dy == 0) ? cur : Current(t + int2(dx, dy), nk);
            if (nk)
                m1 += n, m2 += n * n, count += 1;
        }
    const float4 mean = count > 0 ? m1 / count : 0, sigma = count > 0 ? sqrt(max(m2 / count - mean * mean, 0.0)) : 0;
    const float4 lo = count > 0 ? mean - gamma * sigma : -1e9, hi = count > 0 ? mean + gamma * sigma : 1e9;

    float4 result = cur;
    bool done = false;
    if ((flags & 8) && anchorAlpha > 0)
    {
        const uint4 a = anchorCur.Load4(dst);
        const float2 pu = asfloat(a.xy);
        if (a.z != 0 && a.w != 0 && all(isfinite(pu)) && all(pu > 0) && all(pu < 1))
        {
            // bilinéaire sur les quatre pixels voisins, chacun gardé seulement s'il reflétait le même objet
            // dans le même miroir
            const float2 q = pu * float2(renderWidth, renderHeight) - 0.5;
            const int2 b = int2(floor(q));
            const float2 f = q - b;
            float4 hist = 0;
            float hw = 0;
            for (int j = 0; j < 4; j++)
            {
                const int2 s = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(renderWidth - 1, renderHeight - 1));
                const uint o = (uint(s.y) * renderWidth + uint(s.x)) * 16;
                const uint4 pa = anchorPrev.Load4(o);
                const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
                if (pa.z == a.z && pa.w == a.w)
                    hist += asfloat(history.Load4(o)) * bw, hw += bw;
            }
            if (hw > 0.25)
            {
                hist /= hw;
                const float4 alo = count > 0 ? mean - anchorGamma * sigma : -1e9, ahi = count > 0 ? mean + anchorGamma * sigma : 1e9;
                result = known ? lerp(clamp(hist, alo, ahi), cur, anchorAlpha) : clamp(hist, alo, ahi) * holdDecay;
                done = true;
                if (flags & 4)
                {
                    resolved.Store4(dst, asuint(float4(0, 1, 0, 1))); // diagnostic : vert = historique ancré
                    return;
                }
            }
        }
    }
    if (!done && (flags & 1) && motionConvention < 16)
    {
        const float3 law = asfloat(sunBuf.Load3(36));
        const uint raw = motion.Load(id.y * motionPitch + id.x * 4);
        float2 mv = float2(f16tof32(raw & 0xFFFF), f16tof32(raw >> 16));
        const uint k = motionConvention;
        mv = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / float2(renderWidth, renderHeight) : mv * 2.0;
        if (k & 4)
            mv = -mv;
        if (k & 8)
            mv.y = -mv.y;
        const float2 uv = (float2(id) + 0.5) / float2(renderWidth, renderHeight);
        const float w = ViewDepth(depth, t, law.x, law.y);
        // le reflet ne bouge pas comme la surface qui le porte : son point virtuel (derrière le miroir, à la
        // distance du rayon) bouge comme un point immobile à cette profondeur. Mouvement = part due à la
        // rotation de la caméra (mesurée par les cascades, la même à toute profondeur) + part due à la
        // translation, qui décroît comme 1 / profondeur.
        float2 mvRot = mv;
        if (asfloat(sunBuf.Load(624)) > 0 && w > 0)
        {
            const float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
            const float3 pc = float3(ndc.x * w / p00, ndc.y * w / p11, w);
            const float3 pr = float3(dot(asfloat(sunBuf.Load3(576)), pc), dot(asfloat(sunBuf.Load3(592)), pc), dot(asfloat(sunBuf.Load3(608)), pc));
            if (pr.z > 1e-3)
                mvRot = float2(pr.x * p00 / pr.z * 0.5 + 0.5, 0.5 - pr.y * p11 / pr.z * 0.5) - uv;
        }
        const float hitT = asfloat(reflection.Load((id.y * renderWidth + id.x) * 16 + 12));
        const float2 ndc0 = float2(uv.x * 2 - 1, 1 - uv.y * 2);
        const float dist = w > 0 ? length(float3(ndc0.x * w / p00, ndc0.y * w / p11, w)) : 1;
        const float ratio = hitT >= 1e4 ? 0.0 : dist / (dist + max(hitT, 0.0));
        const float2 mvVirtual = mvRot + (mv - mvRot) * ratio;
        const float2 prevUv = uv + mvVirtual + JitterPixels(asfloat(sunBuf.Load2(628)), p00, p11, float2(renderWidth, renderHeight)) / float2(renderWidth, renderHeight);
        if (law.z > 0 && all(isfinite(mvVirtual)) && all(prevUv > 0) && all(prevUv < 1) && w > 0)
        {
            const int2 p = int2(prevUv * float2(renderWidth, renderHeight));
            const float wp = ViewDepth(depthPrev, p, law.x, law.y);
            if (wp > 0 && abs(wp - w) < depthTol * w + 0.2)
            {
                const float4 hist = asfloat(history.Load4((uint(p.y) * renderWidth + uint(p.x)) * 16));
                // historique borné par le voisinage courant (un reflet qui a bougé ne laisse pas de trace) ;
                // mélange adaptatif : beaucoup d'historique à l'arrêt, peu en mouvement rapide
                const float motionPx = length(mvVirtual * float2(renderWidth, renderHeight));
                const float a = lerp(alpha, alphaMax, saturate(motionPx / 8.0));
                result = known ? lerp(clamp(hist, lo, hi), cur, a) : clamp(hist, lo, hi) * holdDecay;
                if (flags & 4)
                {
                    // confiance : part de l'historique gardée après bornage (blanc : stable, noir : rejeté)
                    const float kept = 1.0 - saturate(length(clamp(hist, lo, hi) - hist) / max(length(hist), 1e-3));
                    resolved.Store4(dst, asuint(float4(kept.xxx, 1)));
                    return;
                }
            }
        }
    }
    if (flags & 4)
        result = float4(0, 0, 0, 1); // pas d'historique repris
    resolved.Store4(dst, asuint(result));
}
