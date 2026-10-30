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
    uint pad;
};

static float gA, gB;

float ViewDepth(ByteAddressBuffer b, int2 t)
{
    t = clamp(t, int2(0, 0), int2(width - 1, height - 1));
    const uint r = b.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(r & 0xFFFFFF) / 16777215.0 : asfloat(r);
    const float w = gB / (d - gA);
    return (w > 0 && w < 1e6) ? w : -1;
}

float Raw(int2 t)
{
    t = clamp(t, int2(0, 0), int2(width - 1, height - 1));
    return asfloat(raw.Load(t.y * shadowPitch + t.x * 4));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.x, gB = law.y;
    const int2 t = int2(id);
    const float center = Raw(t);
    const float w = law.z > 0 ? ViewDepth(depth, t) : -1;
    float outValue = center;
    if (w > 0)
    {
        // flou 3×3 guidé par la profondeur, et statistiques du voisinage
        float sum = 0, wsum = 0, m1 = 0, m2 = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
            {
                const int2 n = t + int2(dx, dy);
                const float v = Raw(n);
                const float wn = ViewDepth(depth, n);
                const float g = (dx == 0 && dy == 0) ? 1.0 : (abs(dx) + abs(dy) == 1 ? 0.6 : 0.36);
                const float k = wn > 0 ? g * exp(-abs(wn - w) / (0.02 * w + 0.02)) : 0;
                sum += v * k, wsum += k;
                m1 += v, m2 += v * v;
            }
        const float cur = wsum > 0 ? sum / wsum : center;
        const float mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
        outValue = cur;
        float motionPixels = 0;
        bool used = false;
        if (motionConvention < 16)
        {
            const uint r = motion.Load(id.y * motionPitch + id.x * 4);
            float2 mv = float2(f16tof32(r & 0xFFFF), f16tof32(r >> 16));
            const uint k = motionConvention;
            mv = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / float2(width, height) : mv * 2.0;
            if (k & 4)
                mv = -mv;
            if (k & 8)
                mv.y = -mv.y;
            motionPixels = length(mv * float2(width, height));
            const float2 prevUv = (float2(id) + 0.5) / float2(width, height) + mv;
            if (all(isfinite(mv)) && all(prevUv > 0) && all(prevUv < 1))
            {
                const float2 q = prevUv * float2(width, height) - 0.5;
                const int2 b = int2(floor(q));
                const float2 f = q - b;
                float hist = 0, hw = 0;
                for (int j = 0; j < 4; j++)
                {
                    const int2 s = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(width - 1, height - 1));
                    const float wp = ViewDepth(depthPrev, s);
                    const float bw = ((j & 1) ? f.x : 1 - f.x) * ((j >> 1) ? f.y : 1 - f.y);
                    if (wp > 0 && abs(wp - w) < 0.04 * w + 0.05)
                        hist += asfloat(history.Load(s.y * shadowPitch + s.x * 4)) * bw, hw += bw;
                }
                if (hw > 0.25)
                {
                    hist = clamp(hist / hw, mean - gamma * sigma - 0.02, mean + gamma * sigma + 0.02);
                    const float alpha = lerp(alphaMin, alphaMax, saturate(motionPixels / 8.0));
                    outValue = lerp(hist, cur, alpha);
                    used = true;
                }
            }
        }
        if (debugMode == 1)
            outValue = center;
        else if (debugMode == 2)
            outValue = saturate(motionPixels / 20.0);
        else if (debugMode == 3)
            outValue = used ? 1.0 : 0.0;
    }
    result.Store(id.y * shadowPitch + id.x * 4, asuint(outValue));
}
