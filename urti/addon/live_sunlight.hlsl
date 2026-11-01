// URTI en direct : part de la lumière directe du soleil dans l'image du jeu, mesurée à chaque image.
// Une ombre ne retire que la lumière du soleil : ce qui reste (ciel, rebonds) dépend de la scène (midi,
// soir, temps couvert) et de sa couleur. On la lit dans l'image elle-même : deux points d'une même surface,
// l'un que le jeu éclaire, l'autre qu'il ombre (sa carte d'ombres), ont pour rapport de couleur
// ciel / (ciel + soleil × N·L). D'où x = soleil / ciel = (éclairé / ombré - 1) / N·L, par canal et en
// luminance, accumulé dans un histogramme (log2 de x, quarts d'octave de 2^-5 à 2^5) : la médiane, lue sur
// le processeur, donne la part du soleil que les ombres RT retirent (live_shadow) et leur teinte
// (live_composite). Un fil par bloc de 4×4 pixels de rendu, un point du bloc tiré au hasard.

ByteAddressBuffer depth : register(t0);   // copie de la profondeur (32 bits par texel, lignes alignées)
ByteAddressBuffer atlas : register(t1);   // copie de la carte d'ombres du jeu
ByteAddressBuffer image : register(t2);   // image présentée (photo d'avant l'interface si elle existe)
RWByteAddressBuffer sunBuf : register(u0); // soleil, cascades, décalage sous-pixel ; histogramme à 1232

cbuffer Constants : register(b0)
{
    uint width;        // résolution de rendu (profondeur)
    uint height;
    uint depthPitch;
    uint depth24;
    float A;
    float B;
    float p00;
    float p11;
    float sunSign;     // +1 : la profondeur de la carte croît en s'éloignant du soleil
    float gameBias;
    uint atlasPitch;
    uint atlasFormat;  // 0 flottants 32 bits, 1 D24, 2 16 bits normalisés (RDR2)
    uint imageWidth;   // image présentée
    uint imageHeight;
    uint imagePitch;
    uint format;       // 0 RGBA8, 1 BGRA8, 2 RGB10A2 en HDR10 (PQ), 3 RGBA16F (scRGB)
    uint frameIndex;
    uint pad0;
    uint pad1;
    uint pad2;
};

static const uint kCount = 1232;  // nombre de paires mesurées
static const uint kHist = 1248;   // 4 × 40 cases : rouge, vert, bleu, luminance
static const uint kBins = 40;

static float gA, gB, gClear;

float2 JitterPixels(float2 ab, float2 size) { return float2(ab.x * p00, -ab.y * p11) * 0.5 * size; }

float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

// lumière linéaire de l'image présentée (même décodage que live_composite)
float3 LoadLinear(int2 px)
{
    px = clamp(px, int2(0, 0), int2(imageWidth - 1, imageHeight - 1));
    const uint a = uint(px.y) * imagePitch + uint(px.x) * (format == 3 ? 8 : 4);
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

// normale tirée de la profondeur (comme live_shadow)
float3 NormalAt(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(1, 0)), r = PointAt(t + int2(1, 0)), u = PointAt(t - int2(0, 1)), b = PointAt(t + int2(0, 1));
    const float3 dx = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
    const float3 dy = (b.z > 0 && (u.z <= 0 || abs(b.z - p.z) < abs(u.z - p.z))) ? b - p : p - u;
    float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n;
}

float AtlasDepth(uint2 t)
{
    if (atlasFormat == 2)
    {
        const uint a = t.y * atlasPitch + t.x * 2;
        const uint raw = atlas.Load(a & ~3u);
        return float((a & 2) ? raw >> 16 : raw & 0xFFFF) / 65535.0;
    }
    const uint raw = atlas.Load(t.y * atlasPitch + t.x * 4);
    return atlasFormat == 1 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
}

