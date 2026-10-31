// URTI en direct : masque d'ombres à partir de la sortie de SIGMA (débruiteur d'ombres de NVIDIA) :
// visibilité débruitée (SIGMA_BackEnd_UnpackShadow : carré de la valeur stockée), puis les corrections
// d'URTI (ombre du jeu, N·L, brume au loin) sous forme de poids, écrites par live_shadow en mode SIGMA.

ByteAddressBuffer sigma : register(t0);   // OUT_SHADOW_TRANSLUCENCY recopiée (R8, lignes alignées)
ByteAddressBuffer weight : register(t1);  // part de l'ombre gardée (float, pas : shadowPitch)
RWByteAddressBuffer shadow : register(u0); // 1 éclairé, 0 à l'ombre (float, pas : shadowPitch)

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint sigmaPitch;
    uint shadowPitch;
    uint debugMode; // 1 : visibilité de SIGMA seule, sans les corrections
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
