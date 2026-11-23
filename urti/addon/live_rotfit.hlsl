// Live URTI: camera orientation from the G-buffer normals, for a game that stores them in world space (Red Dead Redemption 2).
// The shadow pass sums, over flat surfaces, the product of the world normal (G-buffer) by the camera normal (from depth):
// H = sum of a b^T. The rotation R sending the world onto the camera (b = R a) maximises sum b . R a: Horn's method (quaternion, largest
// eigenvector of a symmetric 4 × 4 matrix, here through power iterations). Written into sunBuf (bytes 2048 to 2095): R by rows, mean agreement, pixels, valid.
// A single thread, on every computed frame, after the shadow pass and before the reflections.

RWByteAddressBuffer stats : register(u0); // bytes 64 to 103: H (9 integers ×1000, row = world component), number of pixels
RWByteAddressBuffer sunBuf : register(u1);

cbuffer Constants : register(b0)
{
    uint minPixels; // below: no rotation (valid 0)
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
    // Horn's matrix (S[i][j] = sum a_i b_j)
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
    // power iterations on N + s I (s: bound of the spectrum, every eigenvalue becomes positive); start: the previous rotation
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
    // eigenvalue: q . N q = sum of b . R a; mean agreement = it / pixels
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
