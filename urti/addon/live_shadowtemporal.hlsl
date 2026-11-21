// URTI en direct : débruitage de l'occultation, du rebond de lumière et des ombres RT (dans l'esprit des débruiteurs de NVIDIA, ReBLUR / SIGMA).
// À partir du masque brut de cette image :
//  - historique reprojeté par les vecteurs de mouvement du jeu, rejeté si la profondeur ne correspond plus (point découvert) ;
//  - longueur d'historique par pixel (images accumulées depuis la dernière découverte) : un pixel découvert fait la vraie moyenne de ses premiers
//    échantillons (1/2, 1/3, 1/4...) au lieu de garder son premier échantillon bruité à 94 % ; plafond selon le mouvement ;
//  - flou spatial guidé par la profondeur, historique borné statistiquement par le voisinage courant (pas de traînée).

ByteAddressBuffer raw : register(t0);       // masque brut de cette image (float, lignes alignées) ; -1 : pas calculé à cette image
ByteAddressBuffer history : register(t1);   // masque final de l'image précédente
ByteAddressBuffer depth : register(t2);
ByteAddressBuffer depthPrev : register(t3);
ByteAddressBuffer motion : register(t4);    // vecteurs de mouvement du jeu (R16G16 flottants)
ByteAddressBuffer sunBuf : register(t5);    // loi de profondeur (octets 36 à 47)
ByteAddressBuffer lenPrev : register(t6);   // longueur d'historique de l'image précédente (float, mêmes lignes que le masque)
RWByteAddressBuffer result : register(u0);  // masque final
RWByteAddressBuffer lenOut : register(u1);  // longueur d'historique de cette image (bit 10 de blurRadius : pas écrite)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint shadowPitch;
    uint motionPitch;
    uint motionConvention; // 255 : pas d'historique
    uint depth24;
    uint debugMode;        // 1 brut, 2 vecteurs de mouvement, 3 historique gardé, 4 longueur d'historique
    float alphaMin;        // part de l'image courante à l'arrêt (plafond de la longueur d'historique : 1 / alphaMin)
    float alphaMax;        // en mouvement rapide
    float gamma;           // largeur du bornage (écarts-types)
    uint blurRadius;       // 1 : flou 3×3 (ombres du soleil, bords nets) ; 2 : 5×5 (occultation ambiante seule, douce) ;
                           // bit 8 : pixel jamais calculé -> 0 (sinon 1, « éclairé ») ; bit 9 : trois plans (rebond de
                           // lumière R, G, B, l'un après l'autre, chacun de shadowPitch × height octets) ; bit 10 : longueur lue, pas écrite ;
                           // bit 11 : longueur d'historique disponible (sinon l'ancienne règle)
    float p00;             // projection supposée (décalage sous-pixel en pixels)
    float p11;
    float farSkip;         // occultation seule : au-delà de cette distance (portée de l'occultation), rien à débruiter (0 : pas de saut)
};

// décalage sous-pixel du jeu (jitter, DLSS / TAA), mesuré par live_sunvote dans l'espace caméra d'URTI
// (cisaillement x' = x + a w, y' = y + b w) : en pixels de rendu, le contenu d'un pixel est celui du point
// situé à « pixel - jitter » ; écart d'une image à l'autre (précédente - courante) à 628, absolu à 1184
float2 JitterPixels(float2 ab, float p00_, float p11_, float2 size) { return float2(ab.x * p00_, -ab.y * p11_) * 0.5 * size; }

static float gA, gB;

float ViewDepth(ByteAddressBuffer b, int2 t)
{
    t = clamp(t, int2(0, 0), int2(width - 1, height - 1));
    const uint r = b.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(r & 0xFFFFFF) / 16777215.0 : asfloat(r);
    const float w = gB / (d - gA);
    return (w > 0 && w < 1e6) ? w : -1;
}

static uint gPlane; // octets d'un plan

