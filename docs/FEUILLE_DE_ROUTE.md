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
| Spéculaire | à faire |

## Après le test « comme en jeu » (RESULTATS §14)

KRC v2 garde l'avantage dans un pipeline de jeu avec débruiteur (1,4 à 5,7× à temps égal
sur les séquences). Défauts à régler avant une démo :
1. **Taches après un changement de lumière** : corrections par cellule trop bruitées
   (8 chemins corrélés). Pistes : plus de chemins corrélés pour les lampes, débruiter le
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
