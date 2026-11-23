// Live URTI: which screen snapshot is the image without the game's UI? URTI takes a snapshot of the screen
// at the first draw of each binding of the screen image (up to four per frame). On one pixel
// in sixteen, it counts those of each snapshot identical to this frame's final image, and those identical to the
// previous displayed image (a snapshot taken before the game writes its scene holds an old image).
// live_uiselect then chooses, on the GPU and in the same frame.

ByteAddressBuffer finalImage : register(t0); // this frame's final image, before the composition (aligned rows)
ByteAddressBuffer snap0 : register(t1);
ByteAddressBuffer snap1 : register(t2);
ByteAddressBuffer snap2 : register(t3);
ByteAddressBuffer snap3 : register(t4);
ByteAddressBuffer prevShown : register(t5);  // previous displayed image (after composition)
RWByteAddressBuffer counts : register(u0);   // uint: identical to the final image per snapshot (0 to 12), to the previous
                                             // displayed image (16 to 28), pixels compared (32)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint bytesPerPixel; // 4 ou 8
    uint count;         // snapshots of this frame
    uint pad0;
    uint pad1;
    uint pad2;
};

uint2 Pixel(ByteAddressBuffer b, uint a) { return bytesPerPixel == 8 ? b.Load2(a) : uint2(b.Load(a), 0); }

void Count(ByteAddressBuffer s, uint k, bool inside, uint a, uint2 f, uint2 p)
{
    const uint2 v = inside && k < count ? Pixel(s, a) : uint2(0, 0);
    const uint same = WaveActiveCountBits(inside && k < count && all(v == f));
    const uint old = WaveActiveCountBits(inside && k < count && all(v == p));
    if (WaveIsFirstLane())
    {
        uint unused;
        counts.InterlockedAdd(k * 4, same, unused);
        counts.InterlockedAdd(16 + k * 4, old, unused);
    }
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 px = id * 4;
    const bool inside = px.x < width && px.y < height;
    const uint a = px.y * pitch + px.x * bytesPerPixel;
    const uint2 f = inside ? Pixel(finalImage, a) : uint2(0, 0);
    const uint2 p = inside ? Pixel(prevShown, a) : uint2(0, 0);
    Count(snap0, 0, inside, a, f, p);
    Count(snap1, 1, inside, a, f, p);
    Count(snap2, 2, inside, a, f, p);
    Count(snap3, 3, inside, a, f, p);
    const uint total = WaveActiveCountBits(inside);
    if (WaveIsFirstLane())
    {
        uint unused;
        counts.InterlockedAdd(32, total, unused);
    }
}
