// URTI en direct : loi de profondeur du jeu mesurée à chaque image (z de découpage = A w + B, d'où
// profondeur = A + B / w), par moindres carrés sur des sommets de la géométrie de l'écran captée dans
// cette image. Écrite dans le tampon du soleil (octets 36 à 47 : A, B, validité) ; sans assez de
// sommets, la valeur précédente reste.

ByteAddressBuffer so : register(t0);       // flux de l'écran (sommets en espace de découpage)
ByteAddressBuffer counters : register(t1); // octets écrits dans chaque flux
RWByteAddressBuffer sun : register(u0);

cbuffer Constants : register(b0)
{
    uint slotOffset;    // octets : début du flux de l'écran
    uint counterOffset; // octets : compteur du flux de l'écran
    uint slotBytes;
    uint pad;
};

#define T 256
#define SAMPLES 8192
groupshared double sAcc[T][5];

[numthreads(T, 1, 1)]
void main(uint t : SV_GroupThreadID)
{
    const uint filled = min(counters.Load(counterOffset), slotBytes) / 16;
    const uint step = max(1u, filled / SAMPLES);
    double acc[5] = { 0, 0, 0, 0, 0 }; // n, Σw, Σz, Σw², Σwz
    for (uint i = t * step; i < filled; i += T * step)
    {
        const float4 c = asfloat(so.Load4(slotOffset + i * 16));
        if (c.w > 1e-3 && c.w < 1e6 && abs(c.z) < 1e6)
            acc[0] += 1, acc[1] += c.w, acc[2] += c.z, acc[3] += double(c.w) * c.w, acc[4] += double(c.w) * c.z;
    }
    for (uint k = 0; k < 5; k++)
        sAcc[t][k] = acc[k];
    GroupMemoryBarrierWithGroupSync();
    for (uint s = T / 2; s > 0; s >>= 1)
    {
        if (t < s)
            for (uint k2 = 0; k2 < 5; k2++)
                sAcc[t][k2] += sAcc[t + s][k2];
        GroupMemoryBarrierWithGroupSync();
    }
    if (t != 0)
        return;
    const double n = sAcc[0][0], sw = sAcc[0][1], sz = sAcc[0][2], sww = sAcc[0][3], swz = sAcc[0][4];
    const double den = n * sww - sw * sw;
    if (n < 64 || abs(den) < 1e-12 * n * n)
        return;
    const double A = (n * swz - sw * sz) / den, B = (sz - A * sw) / n;
    if (!(abs(B) > 1e-9) || !(abs(A) < 1e3))
        return;
    sun.Store3(36, asuint(float3(float(A), float(B), 1.0)));
}
