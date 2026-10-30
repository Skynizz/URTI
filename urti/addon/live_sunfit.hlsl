// URTI en direct : direction du soleil à chaque image, sur le GPU. Un groupe par paire de dessins
// (le même maillage vu par la caméra et dans la carte d'ombres du soleil, même signature) : ajustement
// affine (moindres carrés) des sommets de la caméra vers ceux de la carte (projection parallèle), puis
// la direction que l'application envoie sur l'axe de profondeur de la carte. Les paires mal
// appariées (autre objet, animé) sont rejetées par leur résidu.

ByteAddressBuffer so : register(t0);       // flux de sortie, tous les emplacements
ByteAddressBuffer counters : register(t1); // octets écrits par dessin (chaque dessin a sa zone du flux)
struct Pair
{
    uint4 ids;       // début (octets dans le flux) du dessin caméra, du dessin carte, leurs compteurs
    float4 viewport; // du dessin dans la carte : sa cascade
    uint4 extra;     // facteur de poids (float), tailles des deux zones
};
StructuredBuffer<Pair> pairs : register(t2);
RWByteAddressBuffer dirs : register(u0);   // par paire (80 octets) : direction et poids (0 : rejetée), application (12 float), viewport

cbuffer Constants : register(b0)
{
    float p00;
    float p11;
    uint pairCount;
    uint slotBytes;
};

#define T 64
groupshared double sAcc[T][22];
groupshared float sRes[T][8];
groupshared float sM[12];
groupshared float4 sDir;
groupshared uint sOk;

bool Sample(uint v, uint cBase, uint lBase, out double x[4], out double s[3])
{
    const float4 c = asfloat(so.Load4(cBase + v * 16));
    const float4 l = asfloat(so.Load4(lBase + v * 16));
    x[0] = c.x / p00, x[1] = c.y / p11, x[2] = c.w, x[3] = 1;
    s[0] = l.x / l.w, s[1] = l.y / l.w, s[2] = l.z / l.w;
    return c.w > 1e-3 && abs(l.w) > 1e-6;
}

