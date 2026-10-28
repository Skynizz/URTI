# Feuille de route : de prototype à moteur de jeu

Objectif : intégrer KRC à un moteur de jeu en temps réel (budget typique pour l'éclairage
global : 2 à 4 ms par frame sur GPU, en 1080p à 1440p).

## Où on en est

| Brique | État |
|---|---|
| Algorithme (cache + incertitude + delta corrélé + dépendances pondérées) | validé sur scène jouet, CPU |
| Traçage des chemins sur GPU | ✅ validé (identique au CPU statistiquement) |
| Cache, allocation, événements sur GPU | ✅ pipeline entièrement GPU, stable sur 12 000 frames |
| Rayons matériels (cœurs RT) | ✅ ray query Vulkan, BLAS/TLAS, deux TLAS (avant/après) |
| Géométrie réelle (triangles, BVH) | ✅ Cornell box, Sponza (262 k triangles) |
| Comparaison en ms contre un cache standard | ✅ KRC v2 contre le vrai SHaRC : 2 à 4× moins d'erreur à temps égal (Sponza), 20× (Cornell) ; ⚠️ statique en caméra mobile et coût minimal en faveur de SHaRC |
| Absence de biais sur radiance à queue lourde | ⚠️ deux biais corrigés ; ~3 % résiduel en statique, retour lent après extinction |
| Caméra mobile | ✅ qualité ; ⚠️ coût minimal 3 ms à 1280×720 (KRC v2) |
| Spéculaire | ✅ matériaux PBR (GGX), reflets par pixel lus dans le cache, MIS (RESULTATS §16–17) |

## Analyse (octobre 2026) : optimiser KRC, réduire le bruit, le tester en jeu

Tirée des mesures des trois bancs (Quake II RTX, Doom RT, SDK de NVIDIA ; RESULTATS §18 à §21).
Classée par gain attendu sur l'image finale, à coût d'effort égal.

**Bruit et justesse de l'image**

1. **Rapport par pixel pour l'historique du débruiteur** (1 jour). Aujourd'hui S vient de la
   cellule du premier point touché : moyenne entre partie au soleil et à l'ombre, d'où l'erreur
   restante après un changement (taches de soleil du salon ; Bistro de nuit assombri de 6 % de
   trop). Au premier point touché, la lumière directe de chaque groupe est connue exactement par
   pixel ; son ancienne valeur est sa nouvelle divisée par r_g. Rapport = somme lissée « après » /
   somme lissée « avant » (rapport de sommes, pas moyenne de rapports, sinon bruité). C'était le
   « rapport de la lumière directe » qui marchait dans Doom.
