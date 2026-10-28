/*
 * KRC : rééclairage exact par groupes de lampes, en extension de SHaRC (v1.8.x).
 *
 * Indépendant du moteur : les tampons et réglages passent par KrcRelightParameters, comme SHaRC
 * avec SharcParameters. À inclure après SharcCommon.h. Mode d'emploi : docs/INTEGRATION.md.
 *
 * Principe. Chaque cellule de SHaRC apprend la part de sa lumière qui vient de chaque groupe de
 * lampes (3 groupes suivis ; le reste, ciel et émissifs, est fixe). La lumière directe d'un sommet
 * est rangée dans le groupe de la lampe tirée et remonte vers les cellules précédentes avec les
 * poids de SHaRC ; un sommet qui relit le cache apporte les parts de sa cellule. Quand le moteur
 * change l'intensité d'un groupe d'un rapport connu r (« après / avant »), KrcRelightEntry
 * multiplie la radiance de chaque cellule par k = 1 + Σ f_g (r_g − 1) avant le tracé de la frame :
 * le cache passe d'un coup au nouvel éclairage, sans bruit ni retard. Exact pour la luminance ; la
 * couleur est approchée si les groupes n'ont pas la même teinte.
 *
 * Le produit S des facteurs k de chaque cellule sert au débruiteur : son entrée est divisée par S
 * (lissé à l'écran) et sa sortie remultipliée par S, donc son historique ressort rééclairé alors
 * qu'il n'y a pas accès (NRD, DLSS RR…).
 *
 * Tampons, une entrée par entrée de la table de SHaRC, remis à zéro avec elle :
 *   groupAccumulationBuffer  uint4  sommes de la frame en virgule fixe (xyz groupes, w total)
 *   groupBuffer              float4 moyennes lissées (même rangement)
 *   factorBuffer             float2 x : facteur de la frame, y : produit S (0 vaut 1)
 */

#ifndef KRC_RELIGHT_H
#define KRC_RELIGHT_H

#ifndef KRC_GROUP_SCALE
#define KRC_GROUP_SCALE 1e3f // virgule fixe des sommes (comme SHARC_RADIANCE_SCALE)
#endif

struct KrcRelightParameters
{
    RW_STRUCTURED_BUFFER(groupAccumulationBuffer, uint4);
    RW_STRUCTURED_BUFFER(groupBuffer, float4);
    RW_STRUCTURED_BUFFER(factorBuffer, float2);
    uint lightGroups;   // groupe de la lampe i aux bits 2i..2i+1 (0..2 suivis, 3 fixe) ; 16 lampes au plus
    float3 ratio;       // rapport « après / avant » des groupes 0..2 (1 : inchangé ou inconnu)
};

uint KrcLightGroup(KrcRelightParameters krcParameters, uint lightIndex)
{
    return lightIndex < 16u ? (krcParameters.lightGroups >> (2u * lightIndex)) & 3u : 3u;
}

// lumière directe d'une lampe, rangée dans son groupe (luminance) ; à sommer sur les lampes du sommet
float3 KrcGroupSplit(KrcRelightParameters krcParameters, uint lightIndex, float3 lightContribution)
{
    const uint g = KrcLightGroup(krcParameters, lightIndex);
    const float lum = SharcLuma(lightContribution);
    return float3(g == 0u ? lum : 0.0f, g == 1u ? lum : 0.0f, g == 2u ? lum : 0.0f);
}

#if SHARC_UPDATE
void KrcGroupAdd(KrcRelightParameters krcParameters, HashGridIndex cell, float4 value)
{
    const uint4 s = uint4(max(value, 0.0f) * KRC_GROUP_SCALE);
    if (s.x != 0) InterlockedAdd(BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, cell).x, s.x);
    if (s.y != 0) InterlockedAdd(BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, cell).y, s.y);
    if (s.z != 0) InterlockedAdd(BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, cell).z, s.z);
    if (s.w != 0) InterlockedAdd(BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, cell).w, s.w);
}

