// URTI en direct : application des ombres RT à l'image présentée, à chaque présentation (images du jeu
// et images générées par DLSS / FSR, qui ne passent pas par les effets de ReShade). L'image affichée est
// recopiée dans un tampon, décodée en lumière linéaire (sRGB, HDR10 PQ ou scRGB), assombrie là où le
// masque d'ombres (résolution de rendu, filtré 3×3) le demande, réencodée et recopiée.

ByteAddressBuffer image : register(t0);   // copie de l'image présentée (lignes alignées)
ByteAddressBuffer mask : register(t1);    // 1 éclairé, 0 ombre ajoutée (float, résolution de rendu)
RWByteAddressBuffer output : register(u0); // même disposition que image

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;
    uint format;      // 0 RGBA8, 1 BGRA8, 2 RGB10A2 en HDR10 (PQ), 3 RGBA16F (scRGB)
    uint maskWidth;
    uint maskHeight;
    uint maskPitch;
    float strength;   // assombrissement à l'ombre (0 : rien, 1 : noir)
    uint split;       // 1 : moitié gauche sans ombres RT, trait au centre
    uint showMask;    // 1 : masque seul
    uint pad0;
    uint pad;
};

float3 SrgbToLinear(float3 c) { return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4)); }
float3 LinearToSrgb(float3 c) { return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055); }
float3 PqToNits(float3 e)
{
    const float3 p = pow(saturate(e), 1.0 / 78.84375);
    return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
}
float3 NitsToPq(float3 l)
{
    const float3 y = pow(saturate(l / 10000.0), 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
float HalfToFloat(uint h) { return f16tof32(h); }

float MaskAt(int2 t)
{
    t = clamp(t, int2(0, 0), int2(maskWidth - 1, maskHeight - 1));
    return asfloat(mask.Load(t.y * maskPitch + t.x * 4));
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    // masque : bilinéaire à la résolution de rendu, adouci par une tente 3×3 (bords d'ombre moins durs)
    const float2 m = (float2(id) + 0.5) / float2(width, height) * float2(maskWidth, maskHeight) - 0.5;
    const int2 b = int2(floor(m));
    const float2 f = m - b;
    float lit = 0, wsum = 0;
    for (int dy = -1; dy <= 2; dy++)
        for (int dx = -1; dx <= 2; dx++)
        {
            const float wx = max(0.0, 1.5 - abs(dx - f.x)), wy = max(0.0, 1.5 - abs(dy - f.y));
            lit += MaskAt(b + int2(dx, dy)) * wx * wy;
            wsum += wx * wy;
        }
    lit /= wsum;

    const uint addr = id.y * pitch + id.x * (format == 3 ? 8 : 4);
    if (split != 0 && id.x == width / 2)
    {
        if (format == 3)
            output.Store2(addr, uint2(f32tof16(1.0) | (f32tof16(1.0) << 16), f32tof16(1.0) | (f32tof16(1.0) << 16)));
        else
            output.Store(addr, 0xFFFFFFFF);
        return;
    }
    float shade = 1.0 - strength * (1.0 - lit);
    if (split != 0 && id.x < width / 2)
        shade = 1.0;

    if (format == 3)
    {
        const uint2 raw = image.Load2(addr);
        float3 c = float3(HalfToFloat(raw.x & 0xFFFF), HalfToFloat(raw.x >> 16), HalfToFloat(raw.y & 0xFFFF));
        c = showMask != 0 ? lit.xxx : c * shade;
        output.Store2(addr, uint2(f32tof16(c.r) | (f32tof16(c.g) << 16), f32tof16(c.b) | (raw.y & 0xFFFF0000)));
        return;
    }
    const uint raw = image.Load(addr);
    if (format == 2)
    {
        float3 e = float3(raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023) / 1023.0;
        // l'ombre : 203 nits comme blanc de référence pour le masque seul
        float3 c = showMask != 0 ? NitsToPq(lit.xxx * 203.0) : NitsToPq(PqToNits(e) * shade);
        const uint3 q = uint3(round(saturate(c) * 1023.0));
        output.Store(addr, q.x | (q.y << 10) | (q.z << 20) | (raw & 0xC0000000));
        return;
    }
    float3 c = float3(raw & 255, (raw >> 8) & 255, (raw >> 16) & 255) / 255.0;
    if (format == 1)
        c = c.bgr;
    float3 linearColor = SrgbToLinear(c); // image 8 bits : encodée sRGB
    linearColor = showMask != 0 ? lit.xxx : linearColor * shade;
    c = LinearToSrgb(saturate(linearColor));
    if (format == 1)
        c = c.bgr;
    const uint3 q = uint3(round(c * 255.0));
    output.Store(addr, q.x | (q.y << 8) | (q.z << 16) | (raw & 0xFF000000));
}
