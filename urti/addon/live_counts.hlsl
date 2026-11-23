// Live URTI: fixed places of objects in the stream (g_livePersist). The count of vertices written by a
// draw of this frame (its counter, reset every frame) is copied into its place's cell,
// which keeps it as long as the object is no longer drawn: its last geometry stays in the RT scene.

StructuredBuffer<uint2> pairs : register(t0); // frame counter, count cell
ByteAddressBuffer counters : register(t1);    // bytes written per draw (8 bytes per counter)
RWByteAddressBuffer counts : register(u0);    // bytes written per place (8 bytes per cell, like counters)

cbuffer Constants : register(b0)
{
    uint pairCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= pairCount)
        return;
    const uint2 p = pairs[id];
    counts.Store2(p.y * 8, uint2(counters.Load(p.x * 8), 0));
}
