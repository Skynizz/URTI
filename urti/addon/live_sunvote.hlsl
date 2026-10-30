// URTI en direct : vote sur les directions des paires (live_sunfit). La direction sur laquelle
// s'accorde le plus de poids (à 2°) devient le soleil de cette image. Le sens (vers le soleil ou à
// l'opposé) dépend seulement de la convention de profondeur de la carte d'ombres du jeu : fixé une
// fois à l'étalonnage (sunSign), jamais déduit de l'image précédente. Sans accord net, rien ne change.

ByteAddressBuffer dirs : register(t0); // par paire : direction, poids
RWByteAddressBuffer sun : register(u0); // float4 soleil (vers le soleil, espace caméra), float4 bilan, uint nombre de
                                        // cascades (à 32), puis par cascade (à 48 + 64 k) : application (3 float4), viewport

cbuffer Constants : register(b0)
{
    uint pairCount;
    float cosTol;
    float minShare; // part minimale du poids total pour la direction gagnante
    float sunSign; // +1 ou -1 : direction de profondeur décroissante de la carte -> vers le soleil
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
        // affinage : moyenne des seules directions à moins de 0,75° de la première moyenne (deux passes)
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
        // saut brusque soutenu par peu de paires : mesure rejetée (le soleil de l'image précédente reste)
        const float3 old = asfloat(sun.Load3(0));
        const bool jump = dot(old, old) > 0.5 && dot(normalize(sum) * sunSign, old) < 0.99863 && share < 0.6; // 3°
        // grand écart (> 3°) même bien soutenu : accepté seulement s'il se confirme à l'image suivante
        // (une fausse mesure isolée ferait pivoter toutes les ombres pendant une image)
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
            // une application par cascade (même viewport) : la paire d'accord de plus grand poids
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
            // la plus fine d'abord (plus grande échelle caméra -> carte)
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
}
