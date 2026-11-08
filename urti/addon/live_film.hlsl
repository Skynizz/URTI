// URTI en direct : film au rythme réel (fichier urti_film). Une image d'écran (disposition de l'image d'écran) réduite de moitié,
// moyenne de quatre pixels dans les valeurs codées, rangée à sa place dans l'anneau du film. Rien n'est écrit sur le disque pendant le
// film : les images restent en mémoire vidéo jusqu'à la fin (une rafale, qui écrit chaque image, ralentit le jeu et ne garde que les
// images calculées).

ByteAddressBuffer src : register(t0);
RWByteAddressBuffer film : register(u0);

cbuffer Constants : register(b0)
{
    uint width;     // demi-largeur
    uint height;    // demi-hauteur
    uint srcPitch;
    uint dstOffset; // octets, début de cette image dans l'anneau
    uint format;    // 0 RGBA8, 1 BGRA8, 2 RGB10A2
    uint pad0;
    uint pad1;
    uint pad2;
};

uint4 Unpack(uint v)
{
    return format == 2 ? uint4(v & 1023, (v >> 10) & 1023, (v >> 20) & 1023, v >> 30) : uint4(v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint a = id.y * 2 * srcPitch + id.x * 8;
    const uint4 s = Unpack(src.Load(a)) + Unpack(src.Load(a + 4)) + Unpack(src.Load(a + srcPitch)) + Unpack(src.Load(a + srcPitch + 4));
    const uint4 m = (s + 2) / 4;
    const uint v = format == 2 ? (m.x | (m.y << 10) | (m.z << 20) | (min(m.w, 3u) << 30)) : (m.x | (m.y << 8) | (m.z << 16) | (m.w << 24));
    film.Store(dstOffset + (id.y * width + id.x) * 4, v);
}
