// Live URTI: the image before the game's UI, chosen on the GPU in the same frame (the order of the
// snapshots changes from one frame to the next when the game records its commands on several threads). Snapshot of
// this frame: more than 60 % identical to the final image, and closer to it than to the previous displayed
// image. Among those, the least identical: the earliest, the one with the least UI. Still
// image (player looking, menu open): every snapshot is as close to the previous image as to the
// current one, the rule no longer decides; the snapshot chosen on the previous frame is then kept. None (in
// RDR2: snapshot taken before the game writes its image, about one time in two): preUi keeps the last
// safe snapshot (live_composite then reuses the UI kept from that snapshot).

ByteAddressBuffer finalImage : register(t0);
ByteAddressBuffer snap0 : register(t1);
ByteAddressBuffer snap1 : register(t2);
ByteAddressBuffer snap2 : register(t3);
ByteAddressBuffer snap3 : register(t4);
RWByteAddressBuffer preUi : register(u0);   // same layout as the image
RWByteAddressBuffer counts : register(u1);  // live_uicompare (0 to 35); choice + 1 of even / odd frames (64, 68)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint bytesPerPixel;
    uint count;
    uint parity;       // even or odd frame: the previous frame's choice is in the other cell
    uint pad1;
    uint pad2;
};

int Choose()
{
    const float total = max(float(counts.Load(32)), 1.0);
    int best = -1;
    float bestSame = 2.0;
    for (uint k = 0; k < min(count, 4u); k++)
    {
        const float same = float(counts.Load(k * 4)) / total, old = float(counts.Load(16 + k * 4)) / total;
        if (same >= 0.6 && same > old && same < bestSame)
            best = int(k), bestSame = same;
    }
    const uint previous = counts.Load(64 + (1 - parity) * 4);
    if (best < 0 && previous > 0 && previous - 1 < min(count, 4u))
    {
        const uint k = previous - 1;
        const float same = float(counts.Load(k * 4)) / total, old = float(counts.Load(16 + k * 4)) / total;
        // still close enough to the final image (a menu may cover a large part of it), and not a copy of
        // the previous displayed image (snapshot taken too early: it would hold the old image and its menus)
        if (same >= 0.3 && (same >= old - 0.03 || old < 0.995))
            best = int(k);
    }
    return best;
}

uint2 Pixel(ByteAddressBuffer b, uint a) { return bytesPerPixel == 8 ? b.Load2(a) : uint2(b.Load(a), 0); }

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint a = id.y * pitch + id.x * bytesPerPixel;
    const int k = Choose();
    if (id.x == 0 && id.y == 0)
        counts.Store(64 + parity * 4, uint(k + 1));
    // no safe snapshot: the final image itself (keeping the last safe snapshot gave reflection colours
    // taken from an old image, v58-v62)
    const uint2 v = k == 0 ? Pixel(snap0, a) : k == 1 ? Pixel(snap1, a) : k == 2 ? Pixel(snap2, a) : k == 3 ? Pixel(snap3, a) : Pixel(finalImage, a);
    if (bytesPerPixel == 8)
        preUi.Store2(a, v);
    else
        preUi.Store(a, v.x);
}
