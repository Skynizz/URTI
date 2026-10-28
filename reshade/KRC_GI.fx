/*
 * KRC GI : éclairage global à l'écran pour ReShade (projet KRC, F:\NewrAYTRACING).
 *
 * Ce qui est fait :
 *  - tracé par masque de visibilité (Therrien, Levesque, Gilet, « Screen Space Indirect Lighting
 *    with Visibility Bitmask », 2023), en demi-résolution : chaque point touché dans la profondeur
 *    masque un intervalle d'angles (épaisseur supposée) et les secteurs nouvellement masqués
 *    reçoivent sa lumière ; la lumière de la frame précédente donne plusieurs rebonds ;
 *  - mode KRC : le ciel vu à l'écran éclaire les directions restées libres (à l'infini, la
 *    direction depuis le point est celle de la caméra, dans le plan de la tranche) ;
 *  - accumulation dans le temps avec rejet par la profondeur, filtre à-trous, remontée en pleine
 *    résolution guidée par la profondeur.
 *
 * Touche F8 : effet coupé / actif, en direct. Écran partagé : gauche sans, droite avec.
 * Profondeur : réglages globaux de ReShade (RESHADE_DEPTH_INPUT_IS_REVERSED, etc.). Si l'écran
 * reste plat, vérifier dans l'onglet « Add-ons > Generic Depth » que la bonne profondeur est prise
 * et que l'anticrénelage MSAA du jeu est coupé.
 */

#include "ReShade.fxh"

// ------------------------------------------------------------------------------------------------
// Menu
// ------------------------------------------------------------------------------------------------

uniform int KrcMode <
    ui_type = "combo";
    ui_label = "Mode";
    ui_items = "Coupé\0Classique (écran seul)\0KRC (écran + ciel vu)\0";
    ui_tooltip = "Classique : comme les effets d'éclairage global à l'écran habituels.\nKRC : le ciel visible à l'écran éclaire aussi les surfaces qui le voient.";
> = 2;

uniform float KrcIntensity <
    ui_type = "slider"; ui_min = 0.0; ui_max = 4.0; ui_step = 0.01;
    ui_label = "Intensité de l'indirect";
> = 1.0;

uniform float KrcAO <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Ombres de contact";
    ui_tooltip = "Assombrit l'image là où peu de directions sont libres.";
> = 0.35;

uniform float KrcRadius <
    ui_type = "slider"; ui_min = 0.2; ui_max = 20.0; ui_step = 0.1;
    ui_label = "Rayon (mètres environ)";
> = 3.0;

uniform float KrcThickness <
    ui_type = "slider"; ui_min = 0.05; ui_max = 3.0; ui_step = 0.01;
    ui_label = "Épaisseur supposée des objets";
> = 0.5;

uniform int KrcSlices <
    ui_type = "slider"; ui_min = 1; ui_max = 4;
    ui_label = "Qualité : directions par pixel";
> = 2;

uniform int KrcSteps <
    ui_type = "slider"; ui_min = 4; ui_max = 24;
    ui_label = "Qualité : pas par direction";
> = 10;

uniform int KrcHistory <
    ui_type = "slider"; ui_min = 1; ui_max = 32;
    ui_label = "Lissage dans le temps (frames)";
    ui_tooltip = "Plus haut : moins de bruit, plus de traînée quand la caméra bouge.";
> = 12;

uniform float KrcFov <
    ui_type = "slider"; ui_min = 30.0; ui_max = 120.0; ui_step = 0.5;
    ui_label = "Champ de vision vertical du jeu (degrés)";
> = 55.0;

uniform float KrcHdr <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Reconstruction des hautes lumières";
    ui_tooltip = "L'image du jeu est déjà compressée : rend aux zones claires (ciel, soleil) un peu de leur force.";
> = 0.6;

uniform float KrcSaturation <
    ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
    ui_label = "Couleur des rebonds";
> = 1.0;

uniform bool KrcSplit <
    ui_label = "Écran partagé (gauche sans, droite avec)";
> = false;

uniform int KrcDebug <
    ui_type = "combo";
    ui_label = "Vue de contrôle";
    ui_items = "Image\0Indirect seul\0Ombres de contact\0Normales\0Profondeur\0";
> = 0;

uniform bool KrcToggle < source = "key"; keycode = 0x77; mode = "toggle"; >; // F8
uniform int KrcFrame < source = "framecount"; >;

// ------------------------------------------------------------------------------------------------
// Textures
// ------------------------------------------------------------------------------------------------

