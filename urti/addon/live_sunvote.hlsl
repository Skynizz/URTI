// Live URTI: vote over the pairs' directions (live_sunfit). The direction on which
// the most weight agrees (within 2°) becomes this frame's sun. The orientation (toward the sun or
// away) depends only on the depth convention of the game's shadow map: set once
// at calibration (sunSign), never deduced from the previous frame. Without a clear agreement, nothing changes.

ByteAddressBuffer dirs : register(t0); // per pair: direction, weight
RWByteAddressBuffer sun : register(u0); // float4 sun (toward the sun, camera space), float4 summary, uint number of
                                        // cascades (at 32), then per cascade (at 48 + 64 k): mapping (3 float4), viewport;
                                        // at 560: pending sun; at 576: camera rotation since the previous
                                        // frame (3 rows, validity at 624); at 640: cascades of the previous frame

cbuffer Constants : register(b0)
{
    uint pairCount;
    float cosTol;
    float minShare; // minimum share of the total weight for the winning direction
    float sunSign; // +1 or -1: direction of decreasing map depth -> toward the sun
    float2 jitterMode; // x: jitter difference used (1) or not (0); y: absolute jitter
    float2 pad;
};

#define T 256
groupshared float sVote[T];
groupshared uint sIndex[T];

[numthreads(T, 1, 1)]
void main(uint t : SV_GroupThreadID)
{
    const float4 di = t < pairCount ? asfloat(dirs.Load4(t * 80)) : float4(0, 0, 0, 0);
    float vote = 0, total = 0;
    for (uint j = 0; j < pairCount; j++)
    {
        const float4 dj = asfloat(dirs.Load4(j * 80));
        total += dj.w;
        if (di.w > 0 && dj.w > 0 && dot(di.xyz, dj.xyz) > cosTol)
            vote += dj.w;
    }
    sVote[t] = vote;
    sIndex[t] = t;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = T / 2; stride > 0; stride >>= 1)
    {
        if (t < stride && sVote[t + stride] > sVote[t])
            sVote[t] = sVote[t + stride], sIndex[t] = sIndex[t + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (t != 0)
        return;
    float accepted = 0;
    uint cascades = 0;
    if (sVote[0] > 0 && sVote[0] >= minShare * total)
    {
        const float3 best = asfloat(dirs.Load3(sIndex[0] * 80));
        float3 sum = float3(0, 0, 0);
        for (uint k = 0; k < pairCount; k++)
        {
            const float4 dk = asfloat(dirs.Load4(k * 80));
            if (dk.w > 0 && dot(best, dk.xyz) > cosTol)
                sum += dk.xyz * dk.w;
        }
        // refinement: average of only the directions within 0.75° of the first average (two passes)
        float share = sVote[0] / max(total, 1e-6);
        for (uint pass = 0; pass < 2 && dot(sum, sum) > 0; pass++)
        {
            const float3 center = normalize(sum);
            float3 tight = float3(0, 0, 0);
            float wTight = 0;
            for (uint k2 = 0; k2 < pairCount; k2++)
            {
                const float4 dk = asfloat(dirs.Load4(k2 * 80));
                if (dk.w > 0 && dot(center, dk.xyz) > 0.99991) // 0,75°
                    tight += dk.xyz * dk.w, wTight += dk.w;
            }
            if (wTight > 0)
                sum = tight, share = wTight / max(total, 1e-6);
        }
        // sudden jump supported by few pairs: measurement rejected (the previous frame's sun stays)
        const float3 old = asfloat(sun.Load3(0));
        const bool jump = dot(old, old) > 0.5 && dot(normalize(sum) * sunSign, old) < 0.99863 && share < 0.6; // 3°
        // large difference (> 3°) even well supported: accepted only if it is confirmed on the next frame
        // (an isolated wrong measurement would swing every shadow for a frame)
        bool confirmed = true;
        if (dot(sum, sum) > 0 && !jump && dot(old, old) > 0.5 && dot(normalize(sum) * sunSign, old) < 0.99863)
        {
            const float4 pending = asfloat(sun.Load4(560));
            confirmed = pending.w > 0 && dot(pending.xyz, normalize(sum) * sunSign) > 0.99939; // 2°
            sun.Store4(560, asuint(float4(normalize(sum) * sunSign, confirmed ? 0.0 : 1.0)));
        }
        else
            sun.Store4(560, asuint(float4(0, 0, 0, 0)));
        if (dot(sum, sum) > 0 && !jump && confirmed)
        {
            sun.Store4(0, asuint(float4(normalize(sum) * sunSign, 1)));
            accepted = 1;
            // one mapping per cascade (same viewport): the agreeing pair with the largest weight
            float4 cvp[8];
            uint cpair[8];
            float cw[8];
            for (uint q = 0; q < pairCount; q++)
            {
                const float4 dq = asfloat(dirs.Load4(q * 80));
                if (dq.w <= 0 || dot(best, dq.xyz) <= cosTol)
                    continue;
                const float4 vq = asfloat(dirs.Load4(q * 80 + 64));
                if (vq.z <= 0 || vq.w <= 0)
                    continue;
                uint found = cascades;
                for (uint c = 0; c < cascades; c++)
                    if (all(cvp[c] == vq))
                        found = c;
                if (found == cascades)
                {
                    if (cascades == 8)
                        continue;
                    cvp[cascades] = vq, cpair[cascades] = q, cw[cascades] = dq.w;
                    cascades++;
                }
                else if (dq.w > cw[found])
                    cpair[found] = q, cw[found] = dq.w;
            }
            // the finest first (largest camera -> map scale)
            float scale[8];
            for (uint c2 = 0; c2 < cascades; c2++)
                scale[c2] = length(asfloat(dirs.Load3(cpair[c2] * 80 + 16)));
            for (uint i2 = 0; i2 < cascades; i2++)
                for (uint j2 = i2 + 1; j2 < cascades; j2++)
                    if (scale[j2] > scale[i2])
                    {
                        const float ts = scale[i2]; scale[i2] = scale[j2]; scale[j2] = ts;
                        const uint tp = cpair[i2]; cpair[i2] = cpair[j2]; cpair[j2] = tp;
                    }
            for (uint c3 = 0; c3 < cascades; c3++)
                for (uint r = 0; r < 4; r++)
                    sun.Store4(48 + c3 * 64 + r * 16, dirs.Load4(cpair[c3] * 80 + 16 + r * 16));
        }
    }
    sun.Store4(16, asuint(float4(float(pairCount), sVote[0], total, accepted)));
    sun.Store(32, cascades);

    // camera rotation: the shadow map has fixed axes in the world; the linear part of the
    // camera -> map mappings of one cascade, over two frames, gives c_prev = Lprev⁻¹ Lcur c
    // (the translation of the cascades, which follow the camera, does not touch that part)
    const uint prevCount = sun.Load(640);
    float valid = 0;
    if (cascades > 0 && prevCount > 0)
    {
        const float4 vp = asfloat(sun.Load4(48 + 48));
        for (uint pk = 0; pk < min(prevCount, 8u) && valid == 0; pk++)
        {
            if (any(asfloat(sun.Load4(656 + pk * 64 + 48)) != vp))
                continue;
            float Mc[12], Mp[12];
            for (uint r2 = 0; r2 < 3; r2++)
            {
                const float4 a = asfloat(sun.Load4(48 + r2 * 16)), b = asfloat(sun.Load4(656 + pk * 64 + r2 * 16));
                Mc[r2 * 4 + 0] = a.x, Mc[r2 * 4 + 1] = a.y, Mc[r2 * 4 + 2] = a.z, Mc[r2 * 4 + 3] = a.w;
                Mp[r2 * 4 + 0] = b.x, Mp[r2 * 4 + 1] = b.y, Mp[r2 * 4 + 2] = b.z, Mp[r2 * 4 + 3] = b.w;
            }
            // M[a*3 + c]: map component c per camera coordinate a; L[c][a] = M[a*3 + c]
            float3x3 Lc, Lp;
            for (uint c = 0; c < 3; c++)
                for (uint a2 = 0; a2 < 3; a2++)
                    Lc[c][a2] = Mc[a2 * 3 + c], Lp[c][a2] = Mp[a2 * 3 + c];
            const float det = determinant(Lp);
            if (abs(det) < 1e-12)
                continue;
            float3x3 inv;
            inv[0] = cross(Lp[1], Lp[2]) / det;
            inv[1] = cross(Lp[2], Lp[0]) / det;
            inv[2] = cross(Lp[0], Lp[1]) / det;
            const float3x3 R = mul(transpose(inv), Lc);
            sun.Store4(576, asuint(float4(R[0], 0)));
            sun.Store4(592, asuint(float4(R[1], 0)));
            sun.Store4(608, asuint(float4(R[2], 0)));
            valid = 1;
        }
    }
    sun.Store(624, asuint(valid));
    // the game's sub-pixel jitter: the jittered projection adds a shear to camera space
    // (x' = x + a w, y' = y + b w), which is not a rotation; A = K_prev R K_cur⁻¹ gives to first order
    // a_prev - a_cur = A[0][2] + A[2][0] and b_prev - b_cur = A[1][2] + A[2][1]. Tracked in absolute terms: sum
    // of the differences minus its running mean (a game's jitter sequence is centred)
    //
    // The projection URTI assumes (50° field) is not the game's: URTI's camera space is
    // the true space stretched by s (in x) and t (in y), and a rotation y becomes S R S⁻¹: A[0][2] = s r02,
    // A[2][0] = -r02 / s. Hence a_prev - a_cur = A[0][2] + s² A[2][0] (and b with t²). s² and t² are measured
    // by regression on frames where the camera turns (otherwise the rotation leaked into the jitter: drift
    // of several pixels while walking). s = tan(true field / 2) / tan(assumed field / 2).
    float2 jitterDelta = 0;
    float4 fit = asfloat(sun.Load4(1200)); // decaying sums: A20², A02 A20, A21², A12 A21
    if (valid > 0)
    {
        const float3 a0 = asfloat(sun.Load3(576)), a1 = asfloat(sun.Load3(592)), a2 = asfloat(sun.Load3(608));
        fit = fit * 0.995 + float4(a2.x * a2.x, a0.z * a2.x, a2.y * a2.y, a1.z * a2.y);
        const float s2 = fit.x > 1e-6 ? clamp(-fit.y / fit.x, 0.25, 4.0) : 1.0;
        const float t2 = fit.z > 1e-6 ? clamp(-fit.w / fit.z, 0.25, 4.0) : 1.0;
        sun.Store4(1200, asuint(fit));
        sun.Store2(1216, asuint(float2(s2, t2)));
        jitterDelta = float2(a0.z + s2 * a2.x, a1.z + t2 * a2.y);
        if (any(abs(jitterDelta) > 0.01))
            jitterDelta = 0; // not a sub-pixel jitter (aberrant measurement)
    }
    sun.Store2(628, asuint(jitterDelta * jitterMode.x));
    float4 track = asfloat(sun.Load4(1168)); // sum (xy), running mean (zw)
    track.xy -= jitterDelta;
    track.zw = lerp(track.zw, track.xy, 1.0 / 32.0);
    sun.Store4(1168, asuint(track));
    sun.Store2(1184, asuint((track.xy - track.zw) * jitterMode.y));
    // this frame's cascades kept for the next
    sun.Store(640, cascades);
    for (uint c4 = 0; c4 < cascades; c4++)
        for (uint r3 = 0; r3 < 4; r3++)
            sun.Store4(656 + c4 * 64 + r3 * 16, sun.Load4(48 + c4 * 64 + r3 * 16));
}
