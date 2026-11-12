// URTI en direct : image du jeu sans son interface. Horizon Forbidden West avec DLSS-G la donne à la génération d'images : une cible
// de la taille de l'écran en RGBA 16 bits flottants, couleurs déjà codées comme l'image d'écran (PQ). Copiée dans les listes du jeu à
// la fin de son écriture (urti_live.inl), elle est remise ici dans la disposition de l'image d'écran : les reflets, le cache de
// radiance et la mesure de la lumière y prennent leurs couleurs (plus de HUD ni de menu dans l'eau).
// Loi entre les deux (mesurée : le jeu retouche son image après l'avoir remise à la génération d'images) : par canal,
// log2(final / sans interface) = L + b × (log2(sans interface) + 1,75), apprise en continu (live_hudgain).
// Masque d'interface : ce qui, dans l'image finale, diffère de l'image sans interface. La comparaison ne vaut que si les deux sont de
// la même image du jeu : l'image finale d'une présentation est celle d'une copie plus ancienne (le jeu a de l'avance), ou une image
// fabriquée entre deux (DLSS-G), qui diffère de toutes partout où ça bouge. Trois passes (mode) :
//   1 : conversion de la dernière copie (une fois par copie) ;
//   2 : un pixel sur 64 comparé à l'image finale, pour les trois dernières copies : pixels qui concordent avec chacune, et
//       histogrammes du rapport (pixels sombres et clairs à part : la pente de la loi) ;
//   4 : si une copie concorde (live_hudgain : part à l'octet 8, laquelle à l'octet kLag), masque de tous les pixels d'après elle :
//       part d'interface de 0 à 1 selon l'écart. Sinon le masque d'avant reste (le HUD ne bouge pas d'une présentation à l'autre).
// (L'alpha de la cible couvre l'interface dans certains écrans seulement : il s'ajoute au masque.)

ByteAddressBuffer hud : register(t0);      // RGBA 16 bits flottants, lignes de srcPitch octets : la dernière copie
ByteAddressBuffer hud1 : register(t1);     // la copie d'avant
ByteAddressBuffer finalImage : register(t2); // image finale de cette présentation (disposition de l'image d'écran)
ByteAddressBuffer hud2 : register(t3);     // encore avant
RWByteAddressBuffer preUi : register(u0);  // même disposition que l'image d'écran
RWByteAddressBuffer uiMask : register(u1); // par pixel : bit 0 interface, bits 8 à 15 part d'interface (live_composite), bits 16 à 23 alpha de la cible
RWByteAddressBuffer gain : register(u2);   // 0 : rapport au pivot (float, 0 : pas encore) ; 8 : part des pixels qui concordent ; 12 : pente b ;
                                           // 16 + 4 × case : histogrammes du rapport (128 cases sombres, 128 claires) ; puis les comptes (kCounts)

static const uint kCounts = 16 + 256 * 4; // pixels comptés, d'interface, comparés, concordants avec chaque copie (3), sommes de log2(code) sombres et claires
static const uint kLag = kCounts + 32;    // copie qui concorde (0 la dernière, 1, 2)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint srcPitch;
    uint dstPitch;
    uint format;   // image d'écran : 0 RGBA8, 1 BGRA8, 2 RGB10A2 (HDR10), 3 RGBA16F
    float alphaUi; // au-delà : pixel d'interface
    uint mode;     // bits 0 à 7 : passe ; bits 8 à 9 : copies disponibles - 1
    float diffUi;  // écart à l'image finale toléré (valeurs codées, 0 à 1), en plus de 7 % de la valeur
};

float4 Hud(ByteAddressBuffer b, uint2 id)
{
    const uint2 raw = b.Load2(id.y * srcPitch + id.x * 8);
    const float4 c = float4(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF), f16tof32(raw.y >> 16));
    return all(isfinite(c)) ? c : 0;
}

float3 Shown(uint2 id)
{
    const uint f = finalImage.Load(id.y * dstPitch + id.x * 4);
    return format == 2 ? float3(f & 1023, (f >> 10) & 1023, (f >> 20) & 1023) / 1023.0
                       : (format == 1 ? float3((f >> 16) & 255, (f >> 8) & 255, f & 255) : float3(f & 255, (f >> 8) & 255, (f >> 16) & 255)) / 255.0;
}

static float gL, gSlope;
static bool gLawKnown;

void LoadLaw()
{
    const float g0 = asfloat(gain.Load(0)), b = asfloat(gain.Load(12));
    gLawKnown = g0 > 0.25 && g0 < 8.0;
    gL = gLawKnown ? log2(g0) : 0.0;
    gSlope = gLawKnown && abs(b) < 0.6 ? b : 0.0;
}