// état du point dans la carte d'ombres du jeu : 1 éclairé, 0 à l'ombre (tout le voisinage 5×5 dans le même
// état : loin des bords flous et de la pénombre), -1 bord ou hors des cascades
int GameState(float3 p)
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
        if (abs(s.x) > 0.95 || abs(s.y) > 0.95 || s.z <= 0 || s.z >= 1)
            continue;
        const float2 texel = vp.xy + float2(s.x * 0.5 + 0.5, 0.5 - s.y * 0.5) * vp.zw;
        uint lit = 0;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
            {
                const float stored = AtlasDepth(uint2(int2(texel) + int2(dx, dy)));
                lit += (sunSign > 0 ? s.z <= stored + gameBias : s.z >= stored - gameBias) ? 1u : 0u;
            }
        return lit == 25 ? 1 : lit == 0 ? 0 : -1;
    }
    return -1;
}

uint Hash(uint x)
{
    x ^= x >> 16, x *= 0x7feb352d, x ^= x >> 15, x *= 0x846ca68b, x ^= x >> 16;
    return x;
}

// couleur de l'image présentée au point vu par le pixel de rendu t (moyenne 2×2 : moins de bruit de texture)
float3 ColorAt(int2 t, float2 jitter)
{
    const float2 q = (float2(t) + 0.5 - jitter) / float2(width, height) * float2(imageWidth, imageHeight) - 0.5;
    const int2 b = int2(floor(q));
    return 0.25 * (LoadLinear(b) + LoadLinear(b + int2(1, 0)) + LoadLinear(b + int2(0, 1)) + LoadLinear(b + int2(1, 1)));
}

void Record(uint channel, float shadowed, float lit, float ndl)
{
    if (lit < 0.01)
        return; // trop sombre pour un rapport fiable
    const float x = (lit / max(shadowed, 1e-4) - 1.0) / ndl;
    const uint bin = x <= 0.03125 ? 0u : uint(clamp(floor((log2(x) + 5.0) * 4.0), 0.0, float(kBins - 1)));
    sunBuf.InterlockedAdd(kHist + (channel * kBins + bin) * 4, 1);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x * 4 >= width || id.y * 4 >= height)
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0;
    const float3 sun = asfloat(sunBuf.Load3(0));
    const uint h = Hash(id.x * 7919 + id.y * 104729 + frameIndex * 2654435761u);
    const int2 t = int2(id * 4) + int2(h & 3, (h >> 2) & 3);
    const float3 p = PointAt(t);
    // au loin, la brume du jeu se mêle à la couleur : pas de mesure
    if (p.z <= 0 || p.z > 100)
        return;
    const float3 n = NormalAt(t, p);
    const float ndl = dot(n, sun);
    if (ndl < 0.2)
        return; // surface rasante ou dos au soleil : le rapport y dit peu de chose
    const int state = GameState(p);
    if (state < 0)
        return;
    // partenaire : à 10 pixels, sur le même plan (même normale, à la même distance du plan), dans l'autre état
    static const int2 kDirs[4] = { int2(1, 0), int2(0, 1), int2(-1, 0), int2(0, -1) };
    for (uint k = 0; k < 4; k++)
    {
        const int2 u = t + kDirs[(k + (h >> 4)) & 3] * 10;
        const float3 q = PointAt(u);
        if (q.z <= 0 || abs(dot(n, q - p)) > 0.01 * p.z + 0.02 || dot(NormalAt(u, q), n) < 0.97)
            continue;
        const int other = GameState(q);
        if (other < 0 || other == state)
            continue;
        const float2 jitter = JitterPixels(asfloat(sunBuf.Load2(1184)), float2(width, height));
        const float3 cp = ColorAt(t, jitter), cq = ColorAt(u, jitter);
        const float3 lit = state == 1 ? cp : cq, shadowed = state == 1 ? cq : cp;
        const float3 kLum = float3(0.2126, 0.7152, 0.0722);
        for (uint c = 0; c < 3; c++)
            Record(c, shadowed[c], lit[c], ndl);
        Record(3, dot(shadowed, kLum), dot(lit, kLum), ndl);
        sunBuf.InterlockedAdd(kCount, 1);
        return;
    }
}
