// Live URTI: denoising of occlusion, light bounce and RT shadows (in the spirit of NVIDIA's denoisers, ReBLUR / SIGMA).
// From this frame's raw mask:
//  - history reprojected by the game's motion vectors, rejected if the depth no longer matches (newly uncovered point);
//  - history length per pixel (frames accumulated since the last disocclusion): a newly uncovered pixel takes the true average of its first
//    samples (1/2, 1/3, 1/4...) instead of keeping its noisy first sample at 94 %; cap depending on motion;
//  - spatial blur guided by depth, history clamped statistically by the current neighbourhood (no trail).

ByteAddressBuffer raw : register(t0);       // this frame's raw mask (float, aligned rows); -1: not computed this frame
ByteAddressBuffer history : register(t1);   // previous frame's final mask
ByteAddressBuffer depth : register(t2);
ByteAddressBuffer depthPrev : register(t3);
ByteAddressBuffer motion : register(t4);    // the game's motion vectors (R16G16 floats)
ByteAddressBuffer sunBuf : register(t5);    // depth law (bytes 36 to 47)
ByteAddressBuffer lenPrev : register(t6);   // previous frame's history length (float, same rows as the mask)
RWByteAddressBuffer result : register(u0);  // masque final
RWByteAddressBuffer lenOut : register(u1);  // this frame's history length (bit 10 of blurRadius: not written)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint shadowPitch;
    uint motionPitch;
    uint motionConvention; // 255: no history
    uint depth24;
    uint debugMode;        // 1 raw, 2 motion vectors, 3 history kept, 4 history length
    float alphaMin;        // share of the current frame at rest (cap of the history length: 1 / alphaMin)
    float alphaMax;        // en mouvement rapide
    float gamma;           // clamp width (standard deviations)
    uint blurRadius;       // 1: 3×3 blur (sun shadows, sharp edges); 2: 5×5 (ambient occlusion only, soft);
                           // bit 8: pixel never computed -> 0 (otherwise 1, "lit"); bit 9: three planes (light
                           // bounce R, G, B, one after the other, each shadowPitch × height bytes); bit 10: length read, not written;
                           // bit 11: history length available (otherwise the old rule); bit 12: dual history (the length is stored
                           // with the fast history: integer part = length × 4, fractional part = fast history × 0.999)
    float p00;             // assumed projection (sub-pixel jitter in pixels)
    float p11;
    float farSkip;         // occlusion only: beyond this distance (occlusion range), nothing to denoise (0: no skip)
};

// the game's sub-pixel jitter (DLSS / TAA), measured by live_sunvote in URTI's camera space
// (shear x' = x + a w, y' = y + b w): in render pixels, a pixel's content is that of the point
// at "pixel - jitter"; frame-to-frame difference (previous - current) at 628, absolute at 1184
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