// Juste avant SharcUpdateHit, avec le même état, la même lumière directe et le même nombre
// aléatoire : refait son insertion et sa décision de relecture du cache, et range les parts aux
// mêmes cellules. directGroups : somme des KrcGroupSplit des lampes du sommet.
void KrcUpdateHit(in SharcParameters sharcParameters, KrcRelightParameters krcParameters, in SharcState sharcState, SharcHitData sharcHitData,
    float3 directLighting, float3 directGroups, float random)
{
    HashGridKey hashGridKey;
    HashGridIndex cell;
    if (!HashGridInsertEntry(sharcParameters.hashGridData, sharcHitData.positionWorld, sharcHitData.normalWorld, sharcParameters.hashGridParameters, hashGridKey, cell))
        return;

    float3 materialDemodulation = float3(1.0f, 1.0f, 1.0f);
#if SHARC_MATERIAL_DEMODULATION
    materialDemodulation = sharcHitData.materialDemodulation;
#endif

    float3 radiance = directLighting;
    float4 groups = float4(directGroups, SharcLuma(directLighting));
    bool resampled = false;
#if SHARC_ENABLE_CACHE_RESAMPLING
    const uint resamplingDepth = uint(round(lerp(SHARC_RESAMPLING_DEPTH_MIN, SHARC_PROPAGATION_DEPTH, random)));
    if (resamplingDepth <= sharcState.pathLength)
    {
        const SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, cell);
        if (voxelData.accumulatedSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
        {
            radiance = SharcDecodeRadiance(voxelData.accumulatedRadiance, SharcGetRadianceDirection(sharcHitData)) * materialDemodulation;
            const float4 f = BUFFER_AT_OFFSET(krcParameters.groupBuffer, cell);
            const float lum = SharcLuma(radiance);
            groups = float4(f.w > 0.0f ? saturate(f.xyz / f.w) * lum : float3(0, 0, 0), lum);
            resampled = true;
        }
    }
#endif // SHARC_ENABLE_CACHE_RESAMPLING

    const float lum = SharcLuma(radiance);
    if (lum <= 0.0f)
        return;

    // la cellule du sommet garde sa lumière directe démodulée (comme SHaRC), sauf relecture du cache
    if (!resampled)
        KrcGroupAdd(krcParameters, cell, groups * (SharcLuma(radiance / materialDemodulation) / lum));

    for (uint i = 0; i < sharcState.pathLength; ++i)
    {
        const HashGridIndex previous = sharcState.cacheIndices[i];
        if (previous == HASH_GRID_INVALID_CACHE_INDEX)
            continue;
        KrcGroupAdd(krcParameters, previous, groups * (SharcLuma(radiance * float3(sharcState.sampleWeights[i])) / lum));
    }
}

// juste avant SharcUpdateMiss : le ciel ne compte que dans le total
void KrcUpdateMiss(KrcRelightParameters krcParameters, in SharcState sharcState, float3 radiance)
{
    for (uint i = 0; i < sharcState.pathLength; ++i)
    {
        const HashGridIndex previous = sharcState.cacheIndices[i];
        if (previous == HASH_GRID_INVALID_CACHE_INDEX)
            continue;
        KrcGroupAdd(krcParameters, previous, float4(0, 0, 0, SharcLuma(radiance * float3(sharcState.sampleWeights[i]))));
    }
}
#endif // SHARC_UPDATE

// Dans la passe de résolution, juste avant SharcResolveEntry : moyenne des parts avec les poids de
// SHaRC (accumulationFrameNum : celui de SharcResolveParameters). Renvoie si l'entrée était occupée,
// à passer à KrcResolveEviction. Les entrées vides (la plupart de la table) ne coûtent qu'une lecture.
bool KrcResolveEntry(uint entryIndex, SharcParameters sharcParameters, KrcRelightParameters krcParameters, uint accumulationFrameNum)
{
    if (entryIndex >= sharcParameters.hashGridData.capacity)
        return false;
    if (BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, entryIndex) == HASH_GRID_INVALID_HASH_KEY)
        return false;

    const uint4 acc = BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, entryIndex);
    if (any(acc != 0))
        BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, entryIndex) = uint4(0, 0, 0, 0);

    const float sampleNum = SharcGetAccumulatedSampleNum(BUFFER_AT_OFFSET(sharcParameters.accumulationBuffer, entryIndex));
    if (sampleNum == 0)
        return true;

    const SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, entryIndex);
    float sampleNumPrev = voxelData.accumulatedSampleNum;
    const uint accumulatedFrameNum = voxelData.accumulatedFrameNum + 1;
    const uint frameNumMax = clamp(accumulationFrameNum, SHARC_ACCUMULATED_FRAME_NUM_MIN, SHARC_ACCUMULATED_FRAME_NUM_MAX);
    if (accumulatedFrameNum > frameNumMax)
        sampleNumPrev *= float(frameNumMax) / float(accumulatedFrameNum);

    const float4 frameMean = float4(acc) / (KRC_GROUP_SCALE * sampleNum);
    BUFFER_AT_OFFSET(krcParameters.groupBuffer, entryIndex) =
        (BUFFER_AT_OFFSET(krcParameters.groupBuffer, entryIndex) * sampleNumPrev + frameMean * sampleNum) / (sampleNumPrev + sampleNum);
    return true;
}

