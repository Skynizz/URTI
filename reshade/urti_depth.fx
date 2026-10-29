// URTI : effet invisible qui demande la profondeur du jeu. Le module « Generic Depth » de ReShade en
// garde alors une copie fiable à chaque frame ; l'add-on URTI la lit pour son test de couverture.

#include "ReShade.fxh"

texture UrtiDepth : DEPTH;
sampler sUrtiDepth { Texture = UrtiDepth; };
texture UrtiDummy { Width = 1; Height = 1; Format = R8; };

float PS_UrtiDepth(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(sUrtiDepth, uv).x;
}

technique URTI_Depth < ui_label = "URTI (profondeur pour l'injecteur, invisible)"; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_UrtiDepth; RenderTarget = UrtiDummy; }
}
