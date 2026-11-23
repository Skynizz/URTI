// Live URTI: the game's image without its UI. Some games with DLSS-G hand it to frame generation: a screen-sized
// RGBA 16-bit float target, colours already encoded like the screen image (PQ). Copied inside the game's lists at
// the end of its writing (urti_live.inl), it is put back here into the screen image's layout: reflections, the radiance
// cache and the light measurement take their colours from it (no more HUD or menu in the water).
// Law between the two (measured: the game retouches its image after handing it to frame generation): per channel,
// log2(final / UI-free) = L + b × (log2(UI-free) + 1.75), learned continuously (live_hudgain).
// UI mask: what, in the final image, differs from the UI-free image. The comparison holds only if both come from
// the same game frame: a present's final image is that of an older copy (the game is ahead), or an image
// generated between two (DLSS-G), which differs from all of them wherever things move. Three passes (mode):
//   1: conversion of the last copy (once per copy);
//   2: one pixel in 64 compared with the final image, for the last three copies: pixels that match each one, and
//       histograms of the ratio (dark and bright pixels apart: the law's slope);
//   4: if a copy matches (live_hudgain: share at byte 8, which one at byte kLag), mask of every pixel from it:
//       UI share from 0 to 1 depending on the difference. Otherwise the previous mask stays (the HUD does not move from one present to the next).
// (The target's alpha covers the UI on some screens only: it is added to the mask.)

ByteAddressBuffer hud : register(t0);      // RGBA 16-bit floats, rows of srcPitch bytes: the last copy
ByteAddressBuffer hud1 : register(t1);     // the copy before
ByteAddressBuffer finalImage : register(t2); // final image of this present (screen image layout)
ByteAddressBuffer hud2 : register(t3);     // the one before that
RWByteAddressBuffer preUi : register(u0);  // same layout as the screen image
RWByteAddressBuffer uiMask : register(u1); // per pixel: bit 0 UI, bits 8 to 15 UI share (live_composite), bits 16 to 23 the target's alpha
RWByteAddressBuffer gain : register(u2);   // 0: ratio at the pivot (float, 0: not yet); 8: share of matching pixels; 12: slope b;
                                           // 16 + 4 × bin: histograms of the ratio (128 dark bins, 128 bright); then the counts (kCounts)

static const uint kCounts = 16 + 256 * 4; // pixels counted, UI, compared, matching each copy (3), sums of log2(code) dark and bright
static const uint kLag = kCounts + 32;    // matching copy (0 the last, 1, 2)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint srcPitch;
    uint dstPitch;
    uint format;   // screen image: 0 RGBA8, 1 BGRA8, 2 RGB10A2 (HDR10), 3 RGBA16F
    float alphaUi; // beyond: UI pixel
    uint mode;     // bits 0 to 7: pass; bits 8 to 9: copies available - 1
    float diffUi;  // tolerated difference to the final image (encoded values, 0 to 1), on top of 7 % of the value
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

// final image expected for a colour of the UI-free image
float3 Predict(float3 c)
{
    c = max(c, 0.0);
    return saturate(c * exp2(gL + gSlope * (log2(max(c, 1e-3)) + 1.75)));
}

// difference beyond the tolerance, the worst of the three channels (<= 0: matches)
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
        // ratio: on the copy closest to the final image at this pixel, if it is not too far from it (UI, motion)
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
    // screen image in 16-bit floats: no comparison, the alpha alone
    if (format == 3)
        uiMask.Store((id.y * width + id.x) * 4, c.a > alphaUi ? (1u | (255u << 8)) : 0u);
}
