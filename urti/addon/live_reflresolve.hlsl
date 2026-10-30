// URTI en direct : reflets stabilisés dans le temps (comme les débruiteurs des jeux en RT). À la
// résolution de rendu : couleur du point reflété lue dans l'image présentée, plafonnée (pas de points
// blancs isolés), puis mélangée avec l'historique reprojeté par les vecteurs de mouvement du jeu ;
// l'historique est borné par le voisinage de l'image courante (pas de traînées) et rejeté si la
// profondeur ne correspond plus (point découvert).

ByteAddressBuffer image : register(t0);       // image présentée (lignes alignées)
ByteAddressBuffer reflection : register(t1);  // float4 par pixel de rendu : u, v, poids, lissé
ByteAddressBuffer history : register(t2);     // float4 par pixel de rendu : couleur linéaire, poids (image précédente)
ByteAddressBuffer motion : register(t3);      // vecteurs de mouvement du jeu (R16G16 flottants)
ByteAddressBuffer depth : register(t4);       // profondeur de rendu (cette image)
ByteAddressBuffer depthPrev : register(t5);   // profondeur de rendu (image précédente)
ByteAddressBuffer sunBuf : register(t6);      // loi de profondeur (octets 36 à 47)
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
    float alpha;         // part de l'image courante
    float maxLuminance;  // plafond de la couleur reflétée (lumière linéaire, 1 = blanc de référence)
    uint flags;          // 1 : historique valide ; 2 : image générée (pas de mise à jour) ; 4 : diagnostic, confiance
    uint pad0;
    uint pad1;
    uint pad2;
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

// reflet courant d'un pixel de rendu : couleur plafonnée × poids, poids
float4 Current(int2 t)
{
    t = clamp(t, int2(0, 0), int2(renderWidth - 1, renderHeight - 1));
    const float4 rf = asfloat(reflection.Load4((uint(t.y) * renderWidth + uint(t.x)) * 16));
    if (rf.z <= 0)
        return float4(0, 0, 0, 0);
    float3 c = SampleLinear(rf.xy);
    const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (lum > maxLuminance)
        c *= maxLuminance / lum;
    return float4(c * rf.z, rf.z);
}

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
    if (flags & 2)
        return; // image générée : le résultat de la dernière image réelle reste

    // courant (couleur prémultipliée par le poids) et boîte du voisinage 3×3
    const float4 cur = Current(t);
    float4 lo = cur, hi = cur;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx != 0 || dy != 0)
            {
                const float4 n = Current(t + int2(dx, dy));
                lo = min(lo, n), hi = max(hi, n);
            }

    float4 result = cur;
    if ((flags & 1) && motionConvention < 16)
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
        const float2 prevUv = (float2(id) + 0.5) / float2(renderWidth, renderHeight) + mv;
        const float w = ViewDepth(depth, t, law.x, law.y);
        if (law.z > 0 && all(isfinite(mv)) && all(prevUv > 0) && all(prevUv < 1) && w > 0)
        {
            const int2 p = int2(prevUv * float2(renderWidth, renderHeight));
            const float wp = ViewDepth(depthPrev, p, law.x, law.y);
            if (wp > 0 && abs(wp - w) < 0.04 * w + 0.05)
            {
                const float4 hist = asfloat(history.Load4((uint(p.y) * renderWidth + uint(p.x)) * 16));
                // historique borné par le voisinage courant : un reflet qui a bougé ne laisse pas de trace
                result = lerp(clamp(hist, lo, hi), cur, alpha);
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
