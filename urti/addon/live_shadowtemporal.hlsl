// URTI en direct : débruitage des ombres RT (comme les débruiteurs d'ombres des jeux en RT). À partir du
// masque brut de cette image : flou 3×3 guidé par la profondeur (pas de bavure sur les contours),
// historique reprojeté par les vecteurs de mouvement du jeu, borné statistiquement par le voisinage
// courant (moyenne ± γ écart-type : un objet qui bouge ne laisse pas de traînée), mélange adaptatif
// (beaucoup d'historique à l'arrêt, peu en mouvement), rejet si la profondeur ne correspond plus.

ByteAddressBuffer raw : register(t0);       // masque brut de cette image (float, lignes alignées)
ByteAddressBuffer history : register(t1);   // masque final de l'image précédente
ByteAddressBuffer depth : register(t2);
ByteAddressBuffer depthPrev : register(t3);
ByteAddressBuffer motion : register(t4);    // vecteurs de mouvement du jeu (R16G16 flottants)
ByteAddressBuffer sunBuf : register(t5);    // loi de profondeur (octets 36 à 47)
RWByteAddressBuffer result : register(u0);  // masque final

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint shadowPitch;
    uint motionPitch;
    uint motionConvention; // 255 : pas d'historique
    uint depth24;
    uint debugMode;        // 1 brut, 2 vecteurs de mouvement, 3 historique gardé
    float alphaMin;        // part de l'image courante à l'arrêt
    float alphaMax;        // en mouvement rapide
    float gamma;           // largeur du bornage (écarts-types)
    uint blurRadius;       // 1 : flou 3×3 (ombres du soleil, bords nets) ; 2 : 5×5 (occultation ambiante seule, douce) ;
                           // bit 8 : pixel jamais calculé -> 0 (sinon 1, « éclairé ») ; bit 9 : trois plans (rebond de
                           // lumière R, G, B, l'un après l'autre, chacun de shadowPitch × height octets)
    float p00;             // projection supposée (décalage sous-pixel en pixels)
    float p11;
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
    const int2 t = int2(id);
    float center[3], outValue[3];
    for (uint c = 0; c < 3; c++)
        center[c] = c < planes ? Raw(t, c) : 0.0, outValue[c] = center[c];
    const float w = law.z > 0 ? ViewDepth(depth, t) : -1;
    if (w > 0)
    {
        // flou guidé par la profondeur (3×3, ou 5×5 pour l'occultation seule), et statistiques du voisinage
        float sum[3] = { 0, 0, 0 }, m1[3] = { 0, 0, 0 }, m2[3] = { 0, 0, 0 };
        float wsum = 0, taps = 0;
        const int r = int(clamp(blurRadius & 255, 1u, 2u));
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
            {
                const int2 n = t + int2(dx, dy);
                const float v0 = Raw(n, 0);
                if (v0 < 0)
                    continue; // pas calculé à cette image (occultation au quart)
                const float wn = ViewDepth(depth, n);
                const float g = r == 1 ? ((dx == 0 && dy == 0) ? 1.0 : (abs(dx) + abs(dy) == 1 ? 0.6 : 0.36)) : exp(-float(dx * dx + dy * dy) / 4.0);
                const float k = wn > 0 ? g * exp(-abs(wn - w) / (0.02 * w + 0.02)) : 0;
                for (uint c = 0; c < planes; c++)
                {
                    const float v = c == 0 ? v0 : Raw(n, c);
                    sum[c] += v * k, m1[c] += v, m2[c] += v * v;
                }
                wsum += k, taps += 1;
            }
        const bool hasCur = wsum > 0;
        taps = max(taps, 1.0);
        float cur[3], mean[3], sigma[3];
        for (uint c = 0; c < planes; c++)
        {
            cur[c] = hasCur ? sum[c] / wsum : (center[c] >= 0 ? center[c] : unknown);
            mean[c] = hasCur ? m1[c] / taps : cur[c];
            sigma[c] = hasCur ? sqrt(max(m2[c] / taps - mean[c] * mean[c], 0.0)) : 1.0;
            outValue[c] = cur[c];
        }
        float motionPixels = 0;
        bool used = false;
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
                float hist[3] = { 0, 0, 0 };
                float hw = 0;
                for (int j = 0; j < 4; j++)
                {
                    const int2 s = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(width - 1, height - 1));
                    const float wp = ViewDepth(depthPrev, s);
                    const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
                    if (wp > 0 && abs(wp - w) < 0.04 * w + 0.05)
                    {
                        for (uint c = 0; c < planes; c++)
                            hist[c] += asfloat(history.Load(c * gPlane + s.y * shadowPitch + s.x * 4)) * bw;
                        hw += bw;
                    }
                }
                if (hw > 0.25)
                {
                    const float alpha = hasCur ? lerp(alphaMin, alphaMax, saturate(motionPixels / 8.0)) : 0.0;
                    for (uint c = 0; c < planes; c++)
                        outValue[c] = lerp(clamp(hist[c] / hw, mean[c] - gamma * sigma[c] - 0.02, mean[c] + gamma * sigma[c] + 0.02), cur[c], alpha);
                    used = true;
                }
            }
        }
        if (debugMode == 1)
            outValue[0] = center[0];
        else if (debugMode == 2)
            outValue[0] = saturate(motionPixels / 20.0);
        else if (debugMode == 3)
            outValue[0] = used ? 1.0 : 0.0;
    }
    for (uint c = 0; c < planes; c++)
        result.Store(c * gPlane + id.y * shadowPitch + id.x * 4, asuint(outValue[c] < 0 ? unknown : outValue[c])); // jamais un pixel « non calculé »
}
