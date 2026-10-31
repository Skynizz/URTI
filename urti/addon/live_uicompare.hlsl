// URTI en direct : quelle photo de l'écran est l'image sans l'interface du jeu ? URTI photographie l'écran
// au premier dessin de chaque liaison de l'image d'écran (jusqu'à quatre par image). On compte, sur un pixel
// sur seize, ceux de chaque photo identiques à l'image finale de cette image, et ceux identiques à l'image
// affichée précédente (une photo prise avant que le jeu écrive sa scène contient une ancienne image).
// live_uiselect choisit ensuite, sur le GPU et dans la même image.

ByteAddressBuffer finalImage : register(t0); // image finale de cette image, avant la composition (lignes alignées)
ByteAddressBuffer snap0 : register(t1);
ByteAddressBuffer snap1 : register(t2);
ByteAddressBuffer snap2 : register(t3);
ByteAddressBuffer snap3 : register(t4);
ByteAddressBuffer prevShown : register(t5);  // image affichée précédente (après composition)
RWByteAddressBuffer counts : register(u0);   // uint : identiques à l'image finale par photo (0 à 12), à l'image
                                             // affichée précédente (16 à 28), pixels comparés (32)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint bytesPerPixel; // 4 ou 8
    uint count;         // photos de cette image
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