// image finale attendue pour une couleur de l'image sans interface
float3 Predict(float3 c)
{
    c = max(c, 0.0);
    return saturate(c * exp2(gL + gSlope * (log2(max(c, 1e-3)) + 1.75)));
}

// écart au-delà du toléré, le pire des trois canaux (<= 0 : concorde)
float Excess(float3 shown, float3 pred)
{
    const float3 d = abs(shown - pred) - (diffUi + 0.07 * max(shown, pred));
    return max(d.r, max(d.g, d.b));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    LoadLaw();
    const uint pass = mode & 255, copies = ((mode >> 8) & 3) + 1;
    if (pass == 2)
    {
        const uint2 px = id * 8 + 4;
        if (px.x >= width || px.y >= height || format == 3)
            return;
        const float3 shown = Shown(px);
        uint unused;
        gain.InterlockedAdd(kCounts + 8, 1, unused);
        float4 best = 0;
        float bestErr = 1e9;
        for (uint l = 0; l < copies; l++)
        {
            float4 c;
            if (l == 0)
                c = Hud(hud, px);
            else if (l == 1)
                c = Hud(hud1, px);
            else
                c = Hud(hud2, px);
            const float3 pred = Predict(c.rgb);
            if (gLawKnown && Excess(shown, pred) <= 0)
                gain.InterlockedAdd(kCounts + 12 + l * 4, 1, unused);
            const float3 d = abs(shown - pred);
            const float err = d.r + d.g + d.b;
            if (err < bestErr)
                bestErr = err, best = c;
        }
        // part de l'interface (journal)
        gain.InterlockedAdd(kCounts, 1, unused);
        if (((uiMask.Load((px.y * width + px.x) * 4) >> 8) & 255) >= 128)
            gain.InterlockedAdd(kCounts + 4, 1, unused);
        // rapport : sur la copie la plus proche de l'image finale en ce pixel, s'il n'en est pas trop loin (interface, mouvement)
        const float hs = best.r + best.g + best.b, ss = shown.r + shown.g + shown.b;
        if (best.a <= alphaUi && hs > 0.15 && hs < 2.4 && all(shown < 0.95) && ss > 0.03 && (!gLawKnown || bestErr < 0.1 + 0.25 * ss))
        {
            const float m = hs / 3.0;
            const uint group = m < 0.3 ? 0u : 1u;
            gain.InterlockedAdd(16 + (group * 128 + uint(clamp(int((log2(ss / hs) + 0.5) * 64.0), 0, 127))) * 4, 1, unused);
            gain.InterlockedAdd(kCounts + 24 + group * 4, uint((log2(m) + 8.0) * 256.0), unused);
        }
        return;
    }
    if (id.x >= width || id.y >= height)
        return;
    if (pass == 4)
    {
        if (!(asfloat(gain.Load(8)) >= 0.96) || !gLawKnown || format == 3)
            return;
        const uint lag = gain.Load(kLag);
        float4 c;
        if (lag == 0)
            c = Hud(hud, id);
        else if (lag == 1)
            c = Hud(hud1, id);
        else
            c = Hud(hud2, id);
        float soft = saturate(Excess(Shown(id), Predict(c.rgb)) / 0.04);
        if (c.a > alphaUi)
            soft = 1.0;
        uiMask.Store((id.y * width + id.x) * 4, (soft >= 0.5 ? 1u : 0u) | (uint(soft * 255.0 + 0.5) << 8) | (uint(saturate(c.a) * 255.0 + 0.5) << 16));
        return;
    }
    float4 c = Hud(hud, id);
    if (gLawKnown)
        c.rgb = Predict(c.rgb);
    const uint a = id.y * dstPitch + id.x * (format == 3 ? 8 : 4);
    if (format == 3)
        preUi.Store2(a, uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (f32tof16(1.0) << 16)));
    else if (format == 2)
    {
        const uint3 q = uint3(saturate(c.rgb) * 1023.0 + 0.5);
        preUi.Store(a, q.r | (q.g << 10) | (q.b << 20) | (3u << 30));
    }
    else
    {
        const uint3 q = uint3(saturate(format == 1 ? c.bgr : c.rgb) * 255.0 + 0.5);
        preUi.Store(a, q.r | (q.g << 8) | (q.b << 16) | (255u << 24));
    }
    // image d'écran en 16 bits flottants : pas de comparaison, l'alpha seul
    if (format == 3)
        uiMask.Store((id.y * width + id.x) * 4, c.a > alphaUi ? (1u | (255u << 8)) : 0u);
}