#define KRC_W (BUFFER_WIDTH / 2)
#define KRC_H (BUFFER_HEIGHT / 2)
#define KRC_PI 3.14159265

texture texKrcRaw       { Width = KRC_W; Height = KRC_H; Format = RGBA16F; };
texture texKrcHist      { Width = KRC_W; Height = KRC_H; Format = RGBA16F; };
texture texKrcHistPrev  { Width = KRC_W; Height = KRC_H; Format = RGBA16F; };
texture texKrcFiltA     { Width = KRC_W; Height = KRC_H; Format = RGBA16F; };
texture texKrcFiltB     { Width = KRC_W; Height = KRC_H; Format = RGBA16F; };
texture texKrcPrevRad   { Width = KRC_W; Height = KRC_H; Format = RGBA16F; };
texture texKrcPrevDepth { Width = KRC_W; Height = KRC_H; Format = R32F; };

#define KRC_POINT MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP
sampler sKrcRaw       { Texture = texKrcRaw;       KRC_POINT; };
sampler sKrcHist      { Texture = texKrcHist;      KRC_POINT; };
sampler sKrcHistPrev  { Texture = texKrcHistPrev;  KRC_POINT; };
sampler sKrcFiltA     { Texture = texKrcFiltA;     KRC_POINT; };
sampler sKrcFiltB     { Texture = texKrcFiltB;     KRC_POINT; };
sampler sKrcFiltBLin  { Texture = texKrcFiltB;     AddressU = CLAMP; AddressV = CLAMP; };
sampler sKrcPrevRad   { Texture = texKrcPrevRad;   AddressU = CLAMP; AddressV = CLAMP; };
sampler sKrcPrevDepth { Texture = texKrcPrevDepth; KRC_POINT; };
sampler sKrcColor     { Texture = ReShade::BackBufferTex; SRGBTexture = true; };

// ------------------------------------------------------------------------------------------------
// Outils
// ------------------------------------------------------------------------------------------------

bool KrcActive()
{
    return KrcMode != 0 && !KrcToggle;
}

float KrcDepth(float2 uv)
{
    return ReShade::GetLinearizedDepth(uv);
}

bool KrcIsSky(float d)
{
    return d >= 0.999;
}

float2 KrcTan()
{
    const float ty = tan(radians(KrcFov) * 0.5);
    return float2(ty * BUFFER_WIDTH * BUFFER_RCP_HEIGHT, ty);
}

// position en espace vue (x droite, y haut, z devant), en mètres environ
float3 KrcViewPos(float2 uv, float d)
{
    const float z = d * RESHADE_DEPTH_LINEARIZATION_FAR_PLANE;
    return float3((uv.x * 2.0 - 1.0) * KrcTan().x, (1.0 - uv.y * 2.0) * KrcTan().y, 1.0) * z;
}

float3 KrcViewPosAt(float2 uv)
{
    return KrcViewPos(uv, KrcDepth(uv));
}

// normale déduite de la profondeur : la plus petite des différences de chaque côté (arêtes nettes)
float3 KrcNormal(float2 uv, float3 P)
{
    const float2 px = BUFFER_PIXEL_SIZE;
    const float3 Pl = KrcViewPosAt(uv - float2(px.x, 0)), Pr = KrcViewPosAt(uv + float2(px.x, 0));
    const float3 Pu = KrcViewPosAt(uv - float2(0, px.y)), Pd = KrcViewPosAt(uv + float2(0, px.y));
    const float3 dx = abs(Pl.z - P.z) < abs(Pr.z - P.z) ? P - Pl : Pr - P;
    const float3 dy = abs(Pu.z - P.z) < abs(Pd.z - P.z) ? P - Pu : Pd - P;
    float3 n = normalize(cross(dx, dy));
    if (dot(n, -P) < 0.0)
        n = -n;
    return n;
}

// image du jeu (sRGB lue en linéaire) vers une radiance approchée : les hautes lumières compressées
// par le jeu retrouvent un peu de leur force
float3 KrcRadiance(float3 c)
{
    const float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float boost = 1.0 + KrcHdr * 3.0 * l * l * l;
    return c * boost;
}

float3 KrcRadianceFromScreen(float2 uv)
{
    return KrcRadiance(tex2Dlod(sKrcColor, float4(uv, 0, 0)).rgb);
}

uint KrcPopcount(uint v)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

