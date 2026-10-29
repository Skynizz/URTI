// URTI : effet qui demande la profondeur du jeu à ReShade (pour l'add-on) et porte les réglages des
// ombres du soleil en ray tracing. L'add-on applique lui-même les ombres à chaque image présentée
// (images générées comprises). Touches : F7 ombres RT actives / coupées, F5 écran partagé.

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
texture UrtiDummy { Width = 1; Height = 1; Format = R8; };

float PS_UrtiDepth(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(sUrtiDepth, uv).x;
}

technique URTI < ui_label = "URTI (ombres du soleil en ray tracing)"; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_UrtiDepth; RenderTarget = UrtiDummy; }
}