// Juste après SharcResolveEntry : une cellule que SHaRC vient d'évincer libère ses parts et son
// produit S, pour que la prochaine cellule rangée là reparte de zéro (et de S = 1).
void KrcResolveEviction(uint entryIndex, SharcParameters sharcParameters, KrcRelightParameters krcParameters, bool wasValid)
{
    if (!wasValid || BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, entryIndex) != HASH_GRID_INVALID_HASH_KEY)
        return;
    BUFFER_AT_OFFSET(krcParameters.groupAccumulationBuffer, entryIndex) = uint4(0, 0, 0, 0);
    BUFFER_AT_OFFSET(krcParameters.groupBuffer, entryIndex) = float4(0, 0, 0, 0);
    BUFFER_AT_OFFSET(krcParameters.factorBuffer, entryIndex) = float2(0, 0);
}

// Passe à part, une entrée par fil, avant la mise à jour de SHaRC d'une frame où un rapport est
// connu : rééclaire la cellule et note son facteur.
void KrcRelightEntry(uint entryIndex, SharcParameters sharcParameters, KrcRelightParameters krcParameters)
{
    if (entryIndex >= sharcParameters.hashGridData.capacity)
        return;
    if (BUFFER_AT_OFFSET(sharcParameters.hashGridData.hashEntriesBuffer, entryIndex) == HASH_GRID_INVALID_HASH_KEY)
        return;
    float2 factor = BUFFER_AT_OFFSET(krcParameters.factorBuffer, entryIndex);

    const float4 groups = BUFFER_AT_OFFSET(krcParameters.groupBuffer, entryIndex);
    if (groups.w <= 0.0f)
    {
        BUFFER_AT_OFFSET(krcParameters.factorBuffer, entryIndex) = float2(1.0f, factor.y);
        return;
    }

    const float3 f = saturate(groups.xyz / groups.w);
    const float k = max(1.0f + dot(f, krcParameters.ratio - 1.0f), 0.0f);

    SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, entryIndex);
    BUFFER_AT_OFFSET(sharcParameters.resolvedBuffer, entryIndex) = SharcPackVoxelData(SharcScaleRadianceData(voxelData.accumulatedRadiance, k),
        voxelData.accumulatedSampleNum, voxelData.accumulatedFrameNum, voxelData.staleFrameNum, voxelData.sampleDataExt);
    BUFFER_AT_OFFSET(krcParameters.groupBuffer, entryIndex) = float4(groups.xyz * krcParameters.ratio, groups.w * k);
    BUFFER_AT_OFFSET(krcParameters.factorBuffer, entryIndex) = float2(k, (factor.y == 0.0f ? 1.0f : factor.y) * k);
}

// Facteurs de la cellule d'un point (x : de la frame, y : produit S) ; (1, 1) sans cellule. Au
// premier point touché, à écrire dans une texture pour le débruiteur.
float2 KrcGetRelightFactor(in SharcParameters sharcParameters, KrcRelightParameters krcParameters, float3 position, float3 normal)
{
    HashGridKey hashGridKey;
    const HashGridIndex cell = HashGridFindEntry(sharcParameters.hashGridData, position, normal, sharcParameters.hashGridParameters, hashGridKey);
    if (cell == HASH_GRID_INVALID_CACHE_INDEX)
        return float2(1.0f, 1.0f);
    const float2 factor = BUFFER_AT_OFFSET(krcParameters.factorBuffer, cell);
    return float2(factor.x, factor.y == 0.0f ? 1.0f : factor.y);
}

#endif // KRC_RELIGHT_H