// ∫ cos(x − n) |sin x| dx de n − π/2 à t (mesure de GTAO dans la tranche)
float KrcSliceMeasure(float t, float n)
{
    const float sn = sin(n);
    const float t0 = n - 0.5 * KRC_PI;
    const float gm0 = cos(2.0 * t0 - n) * 0.25 - t0 * sn * 0.5;
    const float below = (cos(2.0 * t - n) * 0.25 - t * sn * 0.5) - gm0;
    const float above = (cos(-n) * 0.25 - gm0) + (-cos(2.0 * t - n) * 0.25 + t * sn * 0.5) + cos(-n) * 0.25;
    return t <= 0.0 ? below : above;
}

uint KrcSectorMask(float ua, float ub)
{
    const uint a = uint(clamp(floor(saturate(ua) * 32.0), 0.0, 32.0));
    const uint b = uint(clamp(ceil(saturate(ub) * 32.0), 0.0, 32.0));
    const uint count = b > a ? b - a : 0u;
    const uint bits = count >= 32u ? 0xFFFFFFFFu : (((1u << count) - 1u) << a);
    return count == 0u ? 0u : bits;
}

float KrcNoise(float2 pixel, int frame)
{
    const float2 q = pixel + 5.588238 * float(uint(frame) & 63u);
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}

// ------------------------------------------------------------------------------------------------
// Passes
// ------------------------------------------------------------------------------------------------

float4 PS_Trace(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (!KrcActive())
        return float4(0, 0, 0, 1);
    const float d = KrcDepth(uv);
    if (KrcIsSky(d))
        return float4(0, 0, 0, 1);

    const float3 P = KrcViewPos(uv, d);
    const float3 V = normalize(-P);
    const float3 N = KrcNormal(uv, P);
    const float projScale = 0.5 * BUFFER_HEIGHT / KrcTan().y;
    const float radiusPixels = min(KrcRadius * projScale / P.z, 0.25 * BUFFER_HEIGHT);
    const float marchPixels = KrcMode == 2 ? max(radiusPixels, 0.35 * BUFFER_HEIGHT) : radiusPixels;
    const float noiseSlice = KrcNoise(pos.xy, KrcFrame);
    const float noiseStep = KrcNoise(pos.yx + 17.0, KrcFrame * 7 + 3);

    float3 gi = 0;
    float visibility = 0;
    float weightSum = 0;
    for (int s = 0; s < KrcSlices; s++)
    {
        const float phi = (float(s) + noiseSlice) / float(KrcSlices) * KRC_PI;
        const float2 dir = float2(cos(phi), sin(phi));
        float3 x = float3(dir.x, -dir.y, 0.0);
        x = normalize(x - V * dot(x, V));
        const float3 axis = cross(x, V);
        const float3 Np = N - axis * dot(N, axis);
        const float npLen = length(Np);
        const float n = clamp(atan2(dot(Np, x), dot(Np, V)), -0.5 * KRC_PI + 1e-3, 0.5 * KRC_PI - 1e-3);
        const float t0 = n - 0.5 * KRC_PI;
        const float t1 = n + 0.5 * KRC_PI;
        const float total = KrcSliceMeasure(t1, n);

        uint occluded = 0u;
        uint skyBits = 0u;
        float3 skyColor = 0;
        float skyCount = 0;
        float3 sliceGi = 0;
        for (int side = -1; side <= 1; side += 2)
        {
            float prevSkyU = -1.0;
            for (int i = 0; i < KrcSteps; i++)
            {
                const float f = (float(i) + noiseStep) / float(KrcSteps);
                const float offset = max(f * f * marchPixels, float(i + 1) * 2.0);
                const float2 quv = uv + float(side) * dir * offset * BUFFER_PIXEL_SIZE;
                if (any(quv < 0.0) || any(quv > 1.0))
                    break;
                const float dq = KrcDepth(quv);
                if (KrcIsSky(dq))
                {
                    if (KrcMode == 2)
                    {
                        const float3 D = normalize(KrcViewPos(quv, 1e-3));
                        const float u = KrcSliceMeasure(clamp(atan2(dot(D, x), dot(D, V)), t0, t1), n) / total;
                        skyBits |= prevSkyU >= 0.0 ? KrcSectorMask(min(u, prevSkyU), max(u, prevSkyU)) : KrcSectorMask(u - 1.0 / 64.0, u + 1.0 / 64.0);
                        skyColor += KrcRadianceFromScreen(quv);
                        skyCount += 1.0;
                        prevSkyU = u;
                    }
                    continue;
                }
                prevSkyU = -1.0;
                const float3 Pq = KrcViewPos(quv, dq);
                const float3 dv = Pq - P;
                const float dist = length(dv);
                if (dist > KrcRadius || dist < 1e-4)
                    continue;
                const float3 db = dv - V * KrcThickness;
                const float tf = clamp(atan2(dot(dv, x), dot(dv, V)), t0, t1);
                const float tb = clamp(atan2(dot(db, x), dot(db, V)), t0, t1);
                const uint mask = KrcSectorMask(KrcSliceMeasure(min(tf, tb), n) / total, KrcSliceMeasure(max(tf, tb), n) / total);
                const uint fresh = mask & ~occluded;
                if (fresh != 0u)
                {
                    // le point touché n'éclaire que s'il fait face (normale à deux prises, moins chère)
                    const float3 Pqr = KrcViewPosAt(quv + float2(BUFFER_RCP_WIDTH, 0));
                    const float3 Pqd = KrcViewPosAt(quv + float2(0, BUFFER_RCP_HEIGHT));
                    float3 Nq = normalize(cross(Pqr - Pq, Pqd - Pq));
                    if (dot(Nq, -Pq) < 0.0)
                        Nq = -Nq;
                    const float facing = saturate(dot(Nq, -dv / dist) * 4.0);
                    const float3 Lq = tex2Dlod(sKrcPrevRad, float4(quv, 0, 0)).rgb;
                    sliceGi += (float(KrcPopcount(fresh)) / 32.0) * facing * Lq;
                }
                occluded |= mask;
                if (occluded == 0xFFFFFFFFu)
                    break;
            }
        }
        if (skyCount > 0.0)
            sliceGi += (float(KrcPopcount(skyBits & ~occluded)) / 32.0) * (skyColor / skyCount);
        const float w = npLen * total;
        gi += w * sliceGi;
        visibility += w * (1.0 - float(KrcPopcount(occluded)) / 32.0);
        weightSum += w;
    }
    const float norm = 1.0 / float(KrcSlices);
    return float4(gi * norm, saturate(visibility * norm));
}

