// Live URTI: camera motion from one frame to the next, measured on our own geometry, without asking
// the game anything. The conversion (live_convert) records, for objects drawn at the same place in both
// frames, vertex pairs: position in the previous frame's camera space (p) and in this one's (q),
// about fifteen per object (a character following the camera weighs as much as a rock, no more). For a
// still object, q = R p + t (R, t: camera motion). Fit by Horn's method (eigen-quaternion of
// the correlation matrix), repeated four times keeping only the pairs close to the previous estimate:
// what really moves (characters, animals, foliage in the wind) is discarded.
// Output (motion): R (9 numbers, by rows), t (3), validity, pairs kept, pairs received, root mean square
// deviation of the kept pairs (m); bytes 64 to 111: the previous frame's estimate (starting point); bytes 128
// to 175: accumulated pose (R by rows, t) from the geometry cache's fixed frame to this frame's camera.

RWByteAddressBuffer motion : register(u0);
RWByteAddressBuffer pairs : register(u1); // number of pairs, then per pair: p.xyz, q.xyz (float); count reset

cbuffer Constants : register(b0)
{
    uint maxPairs;
    uint resetPose; // 1: the geometry cache's frame restarts from this frame's camera
    uint pad1;
    uint pad2;
};

#define T 512
groupshared float sAcc[T][10];
groupshared float sR[9], sT[3];
groupshared float sTau, sRel, sWeight; // tolerance: absolute (m) and relative to the distance

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
        // start: the previous frame's motion if it was valid (the camera moves without jolts)
        const bool prevValid = asfloat(motion.Load(48)) > 0.5;
        for (uint k = 0; k < 9; k++)
            sR[k] = prevValid ? asfloat(motion.Load(k * 4)) : (k % 4 == 0 ? 1.0 : 0.0);
        for (uint k2 = 0; k2 < 3; k2++)
            sT[k2] = prevValid ? asfloat(motion.Load(36 + k2 * 4)) : 0.0;
        sTau = prevValid ? 0.25 : 2.0, sRel = 0.01;
        // backup of the previous estimate (bytes 64 to 111)
        for (uint k3 = 0; k3 < 12; k3++)
            motion.Store(64 + k3 * 4, motion.Load(k3 * 4));
    }
    GroupMemoryBarrierWithGroupSync();
    float rms = 0, kept = 0;
    bool ok = n >= 64;
    for (uint iter = 0; iter < 7 && ok; iter++)
    {
        // weight: pair close to the current estimate (absolute tolerance + 1 % of the distance)
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
            // the camera changed pace: restart from stillness, with a wide tolerance
            GroupMemoryBarrierWithGroupSync();
            if (ti == 0)
            {
                for (uint k = 0; k < 9; k++)
                    sR[k] = k % 4 == 0 ? 1.0 : 0.0;
                // wide: a quarter turn per second is already several metres per frame for a tree 50 m away
                sT[0] = sT[1] = sT[2] = 0, sTau = 5.0, sRel = 0.15;
            }
            GroupMemoryBarrierWithGroupSync();
            iter = ~0u; // the loop restarts at 0
            continue;
        }
        if (w < 32)
        {
            ok = false;
            break;
        }
        const float3 pm = float3(sAcc[0][1], sAcc[0][2], sAcc[0][3]) / w, qm = float3(sAcc[0][4], sAcc[0][5], sAcc[0][6]) / w;
        GroupMemoryBarrierWithGroupSync();
        // centred correlation S = sum (p - pm)(q - qm)^T
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
            // largest eigenvalue: power iterations on N + c I (all eigenvalues positive)
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
        // deviation of the kept pairs with this motion: the next tolerance (three deviations, 2 cm at least)
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
        // next tolerance: it only tightens (2.5 deviations, 1.5 cm at least). In a forest in the wind
        // what moves is the majority: a tolerance that followed the deviation stayed wide and the measured motion
        // was a compromise (3 to 6 cm per frame, still camera); tightened, only the still core
        // remains (ground, rocks, trunks)
        if (ti == 0)
            sTau = max(0.015, min(sTau, 2.5 * rms)), sRel = 0.004;
        GroupMemoryBarrierWithGroupSync();
    }
    if (ti == 0)
    {
        // validity: enough pairs kept (a third at least), a plausible motion for one frame
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
        // accumulated pose: camera = R frame + t; motion not measured: the camera is assumed still. Rotation
        // re-orthogonalised every frame (rounding errors would add up). The game's sub-pixel jitter (TAA)
        // leaves no drift: it cancels out from one frame to the next in the sum
        float3 x0 = asfloat(motion.Load3(128)), x1 = asfloat(motion.Load3(140)), x2 = asfloat(motion.Load3(152)), xt = asfloat(motion.Load3(164));
        if (resetPose != 0 || !all(isfinite(x0 + x1 + x2 + xt)) || dot(x0, x0) < 0.5)
            x0 = float3(1, 0, 0), x1 = float3(0, 1, 0), x2 = float3(0, 0, 1), xt = 0;
        else if (valid)
        {
            const float3 m0 = float3(sR[0], sR[1], sR[2]), m1 = float3(sR[3], sR[4], sR[5]), m2 = float3(sR[6], sR[7], sR[8]);
            const float3 c0 = float3(x0.x, x1.x, x2.x), c1 = float3(x0.y, x1.y, x2.y), c2 = float3(x0.z, x1.z, x2.z);
            const float3 n0 = float3(dot(m0, c0), dot(m0, c1), dot(m0, c2)), n1 = float3(dot(m1, c0), dot(m1, c1), dot(m1, c2));
            xt = float3(dot(m0, xt), dot(m1, xt), dot(m2, xt)) + t;
            x0 = normalize(n0);
            x1 = normalize(n1 - x0 * dot(x0, n1));
            x2 = cross(x0, x1);
        }
        motion.Store3(128, asuint(x0)), motion.Store3(140, asuint(x1)), motion.Store3(152, asuint(x2)), motion.Store3(164, asuint(xt));
        motion.Store2(176, uint2(0, 0)); // no known sub-pixel jitter (game camera not read)
        pairs.Store(0, 0);
    }
}
