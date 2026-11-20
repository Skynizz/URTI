// URTI en direct : orientation de la caméra tirée des normales du G-buffer, pour un jeu qui les range dans le repère du monde (Red Dead Redemption 2).
// La passe d'ombres somme, sur les surfaces planes, le produit de la normale du monde (G-buffer) par la normale de la caméra (tirée de la profondeur) :
// H = somme de a b^T. La rotation R qui envoie le monde sur la caméra (b = R a) maximise somme b . R a : méthode de Horn (quaternion, plus grand vecteur
// propre d'une matrice 4 × 4 symétrique, ici par itérations de puissance). Écrite dans sunBuf (octets 2048 à 2095) : R par lignes, accord moyen, pixels, valide.
// Un seul fil, à chaque image calculée, après la passe d'ombres et avant les reflets.

RWByteAddressBuffer stats : register(u0); // octets 64 à 103 : H (9 entiers ×1000, ligne = composante du monde), nombre de pixels
RWByteAddressBuffer sunBuf : register(u1);

cbuffer Constants : register(b0)
{
    uint minPixels; // en dessous : pas de rotation (valide 0)
    float minAgreement;
    uint pad0;
    uint pad1;
};

[numthreads(1, 1, 1)]
void main()
{
    float S[3][3];
    for (uint i = 0; i < 3; i++)
        for (uint j = 0; j < 3; j++)
            S[i][j] = float(int(stats.Load(64 + (i * 3 + j) * 4))) / 1000.0;
    const uint count = stats.Load(100);
    // matrice de Horn (S[i][j] = somme a_i b_j)
    float N[4][4];
    N[0][0] = S[0][0] + S[1][1] + S[2][2];
    N[0][1] = S[1][2] - S[2][1];
    N[0][2] = S[2][0] - S[0][2];
    N[0][3] = S[0][1] - S[1][0];
    N[1][1] = S[0][0] - S[1][1] - S[2][2];
    N[1][2] = S[0][1] + S[1][0];
    N[1][3] = S[2][0] + S[0][2];
    N[2][2] = -S[0][0] + S[1][1] - S[2][2];
    N[2][3] = S[1][2] + S[2][1];
    N[3][3] = -S[0][0] - S[1][1] + S[2][2];
    for (uint r = 0; r < 4; r++)
        for (uint c = 0; c < r; c++)
            N[r][c] = N[c][r];
    // itérations de puissance sur N + s I (s : borne du spectre, toutes les valeurs propres deviennent positives) ; départ : la rotation précédente
    float s = 0;
    for (uint r2 = 0; r2 < 4; r2++)
        for (uint c2 = 0; c2 < 4; c2++)
            s += abs(N[r2][c2]);
    float4 q = asfloat(sunBuf.Load4(2096));
    if (!all(isfinite(q)) || dot(q, q) < 0.5)
        q = float4(1, 0, 0, 0);
    for (uint it = 0; it < 64; it++)
    {
        float4 nq;
        for (uint r3 = 0; r3 < 4; r3++)
            nq[r3] = N[r3][0] * q[0] + N[r3][1] * q[1] + N[r3][2] * q[2] + N[r3][3] * q[3] + s * q[r3];
        const float l = length(nq);
        if (!(l > 1e-20))
            break;
        q = nq / l;
    }
    // valeur propre : q . N q = somme des b . R a ; accord moyen = elle / pixels
    float lambda = 0;
    for (uint r4 = 0; r4 < 4; r4++)
        lambda += q[r4] * (N[r4][0] * q[0] + N[r4][1] * q[1] + N[r4][2] * q[2] + N[r4][3] * q[3]);
    const float agreement = count > 0 ? lambda / float(count) : 0.0;
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    // R (quaternion unitaire) : b = R a
    const float3 r0 = float3(1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y));
    const float3 r1 = float3(2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x));
    const float3 rr = float3(2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y));
    const bool valid = count >= minPixels && agreement >= minAgreement && all(isfinite(q));
    sunBuf.Store3(2048, asuint(r0));
    sunBuf.Store3(2060, asuint(r1));
    sunBuf.Store3(2072, asuint(rr));
    sunBuf.Store3(2084, uint3(asuint(agreement), count, valid ? 1u : 0u));
    if (valid)
        sunBuf.Store4(2096, asuint(q));
}
