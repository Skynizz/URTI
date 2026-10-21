# Hypothèse : rendu incrémental piloté par l'incertitude

## 0. L'idée de départ (H0)

> Arrêter de calculer des images et entretenir une *connaissance* de la lumière.
> Chaque rayon est une mesure qui met à jour ce qu'on sait de la lumière dans la scène.

Trois briques :

1. **Cache de lumière avec incertitude** : cellules 3D, chacune stocke une radiance
   estimée et une variance.
2. **Allocation des rayons vers l'incertitude** : chaque frame, le budget de rayons
   va aux cellules visibles les moins sûres.
3. **Graphe de dépendances "comme un compilateur"** : chaque cellule retient quels
   *objets* ses rayons ont touchés. Quand un objet bouge, on n'invalide que les
   cellules qui en dépendaient.

Promesse : une scène statique converge et finit par ne plus rien coûter, et une
scène dynamique coûte en proportion de ce qui change.

## 1. Remise en question

### 1.1 « Objets touchés » est une dépendance fausse (faux négatifs)

Une empreinte des objets *touchés* ne capture que la moitié des changements :

| Changement | Les rayons passés ont-ils touché l'objet ? | Détecté par H0 ? |
|---|---|---|
| Un objet s'en va (désocclusion) | oui | ✅ |
| Un objet **arrive** sur un chemin qui était libre (nouvelle ombre) | **non** : le rayon passait *à côté* | ❌ |
| Une lampe **s'allume** | **non** : une lampe éteinte n'est jamais échantillonnée | ❌ |
| Une lampe s'éteint | oui (rayons d'ombre vers elle) | ✅ |

Ce qui détermine la lumière, ce ne sont pas les objets touchés : c'est **l'espace
traversé**. Proposition H1 : chaque cellule mémorise l'ensemble des *régions
grossières de l'espace* que ses chemins ont traversées (segments de rebond **et**
rayons d'ombre, voxelisés en DDA). Un changement est décrit par une région (AABB
balayée ancienne ∪ nouvelle d'un objet qui bouge). Pour une lampe qui s'allume, on
envoie quelques **rayons-sondes depuis la lampe** et on prend les régions qu'ils
touchent : c'est une propagation *avant* du changement, l'idée de *Selective Photon
Tracing* (Dmitriev 2002) appliquée à un cache côté caméra.

### 1.2 L'invalidation binaire coûte trop cher (faux positifs)

Une dépendance spatiale binaire est conservatrice. Une cellule au fond de la pièce A
dont *un seul* chemin sur mille est passé par la porte vers la pièce B est marquée
dépendante de B. Si on jette son historique quand une lampe s'allume dans B, on
paie une reconvergence complète pour un changement qui vaut peut-être 0,1 %.

Proposition H2 : la dépendance ne décide pas, elle **désigne des suspects**.
Une cellule suspecte garde son estimation et reçoit un petit budget de
vérification (≈ 8 échantillons frais). Un test statistique (z-test sur la
luminance, ancienne moyenne contre échantillons frais) tranche :

- **cohérent** : on fusionne et on garde l'historique. Coût total : 8 rayons.
- **incohérent** : on jette l'historique et on repart des échantillons frais.

Avant le test, le poids de l'historique est plafonné (N_cap = 64 échantillons).
Ainsi, un petit changement sous le seuil de détection est tout de même absorbé en
quelques dizaines d'échantillons, au lieu de rester biaisé pour toujours.

### 1.3 Les cascades : les tronquer par l'erreur, pas par la profondeur

