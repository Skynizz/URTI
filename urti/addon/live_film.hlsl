// Live URTI: real-pace film (urti_film file). A screen image (screen-image layout) halved,
// average of four pixels in encoded values, stored at its place in the film's ring. Nothing is written to disk during the
// film: the images stay in video memory until the end (a burst, which writes every frame, slows the game down and keeps only
// computed frames).

ByteAddressBuffer src : register(t0);
RWByteAddressBuffer film : register(u0);

cbuffer Constants : register(b0)
{
    uint width;     // demi-largeur
    uint height;    // demi-hauteur
    uint srcPitch;
    uint dstOffset; // bytes, start of this image in the ring
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
