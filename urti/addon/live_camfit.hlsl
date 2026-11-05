// URTI en direct : mouvement de la caméra d'une image à l'autre, mesuré sur notre propre géométrie, sans rien
// demander au jeu. La conversion (live_convert) relève, pour les objets dessinés à la même place dans les deux
// images, des paires de sommets : position dans l'espace caméra de l'image précédente (p) et de celle-ci (q),
// une quinzaine par objet (un personnage qui suit la caméra pèse autant qu'un rocher, pas plus). Pour un objet
// immobile, q = R p + t (R, t : mouvement de la caméra). Ajustement par la méthode de Horn (quaternion propre de
// la matrice de corrélation), repris quatre fois en ne gardant que les paires proches de l'estimation précédente :
// ce qui bouge pour de bon (personnages, animaux, feuillage au vent) est écarté.
// Sortie (motion) : R (9 nombres, par lignes), t (3), validité, paires gardées, paires reçues, écart quadratique
// moyen des paires gardées (m) ; octets 64 à 111 : l'estimation de l'image précédente (point de départ).

RWByteAddressBuffer motion : register(u0);
RWByteAddressBuffer pairs : register(u1); // nombre de paires, puis par paire : p.xyz, q.xyz (float) ; compte remis à zéro

cbuffer Constants : register(b0)
{
    uint maxPairs;
    uint pad0;
    uint pad1;
    uint pad2;
};

#define T 512
groupshared float sAcc[T][10];
groupshared float sR[9], sT[3];
groupshared float sTau, sRel, sWeight; // tolérance : absolue (m) et relative à la distance

void Reduce(uint ti, uint count)
{
    GroupMemoryBarrierWithGroupSync();
    for (uint s = T / 2; s > 0; s >>= 1)
    {
        if (ti < s)
            for (uint k = 0; k < count; k++)
                sAcc[ti][k] += sAcc[ti + s][k];
        GroupMemoryBarrierWithGroupSync();
    }
}

float3 LoadP(uint i) { return asfloat(pairs.Load3(16 + i * 24)); }
float3 LoadQ(uint i) { return asfloat(pairs.Load3(16 + i * 24 + 12)); }

float3 Apply(float3 p) { return float3(dot(float3(sR[0], sR[1], sR[2]), p), dot(float3(sR[3], sR[4], sR[5]), p), dot(float3(sR[6], sR[7], sR[8]), p)) + float3(sT[0], sT[1], sT[2]); }