2. **Chemins corrélés pour les groupes inconnus** (2 à 3 jours). Une lampe qui s'allume depuis
   zéro, ou des lampes d'un groupe qui changent différemment, retombent sur le retard de SHaRC.
   Chaque échantillon de la mise à jour évalue la lampe avec son ancienne et sa nouvelle intensité
   (même tirage, MIS entre les deux lois pour qu'une lampe éteinte reste tirée) ; la différence,
   presque sans bruit là où rien ne change, corrige la moyenne de la cellule.
3. **Allocation adaptative des chemins de mise à jour** (2 jours). SHaRC met à jour 1 pixel sur
   25, uniformément. Les moments de KRC-A donnent déjà la variance par cellule : répartir le
   budget en 30 % uniforme + 70 % Neyman, comme dans Doom (bruit brut plus faible là où il compte,
   sans les zones sombres de Neyman seul).
4. **Perte d'énergie du NRD de l'exemple** (1 jour d'enquête). 9 à 15 % hors ciel pour toutes les
   méthodes, même sans cache. La corriger améliore toutes les images et rend la comparaison juste
   (un cache trop clair est aujourd'hui récompensé). Pistes : tirage probabiliste diffus/spéculaire
   au premier rebond, démodulation par l'albédo.
5. **Lumière directe à nombreuses lampes : ReSTIR DI** (NVIDIA RTXDI). Avec plusieurs lampes, le
   bruit de l'image vient surtout du direct (l'exemple tire 8 candidats sans réutilisation). Pas
   propre à KRC, mais indispensable dans un vrai jeu.
6. Abandonné : guidage du premier rebond (aucun gain mesuré, +2 à +3 ms).

**Coût** (KRC-L +0,01 à +0,05 ms, KRC-A +0,24 ms sur SHaRC 3,85 ms, salon 1280×720)

1. Fait : les passes par entrée ne parcourent plus la table en écrivant (−2 ms).
2. Insertion faite deux fois par sommet (SHaRC, puis KRC pour retrouver la même cellule) : demander
   à SHaRC l'indice et la décision de relecture, une sonde de moins.
3. Passe de lissage de S (81 prises, pleine résolution) : seulement après un premier rééclairage ;
   pour des lampes qui clignotent en permanence, la passer en demi-résolution ou séparable.
4. Taille de table : 4 M d'entrées dans l'exemple ; 1 M suffit souvent, et divise par 4 le coût
   des passes par entrée (pour SHaRC aussi).

**Tester en jeu** : KRC est maintenant une extension de SHaRC en bibliothèque indépendante du
moteur (`krc_sdk/include`, mode d'emploi docs/INTEGRATION.md : cinq points d'accroche).
1. Quake II RTX (SHaRC officiel déjà branché) : remplacer notre KRC maison par la bibliothèque,
   pour que les trois bancs testent le même code.
2. RTX Remix (`dxvk-remix`, open source) : un seul branchement pour Portal, Half-Life 2 et
   d'autres ; vérifier d'abord dans ses sources quel cache il utilise (NRC annoncé dans la 1.0).
3. Unreal Engine 5 : plus lourd, plus tard.

## Vers de vrais jeux (après la vitrine, RESULTATS §17)

**Fait (RESULTATS §18)** : KRC et le SDK officiel de SHaRC tournent dans Quake II RTX, à temps
égal, sur une vraie partie enregistrée. **Fait (RESULTATS §20)** : Doom RT (RTGL1), à coût égal
de SHaRC ; rééclairage exact par groupes de lampes et historique du débruiteur rééclairé : KRC
nettement devant après un changement de lumière. À faire : la fin de convergence après un
changement (la part inexpliquée traîne), les scènes éclairées par des surfaces émissives (les
échantillonner directement, guidé par les cellules), puis Portal / RTX Remix.

KRC ne peut pas être « injecté » dans un jeu fermé comme Cyberpunk 2077 : son moteur
(REDengine, DX12) calcule lui-même son éclairage et n'offre aucun point d'entrée pour le
remplacer (le path tracing de Cyberpunk est le code de NVIDIA, intégré par CD Projekt). Les
voies réalistes, dans l'ordre :
1. **Plugin Bevy** (Rust + wgpu, la même pile que ce prototype) : KRC comme passe d'éclairage
   global d'un moteur de jeu ouvert, avec ses G-buffers, ses lumières et sa caméra. C'est la
   démo jouable.
2. **RTX Remix** (runtime `dxvk-remix`, open source) : il remplace le rendu de vieux jeux
   DirectX 8/9 (Portal, Half-Life 2, Morrowind…) par un path tracer, et son cache de
   radiance est justement SHaRC/NRC. Y brancher KRC à la place permettrait de comparer
   les deux dans de vrais jeux.
3. **Unreal Engine 5** (sources accessibles) : en plugin de rendu, à côté de Lumen.
Prérequis communs : HLSL/DX12 ou Vulkan natif (portage du WGSL), budget 2 à 4 ms en 1440p,
lumières nombreuses (ReSTIR pour le direct), objets animés (BLAS mises à jour).

## Après le test « comme en jeu » (RESULTATS §14)

KRC v2 garde l'avantage dans un pipeline de jeu avec débruiteur (1,4 à 5,7× à temps égal
sur les séquences). Défauts à régler avant une démo :
1. **Taches après un changement de lumière** : réduites (RESULTATS §15 : cellules
   « total + part directe », 32 chemins corrélés), pas disparues. Corrections par cellule
   encore bruitées (8 chemins corrélés au départ). Pistes : plus de chemins corrélés pour les lampes, débruiter le
   champ des corrections (les deltas de cellules voisines sont corrélés), priorité
   d'échantillonnage aux cellules corrigées.
2. Dérive sombre après l'extinction (0,73).
3. Statique Sponza : SHaRC un peu meilleur (1,2 à 1,35×).
4. Puis tests 2 à 4 : retard et scintillement en caméra mobile, scènes de jeu (beaucoup
   d'objets et de lumières), budget 1080p.

## Prochaine décision (après KRC v2)

KRC v2 bat SHaRC sur les séquences dynamiques. Pour aller plus loin :
1. Statique en caméra mobile : la phase statique reste ≈2× en faveur de SHaRC en
   1280×720 (ses voxels existent déjà hors champ quand une surface apparaît).
2. Coût minimal de KRC (~3 ms en 1280×720) et pics d'événement (p95 5 ms).
3. Démo dans un moteur (Bevy).

### Ancienne décision (avant KRC v2)

1. Tester le mode « éclairage réactif » de SHaRC : si KRC garde son avantage sur les lampes,
   la piste continue.
2. **Hybride** : SHaRC officiel pour l'état stable, et seulement les corrections corrélées
   de KRC (graphe de dépendances, chemins corrélés) appliquées à ses voxels lors des
   événements.
3. À défaut, réduire le coût fixe de KRC : insertion fusionnée avec la recherche, résolution
   limitée aux cellules vues ou échantillonnées, éviction sans pierres tombales.

## Étapes, dans l'ordre

1. **Cache résident GPU.** Table de hachage des cellules en buffer GPU (comme SHaRC) :
   accumulateurs (n, moyenne, M2), bitset de régions vieilli, compteurs de sommets par
   super-région. Les chemins fusionnent leurs résultats par `atomicAdd`/`atomicOr`.
   Le CPU ne reçoit plus que des agrégats (nombre de suspects, coût). Critère de sortie :
   le run `krc` sur GPU reproduit les métriques CPU, et le coût par frame est mesuré en ms.
2. **Allocation sur GPU.** L'allocation de Neyman gloutonne devient une passe parallèle :
   priorité par cellule, puis préfixe pour distribuer le budget. L'événement (suspects, chemins
   corrélés) devient un compute shader.
3. **Rayons matériels.** Maillages triangulaires + BLAS/TLAS, ray query dans le shader.
   Ancienne et nouvelle scène = deux TLAS (les moteurs gardent déjà les transformations de la
   frame précédente pour les motion vectors).
4. **Caméra mobile.** Les cellules sont dans l'espace monde, donc elles survivent au mouvement
   de la caméra ; il faut gérer l'apparition de nouvelles cellules (désocclusion) et la taille de
   cellule qui dépend de la distance.
5. **Spéculaire.** Tracer à travers les surfaces brillantes jusqu'au premier sommet diffus, puis
   lire le cache (comme NRC/SHaRC).
6. **Intégration moteur.** Plugin pour un moteur ouvert (Bevy en Rust avec wgpu, ou Godot), puis
   comparaison avec la solution native du moteur, à qualité et à coût égaux.

## Risques connus

- **Mémoire** : ~600 octets par cellule aujourd'hui (bitset de 432 octets + accumulateurs).
  Pour 1 M de cellules, c'est 600 Mo, trop. Le bitset exact doit devenir hiérarchique ou
  être remplacé par les seuls compteurs de super-régions (72 octets), en gardant le bitset fin
  pour la géométrie seulement.
- **Coût d'un événement de lumière** : même pondéré par l'énergie, allumer une lampe forte
  reste un vrai changement global. Il faudra l'étaler sur plusieurs frames (budget plafonné,
  cellules les plus visibles d'abord).
- **Radiance à queue lourde** (petites lampes vives, indirect) : l'allocation adaptative et
  les chemins corrélés y sont biaisés vers le sombre (RESULTATS §11). Il faudra une variance
  d'allocation robuste et un échantillonnage explicite des lampes changées.
- **Scènes très dynamiques** (feuillage, foule) : beaucoup de petits changements simultanés ;
  il faudra mesurer quand le delta corrélé cesse d'être rentable face à l'hystérésis simple.
