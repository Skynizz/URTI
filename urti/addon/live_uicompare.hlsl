// URTI en direct : quelle photo de l'écran est l'image sans l'interface du jeu ? URTI photographie l'écran
// chaque fois que le jeu commence à y dessiner (jusqu'à quatre fois par image) ; on compte, sur un pixel
// sur seize, ceux de chaque photo identiques à l'image finale. La première photo presque identique est
// celle d'avant l'interface (menus, textes, mini-carte) : ses pixels différents sont l'interface.

ByteAddressBuffer finalImage : register(t0); // image présentée (lignes alignées)
ByteAddressBuffer snap0 : register(t1);
ByteAddressBuffer snap1 : register(t2);
ByteAddressBuffer snap2 : register(t3);
ByteAddressBuffer snap3 : register(t4);
RWByteAddressBuffer counts : register(u0);   // uint par photo : pixels identiques ; à 16 : pixels comparés

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

bool Same(ByteAddressBuffer b, uint a, uint2 f)
{
    return bytesPerPixel == 8 ? all(b.Load2(a) == f) : b.Load(a) == f.x;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 px = id * 4;
    const bool inside = px.x < width && px.y < height;
    const uint a = px.y * pitch + px.x * bytesPerPixel;
    uint2 f = 0;
    if (inside)
        f = bytesPerPixel == 8 ? finalImage.Load2(a) : uint2(finalImage.Load(a), 0);
    const bool s0 = inside && count > 0 && Same(snap0, a, f);
    const bool s1 = inside && count > 1 && Same(snap1, a, f);
    const bool s2 = inside && count > 2 && Same(snap2, a, f);
    const bool s3 = inside && count > 3 && Same(snap3, a, f);
    // une addition par vague (pas une par pixel : le même compteur pour tous)
    const uint n0 = WaveActiveCountBits(s0), n1 = WaveActiveCountBits(s1), n2 = WaveActiveCountBits(s2), n3 = WaveActiveCountBits(s3);
    const uint total = WaveActiveCountBits(inside);
    if (WaveIsFirstLane())
    {
        uint unused;
        counts.InterlockedAdd(0, n0, unused);
        counts.InterlockedAdd(4, n1, unused);
        counts.InterlockedAdd(8, n2, unused);
        counts.InterlockedAdd(12, n3, unused);
        counts.InterlockedAdd(16, total, unused);
    }
}