float4 PS_Temporal(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float4 current = tex2D(sKrcRaw, uv);
    const float d = KrcDepth(uv);
    const float prevD = tex2D(sKrcPrevDepth, uv).x;
    float4 history = tex2D(sKrcHistPrev, uv);

    // sans vecteurs de mouvement : historique gardé là où la profondeur n'a pas bougé, et ramené dans
    // l'enveloppe des voisins actuels (limite la traînée quand la caméra bouge)
    float frames = history.w;
    if (KrcIsSky(d) || abs(prevD - d) > 0.02 * d)
        frames = 0.0;
    float3 lo = current.rgb, hi = current.rgb;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            const float3 c = tex2Doffset(sKrcRaw, uv, int2(x, y)).rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }
    const float3 spread = (hi - lo) * 0.5;
    history.rgb = clamp(history.rgb, lo - spread, hi + spread);
    frames = min(frames + 1.0, float(KrcHistory));
    const float a = 1.0 / frames;
    // la visibilité se moyenne à part, sans compter les frames dans .w : on la reprend du courant
    return float4(lerp(history.rgb, current.rgb, a), frames);
}

float4 PS_CopyHistory(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(sKrcHist, uv);
}

float4 KrcAtrous(sampler src, float2 uv, float stepPixels)
{
    const float d = KrcDepth(uv);
    const float3 P = KrcViewPos(uv, d);
    const float3 N = KrcNormal(uv, P);
    const float kernel[3] = { 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };
    float3 sum = 0;
    float sumW = 0;
    const float2 texel = 2.0 * BUFFER_PIXEL_SIZE * stepPixels;
    for (int y = -2; y <= 2; y++)
    {
        for (int x = -2; x <= 2; x++)
        {
            const float2 quv = uv + float2(x, y) * texel;
            const float dq = KrcDepth(quv);
            if (KrcIsSky(dq))
                continue;
            const float3 Pq = KrcViewPos(quv, dq);
            const float wPlane = exp(-abs(dot(N, Pq - P)) / (0.02 * P.z));
            const float w = kernel[abs(x)] * kernel[abs(y)] * wPlane;
            sum += w * tex2Dlod(src, float4(quv, 0, 0)).rgb;
            sumW += w;
        }
    }
    const float4 center = tex2D(src, uv);
    return KrcIsSky(d) ? float4(0, 0, 0, 0) : float4(sumW > 0.0 ? sum / sumW : center.rgb, center.a);
}

