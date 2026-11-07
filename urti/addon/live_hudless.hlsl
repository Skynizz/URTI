// URTI en direct : image du jeu sans son interface. Horizon Forbidden West avec DLSS-G la donne à la génération d'images : une cible
// de la taille de l'écran en RGBA 16 bits flottants, couleurs déjà codées comme l'image d'écran (PQ), alpha = couverture de
// l'interface (HUD, menus). Copiée dans les listes du jeu à la fin de son écriture (urti_live.inl), elle est remise ici dans la
// disposition de l'image d'écran : les reflets, le cache de surfaces et la mesure de la lumière y prennent leurs couleurs (plus de
// HUD ni de menu dans l'eau), et son alpha devient le masque d'interface de live_composite (l'interface n'est plus assombrie).
// L'image d'écran vaut cette image multipliée par un rapport (environ 2,2 dans Horizon), appris sur les pixels immobiles hors interface
// (sommes ici, moyenne dans live_hudgain) : les couleurs remises dans la disposition de l'écran le sont avec ce rapport.
// Le menu de pause n'est pas dans cet alpha : un pixel immobile (le même dans la copie précédente) nettement différent de l'image
// finale est aussi de l'interface. Un pixel qui bouge n'est pas comparé (l'image finale n'est pas forcément de la même image du jeu).

ByteAddressBuffer hud : register(t0);      // RGBA 16 bits flottants, lignes de srcPitch octets
ByteAddressBuffer hudPrev : register(t1);  // copie précédente (même disposition)
ByteAddressBuffer finalImage : register(t2); // image finale de cette présentation (disposition de l'image d'écran)
RWByteAddressBuffer preUi : register(u0);  // même disposition que l'image d'écran
RWByteAddressBuffer uiMask : register(u1); // 1 par pixel d'interface (live_composite)
RWByteAddressBuffer gain : register(u2);   // 0 : rapport appris (float, 0 : pas encore) ; 16 + 4 × case : histogramme du rapport de cette image (live_hudgain)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint srcPitch;
    uint dstPitch;
    uint format;   // image d'écran : 0 RGBA8, 1 BGRA8, 2 RGB10A2 (HDR10), 3 RGBA16F
    float alphaUi; // au-delà : pixel d'interface
    uint prevValid;  // copie précédente utilisable
    float diffUi;    // pixel immobile : écart à l'image finale au-delà duquel c'est de l'interface (valeurs codées, 0 à 1)
};

float4 Hud(ByteAddressBuffer b, uint2 id)
{
    const uint2 raw = b.Load2(id.y * srcPitch + id.x * 8);
    const float4 c = float4(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF), f16tof32(raw.y >> 16));
    return all(isfinite(c)) ? c : 0;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 group : SV_GroupID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint2 raw = hud.Load2(id.y * srcPitch + id.x * 8);
    float4 c = float4(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF), f16tof32(raw.y >> 16));
    if (any(!isfinite(c)))
        c = 0;
    const float g0 = asfloat(gain.Load(0));
    const bool gainKnown = g0 > 0.25 && g0 < 8.0;
    const float3 hudRgb = c.rgb;
    c.rgb *= gainKnown ? g0 : 1.0;
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
    bool ui = c.a > alphaUi, mismatch = false;
    // pixel immobile (même valeur dans la copie précédente) : comparé à l'image finale (rapport appris), et compté pour le rapport
    const bool still = prevValid != 0 && format != 3 && all(abs(Hud(hudPrev, id).rgb - hudRgb) < 0.004);
    int ratioBin = -1;
    if (!ui && still)
    {
        const uint f = finalImage.Load(a);
        const float3 shown = format == 2 ? float3(f & 1023, (f >> 10) & 1023, (f >> 20) & 1023) / 1023.0
                                         : (format == 1 ? float3((f >> 16) & 255, (f >> 8) & 255, f & 255) : float3(f & 255, (f >> 8) & 255, (f >> 16) & 255)) / 255.0;
        // écart toléré proportionnel à la lumière : le rapport appris est une médiane, un peu faux dans les ombres et les hautes lumières
        if (gainKnown)
            mismatch = any(abs(shown - saturate(c.rgb)) > diffUi + 0.15 * max(shown, saturate(c.rgb)));
        const float hs = hudRgb.r + hudRgb.g + hudRgb.b, ss = shown.r + shown.g + shown.b;
        if ((id.x & 7) == 0 && (id.y & 7) == 0 && hs > 0.15 && hs < 1.2 && all(shown < 0.95) && ss > 0.01)
            ratioBin = clamp(int((log2(ss / hs) + 1.0) * 64.0), 0, 255);
    }
    // immobile et différent : de l'interface seulement à la deuxième conversion de suite (une herbe arrêtée un instant faisait clignoter
    // l'effet) ; bit 0 : interface, bits 8 à 15 : conversions de suite immobile et différent
    const uint at = (id.y * width + id.x) * 4;
    const uint streak = mismatch ? min(((uiMask.Load(at) >> 8) & 255) + 1, 255u) : 0;
    ui = ui || streak >= 2;
    uiMask.Store(at, (ui ? 1u : 0u) | (streak << 8));
    // part de l'interface (journal) : un pixel sur 64
    if ((id.x & 7) == 4 && (id.y & 7) == 4)
    {
        uint unused;
        gain.InterlockedAdd(16 + 256 * 4, 1, unused);
        if (ui)
            gain.InterlockedAdd(16 + 256 * 4 + 4, 1, unused);
    }
    if (ratioBin >= 0)
    {
        uint unused;
        gain.InterlockedAdd(16 + uint(ratioBin) * 4, 1, unused);
    }
}
