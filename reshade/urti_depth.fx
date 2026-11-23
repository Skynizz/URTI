// URTI: invisible effect that asks for the game's depth. ReShade's "Generic Depth" module then keeps
// a reliable copy of it every frame; the URTI add-on reads it.

#include "ReShade.fxh"

texture UrtiDepth : DEPTH;
sampler sUrtiDepth { Texture = UrtiDepth; };
texture UrtiDummy { Width = 1; Height = 1; Format = R8; };

float PS_UrtiDepth(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(sUrtiDepth, uv).x;
}

technique URTI_Depth < ui_label = "URTI (depth for the injector, invisible)"; >
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_UrtiDepth; RenderTarget = UrtiDummy; }
}
