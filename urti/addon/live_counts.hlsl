// URTI en direct : places fixes des objets dans le flux (g_livePersist). Le compte de sommets écrits par un
// dessin de cette image (son compteur, remis à zéro à chaque image) est recopié dans la case de sa place,
// qui le garde tant que l'objet n'est plus dessiné : sa dernière géométrie reste dans la scène RT.

StructuredBuffer<uint2> pairs : register(t0); // compteur de l'image, case de compte
ByteAddressBuffer counters : register(t1);    // octets écrits par dessin (8 octets par compteur)
RWByteAddressBuffer counts : register(u0);    // octets écrits par place (8 octets par case, comme counters)

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
