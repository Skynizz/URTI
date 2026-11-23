// Live URTI: layer queue. With DLSS-G, the displayed image lags one to two game frames behind the last computation:
// the layer of each computed frame (shadow, stabilised reflection, motion) is stored here, in one of three slices of a single buffer, with the
// number of its game frame; live_composite lays on each present the layer of the game frame it shows (live_hudgain knows it),
// instead of sliding the last layer by two frames of motion (hitches as soon as the game's pace varies).
// Buffer: 16-byte header per slice (game frame number, sub-pixel jitter x and y, 1 if filled), then from byte 64
// the slices; per render pixel 20 bytes: shadow (16-bit float), reflection (4 × 16 bits), motion (4 × 16 bits).

ByteAddressBuffer mask : register(t0);     // 1 lit, 0 shadow (float, rows of maskPitch bytes)
ByteAddressBuffer resolved : register(t1); // stabilised reflection: colour × weight, weight (float4)
ByteAddressBuffer motion : register(t2);   // motion of the surface and of the reflection (float4)
ByteAddressBuffer sunBuf : register(t3);   // this frame's sub-pixel jitter (bytes 1184 to 1191)
RWByteAddressBuffer ring : register(u0);

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint maskPitch;
    uint slice;
    uint serial;
    uint flags;   // 1: reflection available; 2: motion available
    uint pad0;
    uint pad1;
};

uint2 Pack(float4 v)
{
    v = all(isfinite(v)) ? v : 0;
    return uint2(f32tof16(v.x) | (f32tof16(v.y) << 16), f32tof16(v.z) | (f32tof16(v.w) << 16));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x == 0 && id.y == 0)
        ring.Store4(slice * 16, uint4(serial, sunBuf.Load2(1184), 1u));
    if (id.x >= width || id.y >= height)
        return;
    const uint src = (id.y * width + id.x) * 16;
    const uint dst = 64 + slice * (width * height * 20) + (id.y * width + id.x) * 20;
    const float m = asfloat(mask.Load(id.y * maskPitch + id.x * 4));
    ring.Store(dst, f32tof16(isfinite(m) ? m : 1.0));
    ring.Store2(dst + 4, (flags & 1) ? Pack(asfloat(resolved.Load4(src))) : uint2(0, 0));
    ring.Store2(dst + 12, (flags & 2) ? Pack(asfloat(motion.Load4(src))) : uint2(0, 0));
}
