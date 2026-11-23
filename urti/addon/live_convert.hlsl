// Live URTI: captured vertices (clip space) to camera space for the acceleration
// structure. Each draw has its region of the stream and its counter (no barrier between draws), written in the
// part of the stream of the window its list was written in; its place in the RT scene (the same place from one frame to
// the next) is given when its list executes. A vertex is valid only if it falls in the filled part
// of its region; otherwise it is laid on the region's first vertex: a flat triangle, never hit, but active
// (an update of the structure cannot change a triangle's active / inactive state, and fixed
// places change their content from one frame to the next). At the origin (the camera), an update that made it
// real stretched boxes from the camera to the object, across the whole view: rays ten times slower (v59).
// Place kept without being drawn in this frame: left as it is; outside any place: a flat triangle
// at the origin (place given back, its old vertices must not stay in the scene).

StructuredBuffer<float4> clip : register(t0);   // the screen's stream (start of its slot)
ByteAddressBuffer counters : register(t1);      // bytes written per draw
StructuredBuffer<uint4> regions : register(t2); // per place, sorted: start in the scene (bytes), size, counter (~0: kept), identity
StructuredBuffer<uint> source : register(t3);   // per place: start of its region in the stream (bytes); bit 31: drawn at
                                                // the same place in the previous frame (its old vertices are still there)
RWByteAddressBuffer view : register(u0);        // float3 per vertex
RWByteAddressBuffer pairs : register(u1);       // vertex pairs (previous frame, this one) for live_camfit

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint vertexCount; // vertices to convert (the frame's regions)
    uint regionCount;
    uint maxPairs;
    uint heldStep; // 1: kept places moved by the camera motion below
    uint pad1;
    uint pad2;
    float4 step0;  // camera motion since the previous frame (true space): rows of R, t
    float4 step1;
    float4 step2;
    float2 shearPrev; // sub-pixel jitter (shear) of the previous frame and of this one
    float2 shearCur;
};

// region holding the byte (regions sorted by start)
uint4 RegionOf(uint byte)
{
    uint lo = 0, hi = regionCount;
    while (lo + 1 < hi)
    {
        const uint mid = (lo + hi) >> 1;
        if (regions[mid].x <= byte)
            lo = mid;
        else
            hi = mid;
    }
    return regions[lo];
}

[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= vertexCount)
        return;
    float3 v = 0; // triangle plat
    if (regionCount > 0)
    {
        const uint byte = id * 16;
        uint lo = 0, hi = regionCount;
        while (lo + 1 < hi)
        {
            const uint mid = (lo + hi) >> 1;
            if (regions[mid].x <= byte)
                lo = mid;
            else
                hi = mid;
        }
        const uint4 r = regions[lo];
        const bool inside = byte >= r.x && byte - r.x < r.y;
        if (inside && r.z == 0xFFFFFFFF)
        {
            // kept: its geometry from the last frame where it was drawn, which follows the camera
            if (heldStep != 0)
            {
                const float3 o = asfloat(view.Load3(id * 12));
                if (all(isfinite(o)) && any(o != 0))
                {
                    const float3 t = float3(o.x - shearPrev.x * o.z, o.y - shearPrev.y * o.z, o.z);
                    const float3 n = float3(dot(step0.xyz, t) + step0.w, dot(step1.xyz, t) + step1.w, dot(step2.xyz, t) + step2.w);
                    // (never further than 20 km: a single vertex stretches the whole scene's box)
                    if (all(abs(n) < 20000.0))
                        view.Store3(id * 12, asuint(float3(n.x + shearCur.x * n.z, n.y + shearCur.y * n.z, n.z)));
                }
            }
            return;
        }
        if (!inside)
        {
            view.Store3(id * 12, asuint(v));
            return;
        }
        const uint src = source[lo] & 0x7FFFFFFF;
        const uint filled = min(counters.Load(r.z * 8), r.y);
        const bool written = byte - r.x < filled;
        const float4 c = written ? clip[(src + byte - r.x) / 16] : clip[src / 16];
        const float3 cv = float3(c.x / p00, c.y / p11, c.w);
        // aberrant vertex (non-finite, or further than 20 km): folded onto its region's first vertex, otherwise onto
        // the origin. A single one is enough to stretch the whole scene's box: the inner boxes, stored with
        // a precision relative to it, become worthless and no ray hits anything any more (v66-v67, fixed
        // places and rebuild every frame)
        const float4 c0 = clip[src / 16];
        const float3 v0 = float3(c0.x / p00, c0.y / p11, c0.w);
        if (all(isfinite(cv)) && all(abs(cv) < 20000.0))
        {
            v = cv;
            // camera motion: about fifteen vertices per object that stayed in place (same weight for each
            // object: a character following the camera does not outweigh the scenery), with their previous position
            const uint rel = (byte - r.x) / 16, stride = max(1u, filled / 16 / 16);
            if (maxPairs > 0 && (source[lo] & 0x80000000) && written && rel % stride == 0 && rel / stride < 16)
            {
                const float3 old = asfloat(view.Load3(id * 12));
                if (all(isfinite(old)) && any(old != 0) && all(abs(old) < 20000.0))
                {
                    uint k;
                    pairs.InterlockedAdd(0, 1, k);
                    if (k < maxPairs)
                        pairs.Store3(16 + k * 24, asuint(old)), pairs.Store3(16 + k * 24 + 12, asuint(cv));
                }
            }
        }
        else if (all(isfinite(v0)) && all(abs(v0) < 20000.0))
            v = v0;
    }
    view.Store3(id * 12, asuint(v));
}