float4 PS_FilterA(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return KrcAtrous(sKrcHist, uv, 1.0);
}

float4 PS_FilterB(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float4 r = KrcAtrous(sKrcFiltA, uv, 2.0);
    return float4(r.rgb, tex2D(sKrcRaw, uv).a); // visibilité de la frame, pour les ombres de contact
}

float3 KrcAlbedo(float3 c)
{
    // l'image du jeu contient déjà la lumière : on n'en garde que la teinte, à clarté moyenne
    const float m = max(max(c.r, c.g), max(c.b, 1e-4));
    const float3 hue = c / m;
    const float l = dot(hue, float3(0.2126, 0.7152, 0.0722));
    return lerp(l.xxx, hue, KrcSaturation) * 0.6;
}

float4 PS_Composite(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float3 color = tex2D(sKrcColor, uv).rgb;
    if (!KrcActive() || (KrcSplit && uv.x < 0.5))
        return float4(color, 1.0);
    const float d = KrcDepth(uv);
    if (KrcIsSky(d))
        return float4(KrcDebug == 0 ? color : float3(0, 0, 0), 1.0);

    // remontée en pleine résolution guidée par la profondeur (4 voisins de la demi-résolution)
    const float2 halfTexel = 2.0 * BUFFER_PIXEL_SIZE;
    const float2 base = (floor(uv / halfTexel - 0.5) + 0.5) * halfTexel;
    float4 sum = 0;
    float sumW = 0;
    for (int k = 0; k < 4; k++)
    {
        const float2 o = float2(k & 1, k >> 1);
        const float2 quv = base + o * halfTexel;
        const float2 bw = 1.0 - abs(uv - quv) / halfTexel;
        const float dq = KrcDepth(quv);
        const float w = max(bw.x * bw.y, 1e-3) * exp(-abs(dq - d) / (0.01 * d + 1e-6));
        sum += w * tex2Dlod(sKrcFiltB, float4(quv, 0, 0));
        sumW += w;
    }
    const float4 g = sum / max(sumW, 1e-6);

    const float3 P = KrcViewPos(uv, d);
    if (KrcDebug == 1)
        return float4(g.rgb, 1.0);
    if (KrcDebug == 2)
        return float4(g.aaa, 1.0);
    if (KrcDebug == 3)
        return float4(KrcNormal(uv, P) * 0.5 + 0.5, 1.0);
    if (KrcDebug == 4)
        return float4(saturate(d * 20.0).xxx, 1.0);

    const float ao = lerp(1.0, g.a, KrcAO);
    const float3 result = color * ao + KrcIntensity * KrcAlbedo(color) * g.rgb;
    return float4(result, 1.0);
}

// mémoire pour la frame suivante : radiance (plusieurs rebonds) et profondeur, en demi-résolution
float4 PS_StoreRadiance(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(KrcRadianceFromScreen(uv), 1.0);
}

float PS_StoreDepth(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return KrcDepth(uv);
}

technique KRC_GI <
    ui_label = "KRC GI (éclairage global)";
    ui_tooltip = "Éclairage indirect et ombres de contact calculés à l'écran.\nF8 : coupé / actif.";
>
{
    pass Trace          { VertexShader = PostProcessVS; PixelShader = PS_Trace;         RenderTarget = texKrcRaw; }
    pass Temporal       { VertexShader = PostProcessVS; PixelShader = PS_Temporal;      RenderTarget = texKrcHist; }
    pass CopyHistory    { VertexShader = PostProcessVS; PixelShader = PS_CopyHistory;   RenderTarget = texKrcHistPrev; }
    pass FilterA        { VertexShader = PostProcessVS; PixelShader = PS_FilterA;       RenderTarget = texKrcFiltA; }
    pass FilterB        { VertexShader = PostProcessVS; PixelShader = PS_FilterB;       RenderTarget = texKrcFiltB; }
    pass Composite      { VertexShader = PostProcessVS; PixelShader = PS_Composite;     SRGBWriteEnable = true; }
    pass StoreRadiance  { VertexShader = PostProcessVS; PixelShader = PS_StoreRadiance; RenderTarget = texKrcPrevRad; }
    pass StoreDepth     { VertexShader = PostProcessVS; PixelShader = PS_StoreDepth;    RenderTarget = texKrcPrevDepth; }
}