static uint gPlane; // bytes of one plane

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
    // one plane (shadows, occlusion) or three (light bounce): same weights for all three
    const uint planes = (blurRadius & 512) ? 3u : 1u;
    const float unknown = (blurRadius & 256) ? 0.0 : 1.0;
    const bool lenOn = (blurRadius & 2048) != 0;
    const int2 t = int2(id);
    float center[3], outValue[3];
    for (uint c = 0; c < 3; c++)
        center[c] = c < planes ? Raw(t, c) : 0.0, outValue[c] = center[c];
    float lenNew = 1.0;
    // dual history (bit 12, occlusion only): a fast history (2 to 3 frames, new share 0.5 at least) quickly follows what changes (an object
    // passing, motion); the slow history (up to 1 / alphaMin frames) is clamped around it, no longer around the current neighbourhood, too noisy
    // (wide clamp: trails; tight: flicker). The principle of NVIDIA's denoisers (ReBLUR's fast and slow histories)
    const bool packedLen = (blurRadius & 4096) != 0 && lenOn;
    const bool dual = packedLen && planes == 1;
    float fastNew = unknown;
    const float w = law.z > 0 ? ViewDepth(depth, t) : -1;
    // beyond the occlusion range: the ray pass writes "nothing" there (1, bounce 0); nothing to filter or accumulate
    if (w > 0 && farSkip > 0 && w > farSkip && (blurRadius & 255) == 2 && debugMode == 0)
    {
        for (uint c = 0; c < planes; c++)
            result.Store(c * gPlane + id.y * shadowPitch + id.x * 4, asuint(unknown));
        if (lenOn && (blurRadius & 1024) == 0)
            lenOut.Store(id.y * shadowPitch + id.x * 4, asuint(packedLen ? 4.0 + 0.999 * saturate(unknown) : 1.0));
        return;
    }
    if (w > 0)
    {
        // 1. reprojected history (before the blur: its length decides the blur's width)
        float motionPixels = 0;
        bool used = false;
        float hist[3] = { 0, 0, 0 };
        float lenH = 0, fastH = 0;
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
                        const float lp = lenOn ? asfloat(lenPrev.Load(o)) : 32.0;
                        lenH += (packedLen ? floor(lp) * 0.25 : lp) * bw;
                        fastH += (packedLen ? frac(lp) / 0.999 : 0.0) * bw;
                        hw += bw;
                    }
                }
                if (hw > 0.25)
                {
                    for (uint c = 0; c < planes; c++)
                        hist[c] /= hw;
                    lenH = isfinite(lenH) ? lenH / hw : 0.0;
                    fastH = isfinite(fastH) ? fastH / hw : 0.0;
                    used = true;
                }
            }
        }
        // history length: capped by motion (in fast motion, the history weighs less: no trail)
        const float aCap = lerp(alphaMin, alphaMax, saturate(motionPixels / 8.0));
        lenNew = used ? min(lenH + 1.0, 1.0 / aCap) : 1.0;

        // 2. blur guided by depth (3×3, or 5×5 for occlusion only; 9×9 while the history is short), and neighbourhood statistics
        float sum[3] = { 0, 0, 0 }, m1[3] = { 0, 0, 0 }, m2[3] = { 0, 0, 0 };
        float wsum = 0, taps = 0;
        const int r0 = int(clamp(blurRadius & 255, 1u, 2u));
        const int r = r0; // (a blur widened while the history is short cost 2 ms for a gain that could not be measured)
        // quarter-rate occlusion (one pixel per 2×2 block each frame): only pixels computed this frame count; walked
        // directly (one in four), same result
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
                    continue; // not computed this frame (quarter-rate occlusion)
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
            // share of the current frame: the true average of the samples while the history is short, then the floor depending on motion.
            // No sample here this frame: the history alone, its length does not count this frame
            const float alpha = hasCur ? max(1.0 / lenNew, aCap) : 0.0;
            if (!hasCur)
                lenNew = max(lenNew - 1.0, 1.0);
            // short history: wider clamp (the neighbourhood of a newly uncovered point is still noisy)
            // in motion, tighter clamp (half at 4 pixels per frame and more): the occlusion of a few frames ago no longer survives the current
            // neighbourhood (trails)
            const float g = gamma * (lenOn && lenNew < 4.0 ? 1.5 : 1.0) * lerp(1.0, 0.5, saturate(motionPixels / 4.0));
            for (uint c = 0; c < planes; c++)
                outValue[c] = lerp(clamp(hist[c], mean[c] - g * sigma[c] - 0.02, mean[c] + g * sigma[c] + 0.02), cur[c], alpha);
            if (dual)
            {
                // fast history: true average of its first frames, then at least half new
                fastNew = hasCur ? lerp(fastH, cur[0], max(1.0 / lenNew, 0.5)) : fastH;
                // slow history clamped around the fast one (window tightened in motion), then the usual new share
                const float gf = (0.5 * sigma[0] + 0.04) * lerp(1.0, 0.6, saturate(motionPixels / 4.0));
                outValue[0] = lerp(clamp(hist[0], fastNew - gf, fastNew + gf), cur[0], alpha);
            }
        }
        else if (dual)
            fastNew = outValue[0] >= 0 ? outValue[0] : unknown;
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
        result.Store(c * gPlane + id.y * shadowPitch + id.x * 4, asuint(outValue[c] < 0 ? unknown : outValue[c])); // never a "not computed" pixel
    if (lenOn && (blurRadius & 1024) == 0)
        lenOut.Store(id.y * shadowPitch + id.x * 4, asuint(packedLen ? floor(min(lenNew, 60.0) * 4.0) + 0.999 * saturate(dual ? fastNew : 1.0) : lenNew));
}
