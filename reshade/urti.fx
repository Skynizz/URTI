// URTI : effet qui demande la profondeur du jeu (pour l'add-on) et applique les ombres du soleil
// calculées en ray tracing par l'add-on URTI. Touche F7 (dans l'add-on) : ombres actives / coupées.

#include "ReShade.fxh"

uniform bool UrtiActive < hidden = true; > = false; // posé par l'add-on quand une image d'ombres est prête

uniform float UrtiStrength <
    ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
    ui_label = "Force des ombres ray tracing";
> = 0.55;

uniform bool UrtiSplit <
    ui_label = "Écran partagé (gauche : jeu seul, droite : avec les ombres RT)";
> = false;

uniform bool UrtiShowMask <
    ui_label = "Voir les ombres RT seules";
> = false;

texture UrtiDepth : DEPTH;
sampler sUrtiDepth { Texture = UrtiDepth; };
texture UrtiShadowTex : URTI_SHADOW;
sampler sUrtiShadow { Texture = UrtiShadowTex; };
texture UrtiDummy { Width = 1; Height = 1; Format = R8; };

float PS_UrtiDepth(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(sUrtiDepth, uv).x;
}

float4 PS_UrtiComposite(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float4 c = tex2D(ReShade::BackBuffer, uv);
    if (!UrtiActive)
        return c;
    const float lit = tex2D(sUrtiShadow, uv).x;
    if (UrtiShowMask)
        return float4(lit.xxx, 1.0);
    if (UrtiSplit)
    {
        if (abs(uv.x - 0.5) < 0.0008)
            return float4(1.0, 1.0, 1.0, 1.0); // trait de séparation
        if (uv.x < 0.5)
            return c;
    }
    return float4(c.rgb * lerp(1.0, 1.0 - UrtiStrength, 1.0 - lit), c.a);
}

technique URTI < ui_label = "URTI (ombres du soleil en ray tracing)"; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_UrtiDepth; RenderTarget = UrtiDummy; }
    pass { VertexShader = PostProcessVS; PixelShader = PS_UrtiComposite; }
}
