// URTI live: smoothed colour of the water, used as the base under URTI's reflection. Horizon Forbidden West draws its own low-resolution
// reflection into the water: its blocks showed through URTI's sharper reflection (two reflections that do not match). Where the reflection
// matters, the composition replaces the game's water colour by this low-pass version, which keeps the colour of the water and removes the blocks.
// Mode 0: average of the water pixels of each 8x8 block of render pixels. Mode 1: blur over 7x7 blocks, weighted by how much water each block holds.

ByteAddressBuffer image : register(t0);    // game image, screen layout
ByteAddressBuffer resolved : register(t1); // float4 per render pixel: colour * weight, weight (live_reflresolve); weight > 0 on reflecting water
ByteAddressBuffer gathered : register(t2); // mode 1: result of mode 0
RWByteAddressBuffer dst : register(u0);    // float4 per block: colour, share of water

cbuffer Constants : register(b0)
{
    uint width; // screen
    uint height;
    uint pitch;
    uint format; // 0 RGBA8, 1 BGRA8, 2 HDR10 PQ, 3 RGBA16F
    uint renderWidth;
    uint renderHeight;
    uint lowWidth;
    uint lowHeight;
    uint mode;
    uint pad0;
    uint pad1;
    uint pad2;
};

float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}

float3 LoadLinear(int2 px)
{
    px = clamp(px, int2(0, 0), int2(width - 1, height - 1));
    const uint a = uint(px.y) * pitch + uint(px.x) * (format == 3 ? 8 : 4);
    if (format == 3)
    {
        const uint2 raw = image.Load2(a);
        return float3(f16tof32(raw.x & 0xFFFF), f16tof32(raw.x >> 16), f16tof32(raw.y & 0xFFFF));
    }
    const uint raw = image.Load(a);
    if (format == 2)
        return PqToNits(float3(raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023) / 1023.0) / 203.0;
    const float3 c = float3(raw & 255, (raw >> 8) & 255, (raw >> 16) & 255) / 255.0;
    return SrgbToLinear(format == 1 ? c.bgr : c);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= lowWidth || id.y >= lowHeight)
        return;
    const uint at = (id.y * lowWidth + id.x) * 16;
    if (mode == 0)
    {
        float3 sum = 0;
        float n = 0;
        for (uint j = 0; j < 4; j++)
            for (uint i = 0; i < 4; i++)
            {
                const uint2 t = id * 8 + uint2(i, j) * 2;
                if (t.x >= renderWidth || t.y >= renderHeight)
                    continue;
                if (asfloat(resolved.Load((t.y * renderWidth + t.x) * 16 + 12)) <= 0)
                    continue;
                const float3 c = LoadLinear(int2((float2(t) + 0.5) / float2(renderWidth, renderHeight) * float2(width, height)));
                if (all(isfinite(c)))
                    sum += min(c, 8.0), n += 1;
            }
        dst.Store4(at, asuint(n > 0 ? float4(sum / n, n / 16.0) : float4(0, 0, 0, 0)));
        return;
    }
    float3 sum = 0;
    float weight = 0, cover = 0, total = 0;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++)
        {
            const int2 t = clamp(int2(id) + int2(dx, dy), int2(0, 0), int2(lowWidth - 1, lowHeight - 1));
            const float4 v = asfloat(gathered.Load4((uint(t.y) * lowWidth + uint(t.x)) * 16));
            const float g = exp(-float(dx * dx + dy * dy) / 6.0);
            sum += v.rgb * v.a * g, weight += v.a * g, cover += v.a * g, total += g;
        }
    dst.Store4(at, asuint(weight > 1e-3 ? float4(sum / weight, cover / total) : float4(0, 0, 0, 0)));
}