Les dépendances spatiales couvrent déjà le premier ordre (ombre directe, lumière
directe) et une bonne partie du second (les chemins qui rebondissent dans la zone
changée l'ont traversée). Au-delà, H3 : un **filet de sécurité statistique**. Chaque
cellule compare ses 32 derniers échantillons à sa moyenne long terme (z > 4 :
l'historique est jeté), et un plancher d'exploration donne au moins 1 échantillon
toutes les 16 frames à chaque cellule visible. La cascade est donc propagée
*tant que l'erreur induite dépasse le bruit*, ce qui est le bon critère d'arrêt.

### 1.4 Une seule monnaie : l'incertitude

On unifie les trois briques :

- l'allocation minimise l'erreur image attendue. À budget fixe, c'est l'allocation de
  **Neyman** : n_c ∝ √(empreinte écran) · σ_c, calculée de façon gloutonne via le gain
  marginal f·σ²·(1/n − 1/(n+1)) ;
- la variance est estimée avec un **a priori bayésien** (σ² médian des cellules
  mûres, poids κ = 4). Sans cela, une cellule dont les 2 premiers échantillons
  valent 0 ne serait plus jamais échantillonnée ;
- en mode « qualité cible », on n'échantillonne que les cellules dont l'erreur relative
  prédite σ/√n dépasse τ. **C'est ce mode qui fait tendre le coût d'une scène
  statique vers ~0.**

### 1.5 Ce qu'on ne traite pas (encore)

- **Spéculaire** : le cache est diffus (radiance sortante indépendante de la vue).
  Les surfaces brillantes devront être tracées au travers (comme NRC/SHaRC) ou
  stockées dans une base directionnelle. Ce point est hors du périmètre v1.
- **Mémoire** : prototype = 512 bits de régions + 3 accumulateurs par cellule
  (~200 o). Une version GPU utiliserait un filtre de Bloom hiérarchique ou des
  régions creuses.
- **Biais de discrétisation** : une cellule moyenne la radiance sur 2 à 4 pixels, ce
  qui adoucit les ombres nettes. On le mesure.

## 2. Hypothèse testée (H*)

> Avec un cache spatial de radiance où **l'incertitude est la seule monnaie**
> (allocation de Neyman, a priori bayésien), des **dépendances spatiales**
> (régions traversées) qui *désignent des suspects*, et une **vérification
> statistique** qui décide de l'invalidation :
>
> 1. à budget de rayons égal, l'erreur sur une séquence dynamique est plus faible que
>    (a) l'accumulation par pixel avec reset, (b) l'accumulation temporelle
>    exponentielle, (c) un cache à hystérésis type SHaRC/DDGI, et (d) un cache
>    adaptatif avec reset global ;
> 2. les dépendances spatiales battent les dépendances « objets touchés » (H0) ;
> 3. la vérification bat l'invalidation dure ;
> 4. en mode qualité cible, le coût par frame d'une scène statique tend vers ~0, et
>    après un changement il est proportionnel à la zone affectée.

## 3. Protocole

Scène : deux pièces reliées par une porte (vue « maison de poupée »). Séquence de 120 frames :

| Frames | Événement |
|---|---|
| 0–39 | statique |
| 40–63 | une boîte se déplace dans la pièce A (12 pas) |
| 64–79 | statique |
| 80 | une lampe puissante **s'allume** dans la pièce B |
| 100 | le plafonnier de la pièce A **s'éteint** |

Budget : 1 chemin par pixel de surface et par frame, pour toutes les méthodes. Les
rayons primaires (G-buffer) sont gratuits et identiques partout.
Métrique : relMSE et RMSE contre une référence à 4096 spp par pixel, pour chaque
état de la scène.

Méthodes :

| Nom | Description |
|---|---|
| `pt_reset` | accumulation par pixel, reset total (oracle) à chaque changement |
| `pt_ema` | accumulation par pixel exponentielle α = 0,1 (style TAA) |
| `cache_ema` | cache spatial, allocation uniforme, hystérésis (style SHaRC/DDGI) |
| `cache_reset` | cache + Neyman + reset global (oracle) à chaque changement |
| `krc_nodep` | H* sans graphe de dépendances (filet statistique seul) |
| `krc_obj` | H* avec dépendances « objets touchés » (H0) |
| `krc_hard` | H* avec dépendances spatiales mais invalidation dure |
| `krc` | **H\*** complet |

## 4. Travaux voisins

- Ward et al. 1988, *irradiance caching* : cache de surface et interpolation.
- Walter et al. 1999, *Render Cache* : réutilisation d'échantillons entre frames.
- Dmitriev et al. 2002, *Interactive Global Illumination using Selective Photon
  Tracing* : re-trace sélectivement les photons dont les chemins touchent la zone
  modifiée. C'est l'ancêtre direct de la brique 3, côté lumière.
- Binder et al. 2018/2019, *path space filtering* par hachage spatial jitteré :
  c'est notre structure de cellules.
- Müller et al. 2017, *Practical Path Guiding* ; Müller et al. 2021, *Neural Radiance
  Caching*.
- Majercik et al. 2019, *DDGI* (sondes + hystérésis) ; NVIDIA SHaRC (cache par
  hachage spatial).
- Schied et al. 2018, *A-SVGF* : rejet d'historique par gradients temporels, l'analogue
  écran de notre test de vérification.
- Brouillat et al. 2009, Marques et al. 2013, *Bayesian Monte Carlo* pour l'illumination
  globale.
- Lumen (Unreal Engine 5) : *surface cache* avec mises à jour budgétées.
