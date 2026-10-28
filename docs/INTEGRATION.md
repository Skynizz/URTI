# Intégrer KRC dans un moteur ou un jeu

Pour qui : un développeur de moteur qui a déjà un path tracer (ou qui intègre SHaRC). KRC s'ajoute
**par-dessus SHaRC** : même table de hachage, mêmes passes, plus quelques tampons et cinq points
d'accroche. Un moteur qui a déjà SHaRC reçoit KRC en une journée ; sans SHaRC, il faut d'abord
suivre le guide d'intégration de SHaRC (SDK RTXGI de NVIDIA), puis ce document.

Implémentation de référence, compilée et mesurée : le path tracer d'exemple du SDK RTXGI, branche
`krc` de `F:\rtxgi\sdk` (fichiers `Samples/Pathtracer/Shaders/KrcRelight.hlsli`, `SharcResolve.hlsl`,
`Pathtracer.hlsl`, `Denoiser.hlsl`, `KrcBench.cpp`). La bibliothèque elle-même est dans
`Libraries/Krc/include`, recopiée dans ce dépôt sous `krc_sdk/include`.

## Ce que KRC apporte

| Fonction | Fichier | Gain mesuré (banc NVIDIA) | Coût |
|---|---|---|---|
| Rééclairage exact du cache par groupes de lampes | `KrcRelight.h` | image brute juste dès la frame 0 après un changement de lampe (SHaRC : ~128 frames) | +0,01 à +0,05 ms (salon, 1280×720) |
| Historique du débruiteur rééclairé (normalisation par S) | `KrcRelight.h` + passe de lissage | ꟻLIP après NRD 0,095 contre 0,114 à la frame 0 (salon, coupure du soleil) | compris ci-dessus ; lissage seulement après un premier rééclairage |
| Cellules adaptatives (KRC-A) | `KrcAdaptive.hlsli` (exemple) | biais du cache −12 % (relMSE) | +0,24 ms |

## Les cinq points d'accroche

Tout passe par `KrcRelightParameters` (tampons et réglages), comme SHaRC avec `SharcParameters`.
Inclure `KrcRelight.h` après `SharcCommon.h`.

1. **Tampons** (une entrée par entrée de la table de SHaRC, remis à zéro avec elle) :
   `uint4` sommes de la frame, `float4` parts lissées, `float2` facteurs (k de la frame, produit S).
   Soit 40 octets par entrée : 160 Mo pour les 4 M d'entrées de l'exemple ; 1 M d'entrées suffit
   souvent (40 Mo).
2. **Mise à jour de SHaRC** (passe de mise à jour) :
   - à chaque lampe évaluée au sommet, `krcDirectGroups += KrcGroupSplit(p, lightIndex, contribution)` ;
   - juste avant `SharcUpdateHit` : `KrcUpdateHit(sharc, p, sharcState, hitData, directLighting, krcDirectGroups, random)`
     avec **le même nombre aléatoire** que `SharcUpdateHit` (le tirer une fois dans une variable) ;
   - juste avant `SharcUpdateMiss` : `KrcUpdateMiss(p, sharcState, skyRadiance)`.
3. **Résolution** : `bool v = KrcResolveEntry(i, sharc, p, accumulationFrameNum);` avant
   `SharcResolveEntry`, puis `KrcResolveEviction(i, sharc, p, v);` après.
4. **Passe de rééclairage** : une passe de calcul, un fil par entrée, `KrcRelightEntry(i, sharc, p)`,
   **avant la mise à jour de SHaRC**, seulement les frames où `KrcRelightFrame::event` est vrai.
5. **Côté CPU** : `KrcRelightHost` (`KrcRelightHost.h`) reçoit chaque frame la puissance et le
   groupe de chaque lampe, et rend les rapports, `lightGroups`, `event` et `active`.

Groupes : 0 à 2 suivis, 3 fixe. Le moteur choisit : par exemple 0 soleil, 1 lampes d'intérieur,
2 lampes qui clignotent ; dans Doom, un groupe par secteur et forme de lampe. Un groupe dont les
lampes changent de rapports différents, ou qui s'allument depuis zéro, est inconnu cette frame-là
(pas de rééclairage : SHaRC reprend la main, avec son retard).

## Débruiteur (facultatif, recommandé)

NRD, DLSS RR et les autres gardent leur propre historique, que le cache ne peut pas toucher. Pour
qu'il suive le rééclairage :
1. au premier point touché de chaque pixel, écrire `KrcGetRelightFactor(sharc, p, pos, normal).y`
   (le produit S) dans une texture, si `active` ;
2. lisser cette texture (passe `krcScaleBlur` de `Denoiser.hlsl` : 9 × 9 prises espacées de 4 pixels,
   en logarithme, arrêtée aux arêtes), sinon les cellules se dessinent dans l'image ;
3. diviser l'entrée du débruiteur par S lissé, multiplier sa sortie par le même S.

Variante non retenue : la confiance d'historique de NRD (`IN_DIFF_CONFIDENCE`) ; REBLUR ne ramène
son historique qu'à une frame, l'ancien éclairage pèse encore 50 % puis s'efface en 1/n.

## Pièges rencontrés

- L'exemple de NVIDIA ne tire par défaut que la lampe 0 en lumière directe (`targetLight = 0`) :
  des lampes ajoutées ne servent à rien tant qu'on ne le met pas à −1.
- Tout changement d'en-tête partagé CPU/GPU (constantes) doit recompiler tous les fichiers ; le
  compilateur MSVC en français casse le suivi des en-têtes par Ninja.
- Ne pas parcourir toute la table pour remettre des zéros chaque frame : la plupart des entrées
  sont vides, et 4 M d'écritures coûtent plus que le reste de KRC.
- GLSL (Quake II RTX, RTGL1) : `KrcRelight.h` suit les conventions de SHaRC (`RW_STRUCTURED_BUFFER`,
  `BUFFER_AT_OFFSET`) mais utilise la syntaxe HLSL (`float3`, `lerp`, `saturate`, `InterlockedAdd`) ;
  il faut les mêmes macros de compatibilité que celles du projet pour SHaRC.

## Où le tester

| Cible | État | Effort |
|---|---|---|
| Path tracer d'exemple de NVIDIA (RTXGI) | fait, banc automatique (`tools/rtxgi/`) | — |
| Quake II RTX (`F:/Q2RTX-KRC`, GLSL, SHaRC officiel) | KRC d'origine intégré ; la bibliothèque est à brancher | 1 à 2 jours |
| Doom RT (PrBoom+ RT / RTGL1, GLSL) | KRC d'origine intégré, rééclairage exact | — |
| RTX Remix (`dxvk-remix`, open source) | à étudier : un seul branchement donne Portal, Half-Life 2, Morrowind… ; son cache actuel est NRC, il faudrait vérifier dans ses sources s'il a aussi SHaRC | 1 à 2 semaines |
| Unreal Engine 5 (sources accessibles) | à étudier, plus lourd (path tracer de référence, pas temps réel) | plusieurs semaines |
