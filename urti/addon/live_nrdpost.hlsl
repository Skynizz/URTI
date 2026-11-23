// Live URTI: shadow mask from SIGMA's output (NVIDIA's shadow denoiser):
// denoised visibility (SIGMA_BackEnd_UnpackShadow: square of the stored value), then URTI's
// corrections (game shadow, N·L, distant haze) as weights, written by live_shadow in SIGMA mode.

ByteAddressBuffer sigma : register(t0);   // OUT_SHADOW_TRANSLUCENCY copied (R8, aligned rows)
ByteAddressBuffer weight : register(t1);  // share of the shadow kept (float, pitch: shadowPitch)
RWByteAddressBuffer shadow : register(u0); // 1 lit, 0 in shadow (float, pitch: shadowPitch)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint sigmaPitch;
    uint shadowPitch;
    uint debugMode; // 1: SIGMA's visibility alone, without the corrections
    uint pad0;
    uint pad1;
    uint pad2;
};

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const uint a = id.y * sigmaPitch + id.x;
    const float stored = float((sigma.Load(a & ~3u) >> ((a & 3u) * 8)) & 255u) / 255.0;
    const float visibility = stored * stored;
    const float w = asfloat(weight.Load(id.y * shadowPitch + id.x * 4));
    const float lit = debugMode == 1 ? visibility : 1.0 - (1.0 - visibility) * saturate(w);
    shadow.Store(id.y * shadowPitch + id.x * 4, asuint(lit));
}
