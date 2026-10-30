// URTI en direct : convention des vecteurs de mouvement du jeu (ceux que reçoit DLSS), trouvée toute
// seule. Pour 16 conventions candidates (unité : écran 0..1, NDC -1..1 ou pixels ; signe ; axe vertical
// inversé ou non), la profondeur de l'image précédente lue à l'endroit indiqué doit être celle de ce pixel
// (objets immobiles, petits mouvements). Erreur moyenne par candidate, cumulée par le processeur.

ByteAddressBuffer depth : register(t0);     // profondeur de cette image
ByteAddressBuffer depthPrev : register(t1); // profondeur de l'image précédente
ByteAddressBuffer motion : register(t2);    // vecteurs de mouvement (R16G16 flottants)
ByteAddressBuffer sunBuf : register(t3);    // loi de profondeur (octets 36 à 47)
RWByteAddressBuffer errors : register(u0);  // 16 × (somme des erreurs, nombre) en entiers, + mouvement moyen

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint depthPitch;
    uint motionPitch;
    uint depth24;
    uint pad0;
    uint pad1;
    uint pad2;
};

float ViewDepth(ByteAddressBuffer b, int2 t, float A, float B)
{
    t = clamp(t, int2(0, 0), int2(width - 1, height - 1));
    const uint raw = b.Load(t.y * depthPitch + t.x * 4);
    const float d = depth24 != 0 ? float(raw & 0xFFFFFF) / 16777215.0 : asfloat(raw);
    return B / (d - A);
}

float2 Candidate(uint k, float2 mv)
{
    // unité : 0 écran, 1 NDC (moitié), 2 pixels ; puis signe, puis inversion verticale
    const float2 size = float2(width, height);
    float2 m = (k & 3) == 0 ? mv : (k & 3) == 1 ? mv * 0.5 : (k & 3) == 2 ? mv / size : mv * 2.0;
    if (k & 4)
        m = -m;
    if (k & 8)
        m.y = -m.y;
    return m;
}

[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const float3 law = asfloat(sunBuf.Load3(36));
    if (law.z <= 0)
        return;
    // échantillons répartis sur l'image (grille 64 × 64 décalée)
    const uint gx = id % 64, gy = id / 64;
    if (gy >= 64)
        return;
    const int2 t = int2((gx + 0.5) / 64.0 * width, (gy + 0.5) / 64.0 * height);
    const float w = ViewDepth(depth, t, law.x, law.y);
    if (!(w > 0 && w < 1e5))
        return;
    const uint raw = motion.Load(t.y * motionPitch + t.x * 4);
    const float2 mv = float2(f16tof32(raw & 0xFFFF), f16tof32(raw >> 16));
    if (!all(isfinite(mv)) || dot(mv, mv) < 1e-12)
        return;
    const float2 uv = (float2(t) + 0.5) / float2(width, height);
    for (uint k = 0; k < 16; k++)
    {
        const float2 p = uv + Candidate(k, mv);
        if (any(p < 0) || any(p > 1))
            continue;
        const float wp = ViewDepth(depthPrev, int2(p * float2(width, height)), law.x, law.y);
        if (!(wp > 0 && wp < 1e5))
            continue;
        const float e = min(abs(log(wp / w)), 1.0);
        uint dummy;
        errors.InterlockedAdd(k * 8, uint(e * 10000.0), dummy);
        errors.InterlockedAdd(k * 8 + 4, 1, dummy);
    }
    // mouvement moyen (pour ne juger qu'en mouvement) : |mv| en millionièmes
    uint dummy2;
    errors.InterlockedAdd(128, uint(min(length(mv), 1.0) * 1e6), dummy2);
    errors.InterlockedAdd(132, 1, dummy2);
}
