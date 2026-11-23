// Live URTI: law between the final image and the game's UI-free image (live_hudless). Some games retouch their image
// after handing it to frame generation: in PQ codes, per channel, log2(final / UI-free) = L + b × (log2(UI-free) + 1.75)
// (measured: ratio of 1.3 to 2.2 depending on the scene, slope b of about 0.15). Learned on one pixel in 64 (live_hudless): median of
// the ratio histogram of dark pixels, and of bright pixels, each at its mean code: two points, a line; running
// average; histograms reset. The median ignores UI pixels and those that moved, as long as they are a minority.
// Also: which of the last three copies matches this present's final image, and how well.

RWByteAddressBuffer gain : register(u0); // 0: ratio at the pivot exp2(L) (float); 4: UI share (log); 8: share of matching pixels;
                                         // 12: slope b; 16 + 4 × bin: histograms (128 dark bins, 128 bright); then the counts

static const uint kCounts = 16 + 256 * 4;
static const uint kLag = kCounts + 32;
static const uint kShown = kCounts + 36;   // number of the copy the last matching present showed, and the number of that present
static const uint kLead = kCounts + 44;    // lead of the last computation over this present's image, in game frames (float; live_composite)

cbuffer Constants : register(b0)
{
    uint minCount; // minimum pixels for a measurement
    float blend;   // share of the new measurement
    uint serialNew;   // number of the last copy
    uint presentIdx;  // number of this present
    uint layerSerial; // number of the last copy at the time of the last computation (the layer belongs to that game frame)
    float leadBias;   // correction (game frames)
    uint pad0;
    uint pad1;
};

groupshared uint counts[256];

// median (log2 of the ratio) of bins first to first + 127, interpolated within its bin
float Median(uint first, uint n)
{
    uint below = 0;
    for (uint b = 0; b < 128; b++)
    {
        const uint c = counts[first + b];
        if (c > 0 && (below + c) * 2 >= n)
            return (float(b) + saturate((float(n) * 0.5 - float(below)) / float(c))) / 64.0 - 0.5;
        below += c;
    }
    return 0;
}

// 256 threads: each reads and resets its bin; the first computes the medians in shared memory (a single thread in video memory
// waited for each read one after the other)
[numthreads(256, 1, 1)]
void main(uint k : SV_GroupIndex)
{
    counts[k] = gain.Load(16 + k * 4);
    gain.Store(16 + k * 4, 0);
    GroupMemoryBarrierWithGroupSync();
    if (k != 0)
        return;
    const uint4 c0 = gain.Load4(kCounts), c1 = gain.Load4(kCounts + 16);
    gain.Store4(kCounts, uint4(0, 0, 0, 0));
    gain.Store4(kCounts + 16, uint4(0, 0, 0, 0));
    // share of pixels taken for UI (log)
    gain.Store(4, asuint(c0.x ? float(c0.y) / float(c0.x) : 0.0));
    // copy that best matches this present's final image
    const uint m0 = c0.w, m1 = c1.x, m2 = c1.y;
    const uint best = max(m0, max(m1, m2));
    const float share = c0.z >= 1000 ? float(best) / float(c0.z) : 0.0;
    gain.Store(8, asuint(share));
    // still scene: every copy matches; the previous copy stays (the lag can only be measured in motion)
    const uint lagWas = min(gain.Load(kLag), 2u);
    const uint mWas = lagWas == 0 ? m0 : lagWas == 1 ? m1 : m2;
    const uint lag = mWas + c0.z / 200 >= best ? lagWas : best == m0 ? 0u : best == m1 ? 1u : 2u;
    gain.Store(kLag, lag);
    if (share >= 0.96)
        gain.Store2(kShown, uint2(serialNew - lag, presentIdx));
    // the layer's lead over the image shown; a present that does not match (image generated in between): half a game frame more
    // than the previous one
    const uint2 shown = gain.Load2(kShown);
    const uint age = presentIdx - shown.y;
    gain.Store(kLead, asuint(age <= 6 ? float(int(layerSerial - shown.x)) - 0.5 * float(age) + leadBias : 0.0));
    gain.Store(kLead + 4, layerSerial); // (live_composite: game frame number of the last computation)
    uint n0 = 0, n1 = 0;
    for (uint i = 0; i < 128; i++)
        n0 += counts[i], n1 += counts[128 + i];
    if (n0 + n1 < minCount)
        return;
    // two points (mean code, median ratio) in log2; a single group filled: the previous slope
    const float wasG = asfloat(gain.Load(0)), wasB = asfloat(gain.Load(12));
    const bool known = wasG > 0.25 && wasG < 8.0;
    const float bOld = known && abs(wasB) < 0.6 ? wasB : 0.0;
    const float x0 = n0 ? float(c1.z) / float(n0) / 256.0 - 8.0 : 0.0, x1 = n1 ? float(c1.w) / float(n1) / 256.0 - 8.0 : 0.0;
    float L, b;
    if (n0 >= 300 && n1 >= 300 && x1 - x0 > 0.3)
    {
        const float r0 = Median(0, n0), r1 = Median(128, n1);
        b = clamp((r1 - r0) / (x1 - x0), -0.5, 0.5);
        L = r0 + b * (-1.75 - x0);
    }
    else
    {
        const bool low = n0 >= n1;
        b = bOld;
        L = (low ? Median(0, n0) : Median(128, n1)) + b * (-1.75 - (low ? x0 : x1));
    }
    if (!(L > -1.5 && L < 2.5))
        return;
    if (known)
        L = lerp(log2(wasG), L, blend), b = lerp(bOld, b, blend);
    gain.Store(0, asuint(exp2(L)));
    gain.Store(12, asuint(b));
}
