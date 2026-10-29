// URTI en direct : sommets captés (espace de découpage) vers l'espace caméra pour la structure
// d'accélération. Au-delà de ce que le compteur du flux a rempli : NaN (triangles inactifs pour DXR).

StructuredBuffer<float4> clip : register(t0);
ByteAddressBuffer counters : register(t1);
RWByteAddressBuffer view : register(u0); // float3 par sommet

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint vertexCount;   // taille de la structure (sommets)
    uint counterOffset; // octets : compteur du flux de l'écran
};

[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= vertexCount)
        return;
    const uint filled = counters.Load(counterOffset) / 16;
    float3 v = asfloat(0x7FC00000).xxx; // NaN
    if (id < filled)
    {
        const float4 c = clip[id];
        v = float3(c.x / p00, c.y / p11, c.w);
    }
    view.Store3(id * 12, asuint(v));
}
