// URTI en direct : entrées des débruiteurs de NVIDIA (NRD) tirées de la profondeur du jeu : profondeur
// linéaire (IN_VIEWZ) et normale + rugosité encodées comme NRD l'attend (IN_NORMAL_ROUGHNESS,
// R10G10B10A2). L'espace « monde » donné à NRD est l'espace caméra de cette image (matrices identité).
// Écrit dans des tampons aux lignes alignées, recopiés ensuite dans les textures de NRD.

#include "NRD.hlsli"

ByteAddressBuffer depth : register(t0);   // profondeur du jeu (32 bits par texel, lignes alignées)
ByteAddressBuffer sunBuf : register(t1);  // octets 36 à 47 : loi de profondeur de cette image
RWByteAddressBuffer viewZOut : register(u0);  // float par pixel
RWByteAddressBuffer normalOut : register(u1); // R10G10B10A2 par pixel

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint outPitch;
    float A;
    float B;
    float p00;
    float p11;
    uint depth24;
    float skyViewZ; // au-delà de la portée du débruitage : NRD ignore ces pixels
    uint pad0;
    uint pad1;
};

static float gA, gB, gClear;

float3 PointAt(int2 t)
{
    if (t.x < 0 || t.y < 0 || t.x >= int(width) || t.y >= int(height))
        return float3(0, 0, -1);
    const uint raw = depth.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    const float w = gB / (d - gA);
    if (d == gClear || !(w > 0 && w < 1e6))
        return float3(0, 0, -1);
    const float2 ndc = float2((t.x + 0.5) / width * 2 - 1, 1 - (t.y + 0.5) / height * 2);
    return float3(ndc.x * w / p00, ndc.y * w / p11, w);
}

// normale tirée de la profondeur : côté le plus proche de chaque axe (bords d'objets)
float3 NormalAt(int2 t, float3 p)
{
    const float3 l = PointAt(t - int2(1, 0)), r = PointAt(t + int2(1, 0)), u = PointAt(t - int2(0, 1)), b = PointAt(t + int2(0, 1));
    const float3 dx = (r.z > 0 && (l.z <= 0 || abs(r.z - p.z) < abs(l.z - p.z))) ? r - p : p - l;
    const float3 dy = (b.z > 0 && (u.z <= 0 || abs(b.z - p.z) < abs(u.z - p.z))) ? b - p : p - u;
    const float3 n = normalize(cross(dx, dy));
    return dot(n, p) > 0 ? -n : n;
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    const float3 law = asfloat(sunBuf.Load3(36));
    gA = law.z > 0 ? law.x : A, gB = law.z > 0 ? law.y : B;
    gClear = gB > 0 ? 0.0 : 1.0;
    const float3 p = PointAt(int2(id));
    const float3 n = p.z > 0 ? NormalAt(int2(id), p) : float3(0, 0, -1);
    const float4 nr = saturate(NRD_FrontEnd_PackNormalAndRoughness(all(isfinite(n)) ? n : float3(0, 0, -1), 1.0, 0.0));
    const uint4 q = uint4(round(nr * float4(1023, 1023, 1023, 3)));
    const uint a = id.y * outPitch + id.x * 4;
    viewZOut.Store(a, asuint(p.z > 0 ? p.z : skyViewZ));
    normalOut.Store(a, q.x | (q.y << 10) | (q.z << 20) | (q.w << 30));
}
