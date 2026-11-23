// Live URTI: flat transparent surfaces (water, glass) among the draws blended into the game's image. These
// draws (depth tested, not written, blended) are captured separately and converted into their own vertex buffer
// (layer 2 of the RT scene); one group per draw. A water surface or a window is flat: normals of its
// triangles aligned, vertices a few centimetres from the mean plane, of a certain extent, and not turned toward the
// camera like a particle (smoke, sparks: squares facing the camera, aligned too, but spread out in
// depth). The rest (smoke, particles, translucent foliage, decals) is made invisible to rays
// (NaN vertices: inactive triangles). The water's reflection then starts from the surface found by the camera ray
// (live_reflect).

StructuredBuffer<uint4> regions : register(t0); // per draw: start in the layer (bytes), size, counter, 0
ByteAddressBuffer counters : register(t1);      // bytes written per draw
RWByteAddressBuffer view : register(u0);        // float3 per vertex (layer of transparent surfaces)
RWByteAddressBuffer stats : register(u1);       // draws kept, triangles kept, draws received

cbuffer Constants : register(b0)
{
    uint regionCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

#define T 64
groupshared float3 gN[T], gC[T], gLo[T], gHi[T];
groupshared float gArea[T], gE[T];

float3 Vertex(uint i) { return asfloat(view.Load3(i * 12)); }

[numthreads(T, 1, 1)]
void main(uint3 group : SV_GroupID, uint ti : SV_GroupIndex)
{
    if (group.x >= regionCount)
        return;
    const uint4 r = regions[group.x];
    const uint first = r.x / 16, count = min(counters.Load(r.z * 8), r.y) / 16 / 3 * 3, total = r.y / 16;
    // triangle normals turned toward the camera (at the origin), weighted by their area; centre; box
    float3 n = 0, c = 0, lo = 1e30, hi = -1e30;
    float area = 0;
    for (uint t = ti; t < count / 3; t += T)
    {
        const float3 a = Vertex(first + t * 3), b = Vertex(first + t * 3 + 1), d = Vertex(first + t * 3 + 2);
        if (!all(isfinite(a + b + d)))
            continue;
        float3 m = cross(b - a, d - a);
        if (dot(m, a) > 0)
            m = -m;
        const float s = length(m);
        n += m, area += s, c += s * (a + b + d) / 3.0;
        lo = min(lo, min(a, min(b, d))), hi = max(hi, max(a, max(b, d)));
    }
    gN[ti] = n, gC[ti] = c, gArea[ti] = area, gLo[ti] = lo, gHi[ti] = hi;
    GroupMemoryBarrierWithGroupSync();
    for (uint s2 = T / 2; s2 > 0; s2 >>= 1)
    {
        if (ti < s2)
            gN[ti] += gN[ti + s2], gC[ti] += gC[ti + s2], gArea[ti] += gArea[ti + s2], gLo[ti] = min(gLo[ti], gLo[ti + s2]), gHi[ti] = max(gHi[ti], gHi[ti + s2]);
        GroupMemoryBarrierWithGroupSync();
    }
    const float A = gArea[0];
    const float3 N = A > 0 ? gN[0] / max(length(gN[0]), 1e-30) : float3(0, 0, 1);
    const float3 C = A > 0 ? gC[0] / A : 0;
    const float coherence = A > 0 ? length(gN[0]) / A : 0;
    const float size = length(gHi[0] - gLo[0]);
    // deviation of the vertices from the mean plane
    float e = 0;
    for (uint v = ti; v < count; v += T)
    {
        const float3 p = Vertex(first + v);
        if (all(isfinite(p)))
            e = max(e, abs(dot(N, p - C)));
    }
    gE[ti] = e;
    GroupMemoryBarrierWithGroupSync();
    for (uint s3 = T / 2; s3 > 0; s3 >>= 1)
    {
        if (ti < s3)
            gE[ti] = max(gE[ti], gE[ti + s3]);
        GroupMemoryBarrierWithGroupSync();
    }
    const bool facingCamera = abs(dot(N, normalize(C))) > 0.97;
    // large surface (lake, 150 m wide within a metre): a water mesh ripples, its facets are not aligned,
    // but all its vertices stay close to the plane
    // water, not a haze card or a light shaft (flat squares, turned any which way): at least 10 m
    // wide and roughly horizontal (the camera never tilts more than 60°). Those cards looked like
    // large dark rectangles
    const bool flat = count >= 6 && A > 0 && (coherence > 0.95 || (size > 20.0 && coherence > 0.5)) && gE[0] < 0.02 * size + 0.05 && size > 10.0 && abs(N.y) > 0.5 && !facingCamera;
    if (ti == 0)
    {
        uint dummy;
        // summary, first 256 draws (24 bytes): vertices written and verdict, deviation from the plane and size (cm), normal
        if (group.x < 256)
        {
            stats.Store2(16 + group.x * 24, uint2(count | (flat ? 0x80000000u : 0u) | (uint(coherence > 0.95) << 30) | (uint(facingCamera) << 29),
                                                  min(65535u, uint(gE[0] * 100.0)) | (min(65535u, uint(size * 100.0)) << 16)));
            stats.Store3(16 + group.x * 24 + 8, asuint(N));
            stats.Store(16 + group.x * 24 + 20, asuint(dot(N, C))); // signed distance from the camera to the plane
        }
        stats.InterlockedAdd(8, 1, dummy);
        if (flat)
            stats.InterlockedAdd(0, 1, dummy), stats.InterlockedAdd(4, count / 3, dummy);
    }
    if (!flat)
    {
        // invisible to rays: NaN vertices (inactive triangles), the draw's whole place
        const float nan = asfloat(0x7FC00000);
        for (uint k = ti; k < total; k += T)
            view.Store3((first + k) * 12, asuint(float3(nan, nan, nan)));
    }
    else
    {
        // surface kept: vertices brought onto its mean plane. A game's water is a mesh with large triangles
        // gently rippled: with each facet's normal, the reflection broke into tiles the size of the
        // triangles; flat, it is continuous (the waves stay in the colour the game gives its water)
        for (uint k = ti; k < count; k += T)
        {
            const float3 p = Vertex(first + k);
            if (all(isfinite(p)))
                view.Store3((first + k) * 12, asuint(p - N * dot(N, p - C)));
        }
    }
}
