// URTI en direct : vote sur les directions des paires (live_sunfit). La direction sur laquelle
// s'accorde le plus de poids (à 2°) devient le soleil de cette image. Le sens (vers le soleil ou à
// l'opposé) dépend seulement de la convention de profondeur de la carte d'ombres du jeu : fixé une
// fois à l'étalonnage (sunSign), jamais déduit de l'image précédente. Sans accord net, rien ne change.

ByteAddressBuffer dirs : register(t0); // par paire : direction, poids
RWByteAddressBuffer sun : register(u0); // float4 soleil (vers le soleil, espace caméra), float4 bilan

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
    const float4 di = t < pairCount ? asfloat(dirs.Load4(t * 16)) : float4(0, 0, 0, 0);
    float vote = 0, total = 0;
    for (uint j = 0; j < pairCount; j++)
    {
        const float4 dj = asfloat(dirs.Load4(j * 16));
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
    if (sVote[0] > 0 && sVote[0] >= minShare * total)
    {
        const float3 best = asfloat(dirs.Load3(sIndex[0] * 16));
        float3 sum = float3(0, 0, 0);
        for (uint k = 0; k < pairCount; k++)
        {
            const float4 dk = asfloat(dirs.Load4(k * 16));
            if (dk.w > 0 && dot(best, dk.xyz) > cosTol)
                sum += dk.xyz * dk.w;
        }
        if (dot(sum, sum) > 0)
        {
            sun.Store4(0, asuint(float4(normalize(sum) * sunSign, 1)));
            accepted = 1;
        }
    }
    sun.Store4(16, asuint(float4(float(pairCount), sVote[0], total, accepted)));
}