float Raw(int2 t, uint k)
{
    t = clamp(t, int2(0, 0), int2(width - 1, height - 1));
    return asfloat(raw.Load(k * gPlane + t.y * shadowPitch + t.x * 4));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.x, gB = law.y;
    gPlane = shadowPitch * height;
    // un plan (ombres, occultation) ou trois (rebond de lumière) : mêmes poids pour les trois
    const uint planes = (blurRadius & 512) ? 3u : 1u;
    const float unknown = (blurRadius & 256) ? 0.0 : 1.0;
    const bool lenOn = (blurRadius & 2048) != 0;
    const int2 t = int2(id);
    float center[3], outValue[3];
    for (uint c = 0; c < 3; c++)
        center[c] = c < planes ? Raw(t, c) : 0.0, outValue[c] = center[c];
    float lenNew = 1.0;
    const float w = law.z > 0 ? ViewDepth(depth, t) : -1;
    // au-delà de la portée de l'occultation : la passe des rayons y écrit « rien » (1, rebond 0) ; rien à filtrer ni à accumuler
    if (w > 0 && farSkip > 0 && w > farSkip && (blurRadius & 255) == 2 && debugMode == 0)
    {
        for (uint c = 0; c < planes; c++)
            result.Store(c * gPlane + id.y * shadowPitch + id.x * 4, asuint(unknown));
        if (lenOn && (blurRadius & 1024) == 0)
            lenOut.Store(id.y * shadowPitch + id.x * 4, asuint(1.0));
        return;
    }
    if (w > 0)
    {
        // 1. historique reprojeté (avant le flou : sa longueur décide de la largeur du flou)
        float motionPixels = 0;
        bool used = false;
        float hist[3] = { 0, 0, 0 };
        float lenH = 0;
        if (motionConvention < 16)
        {
            const uint rm = motion.Load(id.y * motionPitch + id.x * 4);
            float2 mv = float2(f16tof32(rm & 0xFFFF), f16tof32(rm >> 16));
            const uint k = motionConvention;
            mv = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / float2(width, height) : mv * 2.0;
            if (k & 4)
                mv = -mv;
            if (k & 8)
                mv.y = -mv.y;
            motionPixels = length(mv * float2(width, height));
            const float2 jitterStep = JitterPixels(asfloat(sunBuf.Load2(628)), p00, p11, float2(width, height));
            const float2 prevUv = (float2(id) + 0.5 + jitterStep) / float2(width, height) + mv;
            if (all(isfinite(mv)) && all(prevUv > 0) && all(prevUv < 1))
            {
                const float2 q = prevUv * float2(width, height) - 0.5;
                const int2 b = int2(floor(q));
                const float2 f = q - b;
                float hw = 0;
                for (int j = 0; j < 4; j++)
                {
                    const int2 s = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(width - 1, height - 1));
                    const float wp = ViewDepth(depthPrev, s);
                    const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
                    if (wp > 0 && abs(wp - w) < 0.04 * w + 0.05)
                    {
                        const uint o = s.y * shadowPitch + s.x * 4;
                        for (uint c = 0; c < planes; c++)
                            hist[c] += asfloat(history.Load(c * gPlane + o)) * bw;
                        lenH += (lenOn ? asfloat(lenPrev.Load(o)) : 32.0) * bw;
                        hw += bw;
                    }
                }
                if (hw > 0.25)
                {
                    for (uint c = 0; c < planes; c++)
                        hist[c] /= hw;
                    lenH = isfinite(lenH) ? lenH / hw : 0.0;
                    used = true;
                }
            }
        }
        // longueur d'historique : plafonnée par le mouvement (en mouvement rapide, l'historique pèse moins : pas de traînée)
        const float aCap = lerp(alphaMin, alphaMax, saturate(motionPixels / 8.0));
        lenNew = used ? min(lenH + 1.0, 1.0 / aCap) : 1.0;

        // 2. flou guidé par la profondeur (3×3, ou 5×5 pour l'occultation seule ; 9×9 tant que l'historique est court), et statistiques du voisinage
        float sum[3] = { 0, 0, 0 }, m1[3] = { 0, 0, 0 }, m2[3] = { 0, 0, 0 };
        float wsum = 0, taps = 0;
        const int r0 = int(clamp(blurRadius & 255, 1u, 2u));
        const int r = r0; // (un flou élargi tant que l'historique est court coûtait 2 ms pour un gain non mesurable)
        // occultation au quart (un pixel par bloc 2×2 à chaque image) : seuls les pixels calculés à cette image comptent ; parcourus
        // directement (un sur quatre), même résultat
        int2 lattice = int2(-1, -1);
        if (r0 == 2)
        {
            const bool c0 = Raw(t, 0) >= 0, cx = Raw(t + int2(1, 0), 0) >= 0;
            if (!(c0 && cx))
            {
                if (c0)
                    lattice = t & 1;
                else if (cx)
                    lattice = (t + int2(1, 0)) & 1;
                else if (Raw(t + int2(0, 1), 0) >= 0)
                    lattice = (t + int2(0, 1)) & 1;
                else if (Raw(t + int2(1, 1), 0) >= 0)
                    lattice = (t + int2(1, 1)) & 1;
            }
        }
        const int stepL = lattice.x >= 0 ? 2 : 1;
        const int x0 = lattice.x >= 0 ? ((((lattice.x - (t.x & 1)) & 1) == 0) ? -r : -r + 1) : -r;
        const int y0 = lattice.y >= 0 ? ((((lattice.y - (t.y & 1)) & 1) == 0) ? -r : -r + 1) : -r;
        const float sigma2 = r == 1 ? 1.0 : float(r * r);
        for (int dy = y0; dy <= r; dy += stepL)
            for (int dx = x0; dx <= r; dx += stepL)
            {
                const int2 n = t + int2(dx, dy);
                const float v0 = Raw(n, 0);
                if (v0 < 0)
                    continue; // pas calculé à cette image (occultation au quart)
                const float wn = ViewDepth(depth, n);
                const float g = r == 1 ? ((dx == 0 && dy == 0) ? 1.0 : (abs(dx) + abs(dy) == 1 ? 0.6 : 0.36)) : exp(-float(dx * dx + dy * dy) / sigma2);
                const float k = wn > 0 ? g * exp(-abs(wn - w) / (0.02 * w + 0.02)) : 0;
                for (uint c = 0; c < planes; c++)
                {
                    const float v = c == 0 ? v0 : Raw(n, c);
                    sum[c] += v * k, m1[c] += v, m2[c] += v * v;
                }
                wsum += k, taps += 1;
            }
        const bool hasCur = wsum > 0;
        float cur[3], mean[3], sigma[3];
        for (uint c = 0; c < planes; c++)
        {
            cur[c] = hasCur ? sum[c] / wsum : (center[c] >= 0 ? center[c] : unknown);
            mean[c] = hasCur ? m1[c] / max(taps, 1.0) : cur[c];
            sigma[c] = hasCur ? sqrt(max(m2[c] / max(taps, 1.0) - mean[c] * mean[c], 0.0)) : 1.0;
            outValue[c] = cur[c];
        }
        if (used)
        {
            // part de l'image courante : la moyenne vraie des échantillons tant que l'historique est court, puis le plancher selon le mouvement.
            // Pas d'échantillon ici à cette image : l'historique seul, sa longueur ne compte pas cette image
            const float alpha = hasCur ? max(1.0 / lenNew, aCap) : 0.0;
            if (!hasCur)
                lenNew = max(lenNew - 1.0, 1.0);
            // historique court : bornage plus large (le voisinage d'un point découvert est encore bruité)
            // en mouvement, bornage resserré (moitié à 4 pixels par image et plus) : l'occultation d'il y a quelques images ne survit plus au voisinage
            // courant (traînées)
            const float g = gamma * (lenOn && lenNew < 4.0 ? 1.5 : 1.0) * lerp(1.0, 0.5, saturate(motionPixels / 4.0));
            for (uint c = 0; c < planes; c++)
                outValue[c] = lerp(clamp(hist[c], mean[c] - g * sigma[c] - 0.02, mean[c] + g * sigma[c] + 0.02), cur[c], alpha);
        }
        if (debugMode == 1)
            outValue[0] = center[0];
        else if (debugMode == 2)
            outValue[0] = saturate(motionPixels / 20.0);
        else if (debugMode == 3)
            outValue[0] = used ? 1.0 : 0.0;
        else if (debugMode == 4)
            outValue[0] = saturate(lenNew / 16.0);
    }
    for (uint c = 0; c < planes; c++)
        result.Store(c * gPlane + id.y * shadowPitch + id.x * 4, asuint(outValue[c] < 0 ? unknown : outValue[c])); // jamais un pixel « non calculé »
    if (lenOn && (blurRadius & 1024) == 0)
        lenOut.Store(id.y * shadowPitch + id.x * 4, asuint(lenNew));
}