[numthreads(T, 1, 1)]
void main(uint ti : SV_GroupIndex)
{
    const uint n = min(pairs.Load(0), maxPairs);
    if (ti == 0)
    {
        // départ : le mouvement de l'image précédente s'il était valide (la caméra bouge sans à-coups)
        const bool prevValid = asfloat(motion.Load(48)) > 0.5;
        for (uint k = 0; k < 9; k++)
            sR[k] = prevValid ? asfloat(motion.Load(k * 4)) : (k % 4 == 0 ? 1.0 : 0.0);
        for (uint k2 = 0; k2 < 3; k2++)
            sT[k2] = prevValid ? asfloat(motion.Load(36 + k2 * 4)) : 0.0;
        sTau = prevValid ? 0.25 : 2.0, sRel = 0.01;
        // sauvegarde de l'estimation précédente (octets 64 à 111)
        for (uint k3 = 0; k3 < 12; k3++)
            motion.Store(64 + k3 * 4, motion.Load(k3 * 4));
    }
    GroupMemoryBarrierWithGroupSync();
    float rms = 0, kept = 0;
    bool ok = n >= 64;
    for (uint iter = 0; iter < 7 && ok; iter++)
    {
        // poids : paire proche de l'estimation en cours (tolérance absolue + 1 % de la distance)
        float acc[7] = { 0, 0, 0, 0, 0, 0, 0 };
        for (uint i = ti; i < n; i += T)
        {
            const float3 p = LoadP(i), q = LoadQ(i);
            const float e = length(Apply(p) - q);
            if (e < sTau + sRel * length(q))
                acc[0] += 1, acc[1] += p.x, acc[2] += p.y, acc[3] += p.z, acc[4] += q.x, acc[5] += q.y, acc[6] += q.z;
        }
        for (uint k = 0; k < 7; k++)
            sAcc[ti][k] = acc[k];
        Reduce(ti, 7);
        const float w = sAcc[0][0];
        if (iter == 0 && w < 0.3 * n && sTau < 1.0)
        {
            // la caméra a changé d'allure : on repart de l'immobilité, avec une large tolérance
            GroupMemoryBarrierWithGroupSync();
            if (ti == 0)
            {
                for (uint k = 0; k < 9; k++)
                    sR[k] = k % 4 == 0 ? 1.0 : 0.0;
                sT[0] = sT[1] = sT[2] = 0, sTau = 2.0, sRel = 0.01;
            }
            GroupMemoryBarrierWithGroupSync();
            iter = ~0u; // la boucle reprend à 0
            continue;
        }
        if (w < 32)
        {
            ok = false;
            break;
        }
        const float3 pm = float3(sAcc[0][1], sAcc[0][2], sAcc[0][3]) / w, qm = float3(sAcc[0][4], sAcc[0][5], sAcc[0][6]) / w;
        GroupMemoryBarrierWithGroupSync();
        // corrélation centrée S = somme (p - pm)(q - qm)^T
        float s[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        for (uint i2 = ti; i2 < n; i2 += T)
        {
            const float3 p = LoadP(i2), q = LoadQ(i2);
            const float e = length(Apply(p) - q);
            if (e < sTau + sRel * length(q))
            {
                const float3 a = p - pm, b = q - qm;
                s[0] += a.x * b.x, s[1] += a.x * b.y, s[2] += a.x * b.z;
                s[3] += a.y * b.x, s[4] += a.y * b.y, s[5] += a.y * b.z;
                s[6] += a.z * b.x, s[7] += a.z * b.y, s[8] += a.z * b.z;
            }
        }
        for (uint k4 = 0; k4 < 9; k4++)
            sAcc[ti][k4] = s[k4];
        Reduce(ti, 9);
        if (ti == 0)
        {
            const float Sxx = sAcc[0][0], Sxy = sAcc[0][1], Sxz = sAcc[0][2], Syx = sAcc[0][3], Syy = sAcc[0][4], Syz = sAcc[0][5],
                        Szx = sAcc[0][6], Szy = sAcc[0][7], Szz = sAcc[0][8];
            const float N[16] = { Sxx + Syy + Szz, Syz - Szy, Szx - Sxz, Sxy - Syx,
                                  Syz - Szy, Sxx - Syy - Szz, Sxy + Syx, Szx + Sxz,
                                  Szx - Sxz, Sxy + Syx, -Sxx + Syy - Szz, Syz + Szy,
                                  Sxy - Syx, Szx + Sxz, Syz + Szy, -Sxx - Syy + Szz };
            // plus grande valeur propre : itérations de puissance sur N + c I (toutes les valeurs propres positives)
            float c = 0;
            for (uint k5 = 0; k5 < 16; k5++)
                c += abs(N[k5]);
            float4 v = float4(1, 0, 0, 0);
            for (uint it = 0; it < 64; it++)
            {
                float4 r = c * v;
                for (uint a = 0; a < 4; a++)
                    for (uint b = 0; b < 4; b++)
                        r[a] += N[a * 4 + b] * v[b];
                v = r / max(length(r), 1e-30);
            }
            const float qw = v.x, qx = v.y, qy = v.z, qz = v.w;
            sR[0] = 1 - 2 * (qy * qy + qz * qz), sR[1] = 2 * (qx * qy - qw * qz), sR[2] = 2 * (qx * qz + qw * qy);
            sR[3] = 2 * (qx * qy + qw * qz), sR[4] = 1 - 2 * (qx * qx + qz * qz), sR[5] = 2 * (qy * qz - qw * qx);
            sR[6] = 2 * (qx * qz - qw * qy), sR[7] = 2 * (qy * qz + qw * qx), sR[8] = 1 - 2 * (qx * qx + qy * qy);
            const float3 rp = float3(dot(float3(sR[0], sR[1], sR[2]), pm), dot(float3(sR[3], sR[4], sR[5]), pm), dot(float3(sR[6], sR[7], sR[8]), pm));
            sT[0] = qm.x - rp.x, sT[1] = qm.y - rp.y, sT[2] = qm.z - rp.z;
            sWeight = w;
        }
        GroupMemoryBarrierWithGroupSync();
        // écart des paires gardées avec ce mouvement : la tolérance suivante (trois écarts, 2 cm au moins)
        float e2 = 0, cnt = 0;
        for (uint i3 = ti; i3 < n; i3 += T)
        {
            const float3 p = LoadP(i3), q = LoadQ(i3);
            const float e = length(Apply(p) - q);
            if (e < sTau + sRel * length(q))
                e2 += e * e, cnt += 1;
        }
        sAcc[ti][0] = e2, sAcc[ti][1] = cnt;
        Reduce(ti, 2);
        rms = sqrt(sAcc[0][0] / max(sAcc[0][1], 1.0));
        kept = sAcc[0][1];
        // tolérance suivante : elle ne fait que se resserrer (2,5 écarts, 1,5 cm au moins). Dans une forêt au vent
        // (Horizon), ce qui bouge est majoritaire : une tolérance qui suivait l'écart restait large et le mouvement
        // mesuré était un compromis (3 à 6 cm par image, caméra immobile) ; resserrée, il ne reste que le noyau
        // immobile (sol, rochers, troncs)
        if (ti == 0)
            sTau = max(0.015, min(sTau, 2.5 * rms)), sRel = 0.004;
        GroupMemoryBarrierWithGroupSync();
    }
    if (ti == 0)
    {
        // validité : assez de paires gardées (un tiers au moins), mouvement plausible pour une image
        const float3 t = float3(sT[0], sT[1], sT[2]);
        const bool valid = ok && kept >= max(64.0, 0.33 * n) && length(t) < 20.0 && sR[0] + sR[4] + sR[8] > 2.0;
        for (uint k = 0; k < 9; k++)
            motion.Store(k * 4, asuint(valid ? sR[k] : (k % 4 == 0 ? 1.0 : 0.0)));
        for (uint k2 = 0; k2 < 3; k2++)
            motion.Store(36 + k2 * 4, asuint(valid ? sT[k2] : 0.0));
        motion.Store(48, asuint(valid ? 1.0 : 0.0));
        motion.Store(52, asuint(kept));
        motion.Store(56, asuint(float(n)));
        motion.Store(60, asuint(rms));
        pairs.Store(0, 0);
    }
}