[numthreads(T, 1, 1)]
void main(uint3 gid : SV_GroupID, uint t : SV_GroupThreadID)
{
    const uint4 p = pairs[gid.x].ids;
    const uint4 x = pairs[gid.x].extra;
    const uint cFilled = min(counters.Load(p.z * 8), x.y), lFilled = min(counters.Load(p.w * 8), x.z);
    const uint n = cFilled / 16;
    // même nombre de sommets, rien de tronqué par une zone trop petite
    if (n < 24 || lFilled / 16 != n || cFilled >= x.y + 16 || lFilled >= x.z + 16)
    {
        if (t == 0)
            dirs.Store4(gid.x * 80, uint4(0, 0, 0, 0));
        return;
    }
    const uint cBase = p.x, lBase = p.y;
    const uint step = max(1u, n / 256);

    // équations normales : N = XᵀX (10 termes), R = XᵀS (12 termes)
    double acc[22];
    for (uint i = 0; i < 22; i++)
        acc[i] = 0;
    for (uint v = t * step; v < n; v += T * step)
    {
        double x[4], s[3];
        if (!Sample(v, cBase, lBase, x, s))
            continue;
        uint k = 0;
        for (uint a = 0; a < 4; a++)
            for (uint b = a; b < 4; b++)
                acc[k++] += x[a] * x[b];
        for (uint a2 = 0; a2 < 4; a2++)
            for (uint c = 0; c < 3; c++)
                acc[10 + a2 * 3 + c] += x[a2] * s[c];
    }
    for (uint j = 0; j < 22; j++)
        sAcc[t][j] = acc[j];
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = T / 2; stride > 0; stride >>= 1)
    {
        if (t < stride)
            for (uint j2 = 0; j2 < 22; j2++)
                sAcc[t][j2] += sAcc[t + stride][j2];
        GroupMemoryBarrierWithGroupSync();
    }

    if (t == 0)
    {
        double N[4][4], R[4][3];
        uint k = 0;
        for (uint a = 0; a < 4; a++)
            for (uint b = a; b < 4; b++)
                N[a][b] = N[b][a] = sAcc[0][k++];
        for (uint a2 = 0; a2 < 4; a2++)
            for (uint c = 0; c < 3; c++)
                R[a2][c] = sAcc[0][10 + a2 * 3 + c];
        bool ok = N[3][3] >= 24; // N[3][3] : nombre de sommets retenus
        for (uint col = 0; col < 4 && ok; col++) // Gauss avec pivot partiel
        {
            uint pv = col;
            for (uint r = col + 1; r < 4; r++)
                if (abs(N[r][col]) > abs(N[pv][col]))
                    pv = r;
            if (abs(N[pv][col]) < 1e-12)
            {
                ok = false;
                break;
            }
            for (uint q = 0; q < 4; q++)
            {
                const double tmp = N[col][q];
                N[col][q] = N[pv][q];
                N[pv][q] = tmp;
            }
            for (uint q2 = 0; q2 < 3; q2++)
            {
                const double tmp = R[col][q2];
                R[col][q2] = R[pv][q2];
                R[pv][q2] = tmp;
            }
            for (uint r2 = 0; r2 < 4; r2++)
            {
                if (r2 == col)
                    continue;
                const double f = N[r2][col] / N[col][col];
                for (uint q3 = 0; q3 < 4; q3++)
                    N[r2][q3] -= f * N[col][q3];
                for (uint q4 = 0; q4 < 3; q4++)
                    R[r2][q4] -= f * R[col][q4];
            }
        }
        float3 d = float3(0, 0, 0);
        if (ok)
        {
            double M[4][3];
            for (uint a3 = 0; a3 < 4; a3++)
                for (uint c2 = 0; c2 < 3; c2++)
                {
                    M[a3][c2] = R[a3][c2] / N[a3][a3];
                    sM[a3 * 3 + c2] = float(M[a3][c2]);
                }
            // d tel que A d = (0, 0, 1), A[r][c] = M[c][r] : troisième colonne de l'inverse
            const double a00 = M[0][0], a01 = M[1][0], a02 = M[2][0];
            const double a10 = M[0][1], a11 = M[1][1], a12 = M[2][1];
            const double a20 = M[0][2], a21 = M[1][2], a22 = M[2][2];
            const double det = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
            if (abs(det) > 1e-18)
            {
                const float3 dd = float3(float(a01 * a12 - a02 * a11), float(a02 * a10 - a00 * a12), float(a00 * a11 - a01 * a10));
                d = -normalize(dd) * sign(float(det));
            }
            else
                ok = false;
        }
        sDir = float4(d, 0);
        sOk = ok ? 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (sOk == 0)
    {
        if (t == 0)
            dirs.Store4(gid.x * 80, uint4(0, 0, 0, 0));
        return;
    }

    // résidu moyen de l'ajustement, rapporté à l'étendue des sommets dans la carte
    float r[8] = { 0, 0, 1e30, 1e30, 1e30, -1e30, -1e30, -1e30 };
    for (uint v2 = t * step; v2 < n; v2 += T * step)
    {
        double x[4], s[3];
        if (!Sample(v2, cBase, lBase, x, s))
            continue;
        float e2 = 0;
        for (uint c = 0; c < 3; c++)
        {
            float pred = sM[9 + c];
            for (uint a = 0; a < 3; a++)
                pred += float(x[a]) * sM[a * 3 + c];
            e2 += (pred - float(s[c])) * (pred - float(s[c]));
            r[2 + c] = min(r[2 + c], float(s[c]));
            r[5 + c] = max(r[5 + c], float(s[c]));
        }
        r[0] += sqrt(e2), r[1] += 1;
    }
    for (uint j3 = 0; j3 < 8; j3++)
        sRes[t][j3] = r[j3];
    GroupMemoryBarrierWithGroupSync();
    for (uint stride2 = T / 2; stride2 > 0; stride2 >>= 1)
    {
        if (t < stride2)
        {
            sRes[t][0] += sRes[t + stride2][0], sRes[t][1] += sRes[t + stride2][1];
            for (uint c = 0; c < 3; c++)
            {
                sRes[t][2 + c] = min(sRes[t][2 + c], sRes[t + stride2][2 + c]);
                sRes[t][5 + c] = max(sRes[t][5 + c], sRes[t + stride2][5 + c]);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0)
    {
        const float mean = sRes[0][0] / max(sRes[0][1], 1.0);
        const float scale = max(max(sRes[0][5] - sRes[0][2], sRes[0][6] - sRes[0][3]), sRes[0][7] - sRes[0][4]) + 1e-12;
        const float w = mean <= 2e-3 * scale ? sRes[0][1] * asfloat(x.x) : 0;
        dirs.Store4(gid.x * 80, asuint(float4(sDir.xyz, w)));
        dirs.Store4(gid.x * 80 + 16, asuint(float4(sM[0], sM[1], sM[2], sM[3])));
        dirs.Store4(gid.x * 80 + 32, asuint(float4(sM[4], sM[5], sM[6], sM[7])));
        dirs.Store4(gid.x * 80 + 48, asuint(float4(sM[8], sM[9], sM[10], sM[11])));
        dirs.Store4(gid.x * 80 + 64, asuint(pairs[gid.x].viewport));
    }
}
