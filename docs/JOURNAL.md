# Journal de bord

## 2026-10-21 — Démarrage

- Idée initiale reçue (H0 : cache + incertitude + graphe de dépendances par objets).
- État de l'art : l'ancêtre le plus proche de la brique « dépendances » est
  *Selective Photon Tracing* (Dmitriev et al., EGWR 2002), qui re-trace côté lumière
  les photons des zones modifiées. Les caches temps réel (SHaRC, NRC, DDGI) gèrent le
  dynamique par hystérésis ou apprentissage continu, sans dépendances explicites.
- Critique de H0 et reformulation en H* : voir [HYPOTHESE.md](HYPOTHESE.md).
  1. Dépendances par *régions traversées* plutôt que par *objets touchés*
     (faux négatifs : nouvelle ombre, lampe allumée).
  2. Les dépendances désignent des suspects, un test statistique décide
     (contre les faux positifs).
  3. Les cascades sont tronquées par l'erreur (filet statistique), pas par la profondeur.
  4. Une seule monnaie : l'incertitude (Neyman + a priori bayésien).
- Choix techniques : Rust (CPU, zéro dépendance, multi-thread `std::thread::scope`),
  scène de boîtes alignées sur les axes, NEE sur lampes rectangulaires, 192×96 px.

## v1 — premier prototype (commit 23e1272)

- **Bug : famine de l'a priori.** Après un événement, presque toutes les cellules sont
  suspectes. Les seules cellules « mûres » restantes sont les faces tournées vers le
  vide (σ = 0), donc l'a priori vaut 0, les cellules invalidées ont un gain nul et ne
  sont plus jamais échantillonnées : l'image gèle. Correctif : a priori calculé sur
  *toutes* les cellules mûres, avec un plancher de 0,1. C'est exactement le piège que
  l'a priori bayésien devait éviter.
- **Bug : régions mal alignées.** Une région de 0,75 m chevauchait les deux faces de
  la cloison et créait de fausses dépendances entre les pièces. Correctif : régions de
  0,6 m, la cloison tombe sur une frontière.
- **Résultat négatif : le z-test de vérification est sous-puissant.** Avec 8
  échantillons de path tracing (variance relative ~2 par échantillon), on ne détecte
  même pas une pièce qui s'éteint : la cellule est confirmée à tort et garde l'ancien
  éclairage (`krc_ztest`, lampe éteinte : 9,5e-2 contre 6,5e-3 pour l'invalidation
  dure). *Tester* si une cellule a changé avec des échantillons indépendants est une
  impasse.

## v2 — mesurer le changement au lieu de le tester (commit c07d5dc)

- **Pivot (H\*\*) : mise à jour différentielle par chemins corrélés.** On retrace les
  mêmes chemins (même graine) dans l'ancienne et la nouvelle scène. Un chemin qui ne
  touche ni la zone changée ni une lampe changée est *identique* : Δ = 0 exactement,
  sans second tracé. On *corrige* l'estimation (moyenne += Δ̂) au lieu de la jeter.
  C'est le vrai analogue de la compilation incrémentale : on applique un diff.
- Une pmf de lampes « union » (indépendante de l'état allumé/éteint) est nécessaire
  pour que les deux scènes consomment les mêmes nombres aléatoires.
- **Bug : dépendance des rayons d'ombre.** On enregistrait le segment jusqu'à la lampe,
  même quand le rayon était bloqué. Correctif : segment jusqu'à l'occulteur.
- **Variable de contrôle** : μ = x̄ − β(ȳ − μ_cache), avec β optimal estimé sur les
  paires. β = 1 donne le delta pur, β = 0 un redémarrage frais. En pratique, β optimal
  et β = 1 sont statistiquement indiscernables sur cette scène.
- Plusieurs graines (5) : les écarts entre variantes delta sont dans le bruit.
- **Constat important** : `krc_all` (delta sur *toutes* les cellules, sans graphe) est
  aussi précis que `krc`. La sortie anticipée exacte des chemins corrélés est
  elle-même un test de dépendance, par chemin et exact. Le graphe ne sert qu'à réduire
  le *coût* : O(cellules affectées) au lieu de O(cellules visibles).

## v3 — passage à l'échelle et événements de lampes (commit 3f0533c)

- Scène paramétrée par le nombre de pièces (2, 4, 8), à densité de pixels constante.
- **Changement de lampes seules** : la géométrie est identique, donc l'ancienne
  radiance se déduit du même chemin : L_old = L + Σ_j w_j C_j, où C_j est la
  contribution qu'aurait la lampe j si elle était allumée. Plus de second tracé, et le
  coût d'un événement de lampe est divisé par deux (20 k au lieu de 37,7 k chemins).
  Exactitude vérifiée chemin par chemin contre un retraçage complet.
- **Bug révélé par ce changement : la règle d'arrêt du m adaptatif est fausse pour les
  lampes.** Si 2 chemins n'ont pas *sélectionné* la lampe changée, Δ = 0 exactement et
  la cellule est déclarée inchangée : biais de +7,8 % dans la pièce A. La sélection NEE
  est aléatoire et indépendante de l'importance de la lampe pour la cellule, donc
  « Δ = 0 sur 2 tirages » ne prouve rien. Correctif : M_DELTA chemins pour toute
  cellule suspecte lors d'un changement de lampe. (Pour la géométrie, l'arrêt reste
  justifié : une cellule dont les chemins touchent rarement la zone balayée a un Δ
  petit, borné par cette fréquence.)
- Diagnostic de la traîne après l'extinction de A : ce n'est pas un biais (|biais| <
  0,6 %) mais de la variance dans la pièce A devenue sombre. Hypothèses testées et
  réfutées : variance par échantillon sous-estimée (A), m₀ trop petit (F, effet
  faible), n_eff trop grand (effet faible).
- Erreur de méthode, de mon côté : mes agrégats rapides lisaient `metrics*.csv`, qui
  contenait des graines laissées par une version antérieure du code. J'ai nettoyé et
  tout relancé ; les chiffres de RESULTATS.md viennent d'un run propre.

## v4 — vieillissement des dépendances, résultats consolidés (commit e307e72 et suivant)

- **Cascades mesurées** : à 8 pièces, allumer la lampe B rendait 80 % des cellules
  suspectes. Les dépendances binaires accumulées sur tout l'historique saturent : un
  seul chemin passé par deux portes suffit.
- **Vieillissement** : deux bitsets alternés couvrent les K à 2K derniers échantillons
  (K = 64). Effet net pour la géométrie (suspects à N = 8 : 1 070 contre 1 837) et pour
  l'extinction de A, mais pas pour l'allumage de B.
- Diagnostic (debug des régions de l'événement) : les sondes de la lampe B traversent
  réellement la porte et touchent les murs des pièces 0 et 2. Le graphe a raison
  *qualitativement* ; il lui manque la *magnitude*. Prochaine étape : des dépendances
  pondérées par l'énergie.
- Préchauffage de 150 frames pour le passage à l'échelle. Sans lui, les coûts mesurés
  en mode qualité cible à N = 8 étaient dominés par la convergence initiale.
- Résultats consolidés : voir [RESULTATS.md](RESULTATS.md).

## v5 — dépendances pondérées par l'énergie (commit 711c5d9)

- **Vérité terrain d'abord** : changement signé agrégé par pièce entre références
  (N = 8). Lampe B allumée : pièces 0 à 3 = +4,1 %, +153 %, +165 %, +1,1 % ; pièces 4 à 7
  < 0,1 %. Le graphe binaire rendait suspectes ~78 % des cellules *partout*. (Un
  premier calcul en |Δ| moyen par pixel donnait 2 à 5 % partout : c'était le bruit des
  références.)
- Sondes de lampe avec magnitude : ΔL_R ≈ ρ·kΦ/(P·π·A_R).
- **Impasse 1** : sketch haché sur 256 compteurs. Une cellule traverse 100 à 300
  régions par fenêtre, les collisions rendent toutes les fréquences élevées, donc tout
  devient suspect.
- **Impasse 2** : fréquences de *traversée* sur super-régions exactes. Les rayons qui
  s'échappent par la façade ouverte longent les autres pièces sans y rebondir.
- **Ce qui marche** : fréquences des *sommets* de chemin (là où la lumière est captée)
  par super-région (36 compteurs u8 par fenêtre), et critère « régions traversées ET
  Σ f_S·M_S > 1 % de la radiance ». Suspects par pièce : 100/100/94/53/8/1,5/0,3/0,6 %,
  ce qui colle à la physique.
- N = 8, lampe B allumée : 7 016 → 3 466 suspects, 56 k → 28 k chemins (−51 %), erreur
  équivalente.

## v6 — traceur GPU (commit b3660fe)

- RTX 4070, wgpu 24 sur Vulkan (ray query matériel disponible, pas encore utilisé).
- `trace.wgsl` : portage fidèle de `pt.rs`, interface `Tracer` commune CPU/GPU.
- Validation : relMSE GPU/CPU = bruit CPU/CPU ; chemins corrélés exacts sur GPU
  (≤ 2e-7) ; mêmes statistiques de dépendances.
- Débit : le calcul pur atteint 466 M chemins/s. Deux surcoûts trouvés et corrigés :
  recréer les buffers à chaque appel (~85 ms), et fabriquer un DepRec de 450 octets
  par résultat même sans dépendances (900 Mo pour 2 M chemins).
- **Leçon d'architecture** : avec dépendances, le gain n'est que de ×1,6, parce que
  relire les dépendances de chaque chemin coûte plus que les tracer. Pour du temps
  réel, le cache doit vivre sur le GPU et rien ne doit revenir au CPU par chemin.

## v7 — profilage, tout le pipeline sur GPU (commits 3bfd29d → 5a5bbac)

- **Profil d'un lot 1080p** (timestamps GPU + chronomètres par phase) : sans dépendances,
  le noyau prend 2,6 ms sur 113 ms. Plus de 97 % du temps est de la plomberie CPU↔GPU
  (préparation, envoi, relecture, conversion). D'où le portage complet.
- **Mémoire** : bitset binaire par cellule à 0,6 / 1,2 / 2,4 m (864 / 108 / 16 octets). La
  pondération par l'énergie fait le gros du tri : L2 (16 octets) ne coûte que +14 à
  +33 % de chemins par événement, et l'erreur reste dans les barres. L2 est retenu.
- **Pipeline entièrement GPU** (`krc_gpu.wgsl`, ~20 passes) : G-buffer, table de hachage
  (CAS en deux temps, sans attente active), événement, chemins corrélés en 2 tours,
  allocation, traçage avec fusion atomique, résolution, affichage. Le CPU n'envoie que la
  scène et l'événement.
- Bugs trouvés en route : remise à zéro des compteurs incomplète (2 groupes au lieu de 4),
  budget sous-consommé à 69 % (arrondi des quotas, puis sensibilité extrême de Σquota(λ) à
  l'état convergé : 3 balayages de 32 candidats), dispatch > 65 535 groupes.
- Optimisations mesurées : allocation par balayages parallèles (12× plus rapide qu'une
  bisection à un seul groupe), dispatch indirect sur le nombre réel de cellules, DDA
  grossier hors chemins corrélés, entrelacement des tâches contre la contention des
  atomiques.
- Validation : `gpu_krc` 4,19e-3 contre 4,11e-3 pour `krc_energy_l2` CPU ; `gpu_sharc`
  égal à `cache_ema` CPU.

## v8 — dérive longue (commits 9705364, 39ad338)

- 12 000 frames (100 cycles de la séquence). Aucune saturation, aucune croissance de la
  table, dette revenue à 0.
- Deux défauts trouvés et corrigés :
  1. Après un gros changement combiné (redémarrage de cycle : boîte + 2 lampes),
     l'erreur restait 3× plus haute pendant ~40 frames : le n_eff d'une estimation
     corrigée était trop optimiste. Plafond N_POST = 64 (balayage 32/64/128/sans).
  2. Une dérive lente (+3,1 % ± 1,3 % sur 100 cycles, t = 2,4) : historique plafonné à
     1 024 échantillons, ce qui la ramène à −0,8 % ± 1,4 %.

## v9 — triangles et ray tracing matériel, comparaison en ms (commits 6d2ae3f → …)

- wgpu 24 expose le ray query Vulkan (BLAS/TLAS) sur la RTX 4070.
- Shader en gabarit : grille propre à la scène et bloc de géométrie interchangeable
  (boîtes, ou triangles par ray query sur la TLAS courante et la précédente).
  Sondes de lampe et références calculées sur GPU (~1 G chemins/s).
- Cornell box en triangles, même séquence : `tri_krc` 5,03e-3 ± 4e-5 contre
  `tri_sharc` 3,91e-2 (7,8×).
- **Comparaison erreur/temps (1024×1024)** : à temps GPU moyen égal, KRC a 2,4 à 2,9×
  moins d'erreur que SHaRC ; à erreur égale, il est ~2× plus rapide. Mais ses frames
  d'événement montent à 12–16 ms (p95 ~13 ms), contre un coût constant pour SHaRC.
- **Résultat négatif : amortir les événements de lampe** (chemins corrélés plafonnés à
  1 chemin/px, cellules en attente) réduit bien le pic (16,3 → 8,7 ms), mais l'erreur
  monte de 4,7e-3 à 1,19e-2 : des cellules très fausses restent affichées pendant
  2 frames. Le p95 reste de toute façon fixé par les pas de la boîte. Option conservée,
  désactivée.

## v10 — Sponza, et deux biais que la relMSE cachait (commits c2293d4 → …)

- **Sponza** (glTF Crytek, 262 k triangles, échelle 0,008) : 4 lampes (une grande au-dessus
  de l'atrium, une petite lampe chaude dans la galerie, deux de chaque côté), une boîte
  de 1,4 m qui se déplace, même séquence de 120 frames. Références à 16 384 spp sur GPU
  (les petites lampes près des murs donnent des lucioles à 4 096 spp).
- **Métrique** : la relMSE brute de Sponza est dominée par quelques lucioles (une frame
  à 117 suffit à décider de la moyenne, écart-type entre graines ~70 %). Ajout de la
  relMSE sans le 0,1 % de pixels les plus fautifs (pratique du path guiding) : écart-type
  entre graines ~2 %, monotone avec le budget.
- **Biais trouvé en regardant les images** : 20 frames après l'extinction de la grande
  lampe, l'image KRC était 2× trop sombre et ne récupérait pas. La relMSE le récompensait
  (avec y² + 0,01 au dénominateur, une scène sombre favorise une image trop sombre).
  Nouvel indicateur : luminance totale image / référence. Commande `krc bias`.
  1. **Allocation** : depuis un cache vide, KRC statique tombe à 0,68 en 4 frames
     (SHaRC 1,00 ; KRC 100 % uniforme 0,98). Une cellule qui tire une valeur rare (queue
     lourde : petite lampe vive, indirect) voit sa variance exploser, est aussitôt
     sur-échantillonnée, et la valeur rare est diluée dans un n qui grandit *parce qu'elle
     a été vue*. Une part défensive uniforme n'y fait presque rien (50 % : 0,74).
     Correctif : variance relative d'allocation plafonnée à la médiane → 0,93 à f4,
     0,96–0,97 ensuite.
  2. **Événements de lampe** : à l'allumage de la petite lampe, 81 % des suspects n'ont
     vu aucune différence sur leurs 8 chemins corrélés et gardaient 1 024 échantillons sans
     la lampe. La sortie anticipée est exacte pour les chemins, pas pour l'espérance de la
     cellule. Correctif : ces cellules gardent leur estimation avec un poids plafonné ;
     poids max d'une correction 16 (au lieu de 64) ; variance après correction au moins
     celle de l'historique.
- Prix des correctifs : relMSE* Sponza 1,85e-2 → 2,47e-2 (l'ancienne valeur profitait du
  biais), Cornell +8 %, scène de boîtes +13 % (4,14e-3 → 4,67e-3).
- **Résultat Sponza 1280×720** : à temps moyen égal, KRC a 2,7 à 2,9× moins d'erreur que
  SHaRC ; à erreur égale, il est 3,6× plus rapide. Pics : p95 9,6 ms contre 4,5 ms.

## v11 — caméra mobile et vrai SHaRC : l'avantage se réduit aux changements de lumière (commits 39b9eeb → …)

- **Caméra mobile** : une vue et une référence par frame ; KRC évince les cellules
  invisibles depuis 60 frames (même règle que l'exemple SHaRC), liste libre, reconstruction
  de la table. Il faut ~2 cellules par pixel : en marchant dans Sponza en 1280×720, KRC
  garde jusqu'à 1,35 M cellules vivantes (~460 Mo), contre 176 Mo fixes pour SHaRC.
- **SHaRC officiel** (NVIDIA RTXGI, v1.8.3) porté en WGSL, fonction par fonction, avec les
  paramètres de l'exemple de NVIDIA. Validé : avec des réglages généreux, il converge vers
  la référence (relMSE* 6,8e-4, lum 0,99, Cornell statique).
- **Correction d'une conclusion précédente** : les « 2,7 à 2,9× moins d'erreur que SHaRC à
  temps égal » (v9, v10) étaient mesurés contre mon cache SHaRC-like, qui traçait 1 chemin
  par pixel avec toute la machinerie de cellules de KRC. Le vrai SHaRC met à jour 4 % des
  pixels et coûte 0,8 à 3 ms là où KRC en coûte 4 à 22.
- **Résultats contre le vrai SHaRC** :
  - En statique (caméra fixe ou mobile), SHaRC est aussi bon ou meilleur que KRC pour 3 à
    25× moins de temps GPU.
  - Sur la séquence complète, KRC a 3 à 18× moins d'erreur, presque entièrement grâce aux
    changements de lumière : après l'extinction de la grande lampe, SHaRC reste 7× trop
    clair (lum 7,5) pendant des dizaines de frames, KRC est à 0,96.
  - Le mode « éclairage réactif » de SHaRC, prévu pour ce cas, n'a pas encore été testé :
    l'avantage de KRC sur les lampes reste à confirmer contre lui.
- **Coût de KRC en caméra mobile** : 12,6 ms par frame à 0,25 chemin/px en 1280×720, dont
  5,8 ms d'insertion dans la table (pierres tombales, 3,7 M insertions par frame) et 3,6 ms
  de résolution (toutes les cellules, visibles ou non, à chaque frame). Caméra fixe : 3,9 ms.

## v12 — KRC v2 : affiner KRC pour battre le vrai SHaRC (commits 7ecba68 → …)

Tests courts (`krc duel`, `krc statique` : 480×270 ou 256², 3 graines) pour décider vite.

- **Coût fixe** divisé par 3 en caméra mobile (12,6 → 4,4 ms en 1280×720) : insertion sans
  CAS inutile ni compteur partagé, liste des cellules visibles pour l'allocation et la
  résolution, éviction par balayage.
- **Repris de SHaRC, et utile** :
  - rééchantillonnage du cache : un chemin ordinaire s'arrête au 1er ou 2e rebond sur une
    cellule, en héritant de ses dépendances (les chemins corrélés restent complets) ;
  - cellules plus grandes (8 px au lieu de 4) ;
  - a priori d'une cellule neuve depuis le niveau voisin (caméra qui avance) ;
  - démodulation matériau.
- **Propre à KRC, et utile** : la qualité cible sur GPU. En statique, KRC cesse
  d'échantillonner les cellules convergées, alors que SHaRC dépense son budget à chaque frame.
- **Essayé, sans gain** : réutilisation des sommets, cellules de support hors champ
  (mémoire ×3, débordement), K = 2, cache à deux résolutions, enregistrement partiel des
  dépendances. Gardés en options désactivées.
- **Résultat** : à temps GPU égal, KRC v2 a 2 à 4× moins d'erreur que SHaRC sur Sponza
  (caméra fixe ou mobile), y compris contre un SHaRC réglé pour réagir vite (accumulation
  5 frames), et 20 à 27× moins sur la Cornell box. Reste en faveur de SHaRC : le statique
  avec caméra mobile en haute résolution (≈2×), et son coût minimal plus bas.

## v13 — Test 1 « comme en jeu » (commit d6e9be2 → …)

- Pipeline identique pour les deux méthodes : image finale par pixel puis débruiteur commun
  (SVGF simplifié : accumulation temporelle reprojetée, historique borné par la variance du
  voisinage, trois filtres à-trous). KRC : direct par pixel + indirect des cellules (les
  cellules ne stockent plus que l'indirect). SHaRC : sa passe Render officielle.
- Sans bornage de l'historique, le débruiteur traîne une lampe éteinte 2× trop claire
  pendant 10 frames et efface l'avantage de KRC : le bornage est indispensable (c'est ce que
  font A-SVGF et les débruiteurs de jeu).
- À temps GPU égal, image finale débruitée : KRC 1,4 à 3× moins d'erreur sur Sponza, 4,5 à
  5,7× sur la Cornell box. Statique : KRC meilleur sur Cornell (2,5 à 3,6×), SHaRC un peu
  meilleur sur Sponza (1,2 à 1,35× caméra fixe, à égalité caméra mobile).
- **À l'œil**, après l'extinction de la grande lampe de Sponza : SHaRC trop clair mais
  propre ; KRC plus juste en moyenne mais **taché** (chaque cellule corrigée par 8 chemins
  corrélés garde son propre bruit) et un peu trop sombre. C'est le prochain défaut à régler.

## v14 — corriger les défauts du test « comme en jeu »

- **Cause des taches trouvée** : en mode « indirect seulement », un chemin qui relisait une
  cellule au rebond suivant recalculait l'éclairage direct de ce sommet avec un seul tirage
  (la queue lourde des petites lampes). Correctif : les cellules gardent la radiance totale
  (relue comme le fait SHaRC) et la moyenne de leur part directe ; image finale = direct par
  pixel + (total − direct moyen). Lors d'un événement, la part directe est corrigée par les
  mêmes chemins corrélés.
- 32 chemins corrélés par cellule pour un événement de lampe (au lieu de 8).
- Testés sans gain net : cellules 12 et 16 px, pas de remboursement des événements, remise
  à zéro sur grand changement, débruiteur à 5 passages (floute trop sans arrêt sur la
  luminance).
- Bornage de l'historique du débruiteur : 3 écarts-types aide KRC sur Sponza mais le rend
  plus lent sur la Cornell box ; comparaisons faites aux deux réglages, pour les deux
  méthodes.
- Résultat : taches nettement réduites mais pas disparues ; dérive sombre après l'extinction
  0,74–0,81 (réglage du débruiteur) ; statique Sponza toujours légèrement en faveur de SHaRC.

## v15 — vitrine : Sponza texturée et brillante, vidéo comparative

- Matériaux PBR : vraies textures de Sponza (69, 512² avec mipmaps), BRDF lambertien + GGX,
  sol de pierre polie presque miroir, pierre satinée, métal, feuillages en masque alpha
  (texels transparents traversés), sphère chromée qui roule, sphère dorée, ciel au-dessus
  du toit qui « se couche », tone mapping ACES.
- Les deux méthodes dans le même pipeline de jeu : reflet par pixel (KRC relit ses cellules
  au point reflété, démodulées puis remultipliées par l'albédo exact : texture nette, lumière
  sans bruit ; SHaRC suit sa règle officielle d'empreinte du lobe), débruiteur commun avec
  canal spéculaire séparé et filtre anti-lucioles.
- Réglages à temps égal (~5,5 ms en 960×540) : KRC 0,25 chemin/px dans les cellules + 2
  estimations directes par pixel ; SHaRC 1 pixel mis à jour par bloc 2×2, 3 chemins/px.
- Corrections trouvées en regardant les images : écrêtage des lucioles dans les cellules à
  100× la moyenne (20× assombrissait), 16 chemins corrélés par cellule pour une lampe
  (pic de 19,7 → 12,6 ms à l'allumage).
- Faux problème écarté : la sphère miroir « mal placée » dans la référence (sa moitié basse
  reflète le sol et s'y fond).

## v16 — les points blancs de KRC dans la vitrine

- Diagnostic par composante (`krc vitrinedbg f` : indirect des cellules seul, direct + reflet
  seuls, image complète, avec et sans débruiteur) : les taches blanches venaient des
  **cellules** (une cellule qui tire une valeur rare énorme devient une tache de sa taille, que
  le débruiteur étale), pas de l'éclairage direct.
- Erreur de ma part trouvée en route : après le test des seuils, l'écrêtage des lucioles des
  cellules était resté désactivé (1e9) dans la vidéo précédente.
- Correctifs : moyenne tronquée des 4 cellules d'un pixel (sans la plus claire ni la plus
  sombre : une cellule aberrante isolée n'est jamais affichée) ; écrêtage à 100× la moyenne
  pour une cellule mûre, 20× pour une jeune (< 64 échantillons, juste après un événement) ;
  régularisation des chemins en mode jeu pour les deux méthodes (au-delà du premier sommet,
  rugosité ≥ 0,3 : les caustiques diffus → miroir → petite lampe, à l'origine des taches
  cyan sur le rideau bleu, disparaissent ; la référence reste exacte).
- Cellules de 12 ou 16 px : pas mieux.

## v17 — vitrine : bannières, luminosité et miroirs

Retours à l'œil : « le drapeau n'est pas pareil », « c'est plus sombre ». Quatre causes, toutes
dans ce qui entoure les caches (débruiteur, reflets), trois communes aux deux méthodes :

- **Bannières floues et ternes** (les deux) : le débruiteur divise la lumière par l'albédo
  avant de filtrer, avec un plancher à 0,05. Sur un rouge saturé (vert et bleu ≈ 0,02), le
  plancher laissait la texture dans la lumière filtrée : bordures dorées étalées, orangées.
  Plancher à 0,002 : bordures nettes, comme la référence.
- **Image trop sombre** (les deux) : le ciel vu dans les surfaces brillantes n'arrivait que
  par le tirage de lampe, presque impossible pour un lobe étroit (sol poli, sphère miroir).
  MIS (heuristique d'équilibre) au premier sommet entre tirage de lampe et rayon de reflet :
  luminance de 0,79 → 0,91 à f39, 0,98 à 1,05 ensuite (référence = 1).
- **Grande tache blanche sur la sphère qui roule** (les deux, révélée par le MIS) :
  l'historique spéculaire sans bornage traînait le reflet du ciel. Bornage 3×3 (γ = 2)
  rétabli ; il n'assombrit plus, car avec le MIS les lampes reflétées ne sont plus des
  valeurs rares.
- **Points blancs dans la sphère miroir** (KRC) : les reflets hors champ (décor derrière la
  caméra, sol caché par la sphère) étaient des chemins isolés, et le direct au point reflété
  un seul rayon d'ombre. Nouvelle passe `krc_refl`, avant le resolve : le rayon de reflet crée
  la cellule au point touché et la nourrit (puis 1 rayon sur 4 une fois mûre, pour suivre la
  lumière) ; un point non métallique est lu entièrement dans sa cellule (direct compris).
  La sphère de KRC devient nettement plus propre que celle de SHaRC.
- Taille des cellules : 16 px au lieu de 8 dans la vitrine (moins de taches basse
  fréquence, et moins cher) ; le temps gagné passe à 0,4 chemin/px.
- Mesure ajoutée : PSNR des images affichées. Le relMSE* pénalise énormément un pixel clair
  sur un fond sombre de la référence (bord d'un reflet du ciel) et presque pas un reflet
  manquant : avec le ciel enfin présent dans les miroirs, il monte pour les deux méthodes
  alors que l'image s'approche de la référence.

## v18 — dans un vrai jeu : Quake II RTX

- Jeu : Quake II RTX (NVIDIA, open source, path tracing Vulkan), branche `krc` de
  `F:/Q2RTX-KRC` (données du jeu : version 1.8.1, 3 niveaux shareware). La variable de console
  `pt_cache` choisit l'éclairage indirect : 0 jeu d'origine (un rebond tracé), 1 SHaRC (SDK
  officiel de NVIDIA v1.8.3 en GLSL, non modifié, règles de son guide d'intégration), 2 KRC.
  Le reste (direct, reflets, débruiteur ASVGF, matériaux) est celui du jeu.
- KRC dans le jeu : cellules en espace monde (~16 px à l'écran, par face), allocation de
  Neyman par pixel, liste de tâches compacte, rééchantillonnage sur les cellules mûres, et
  chemins corrélés sur les lampes animées du jeu (le moteur garde l'intensité de la frame
  précédente de chaque lampe : chaque échantillon évalue l'avant et l'après).
- Débruiteur intégré (idée de Skynizz) : l'indirect est reconstruit par KRC (4 cellules voisines,
  pondérées par leur confiance), sans historique ni filtre du débruiteur du jeu, qui
  réaccumulait l'indirect et lui rendait le retard que les chemins corrélés venaient d'enlever.
  Gain de temps faible ici (0,06 ms : le débruiteur filtre l'indirect à 1/3 de résolution,
  ce qui coûte, c'est le direct et les reflets à pleine résolution).
- Coût de KRC : 1,48 → 0,54 ms par frame en 1280×720 (une insertion par pixel, marquage sans
  atomiques contendues, réductions par sous-groupe, liste de tâches pour les chemins) ;
  0,1 chemin/px pour le même coût que SHaRC.
- Pièges : la pause du moteur est ignorée en timedemo (référence capturée sur l'écran de
  fin de démo, corrigé) ; le compilateur MSVC en français casse le suivi des en-têtes par
  Ninja (UBO incohérent entre fichiers C, image noire) ; l'exposition automatique se mêlait à
  la mesure du retard (figée pendant le test).

## v19 — reflets, débruiteur piloté par KRC, options NVIDIA

- Lieux à reflets : `krc_view x y z pitch yaw` (commande ajoutée au code du jeu) ; repérage des
  surfaces d'eau et de verre dans les cartes (`tools/q2rtx/find_reflective.py`) ; deux vues de
  `demo1` : salle rouge au sol d'eau, bassin extérieur. Mesure au même instant pour toutes les
  méthodes (jeu en pause, exposition figée) : sinon l'eau qui ondule faisait varier le PSNR de
  3 dB d'une session à l'autre (un premier « KRC gagne » n'était que ce bruit).
- Référence honnête : `pt_cache 3`, chemins complets à 8 rebonds sans cache, accumulés ; sans
  profondeur de champ, avec du vrai spéculaire partout. La référence du jeu ne trace qu'un rebond.
- Trois erreurs de KRC trouvées en comparant : (1) l'indirect fourni avec la normale pour
  direction (faux reflets du débruiteur trop forts) ; (2) puis direction mesurée par cellule
  (idée de l'option SH de SHaRC) ; (3) premier rebond tiré comme le jeu (loi « uniformish »),
  la convention que sa reconstruction en harmoniques sphériques attend. Historique des cellules
  4096 (les chemins corrélés le gardent à jour).
- KRC ↔ débruiteur : confiance (erreur relative σ/√n de la cellule) qui décide où filtrer
  l'indirect ; rapport « après / avant » de la lumière directe (chemins corrélés) qui rééclaire
  l'historique du débruiteur. Essayé et retiré : jeter l'historique là où la lumière change
  (bruit), garder l'historique rééclairé contre les gradients du débruiteur (le rapport mesuré
  est bruité avec des lampes qui clignotent).
- Options NVIDIA de SHaRC : pas de préréglages nommés dans le SDK ; FADE_ACCELERATION activé
  (sans effet mesurable sur la coupure) ; accumulation 5 frames : plus réactif, même plateau.

## v20 — Doom RT (PrBoom+ RT / RTGL1)

- Avant de s'y lancer (demande de Skynizz) : l'indirect de RTGL1 v1.1.1 est un chemin à deux
  rebonds par pixel, rangé en harmoniques sphériques, débruité par A-SVGF ; pas de cache. Les
  lampes de secteur de Doom sont des lampes sphériques dont la couleur suit le niveau de lumière
  du secteur, avec un identifiant stable : idéal pour le rééclairage exact.
- Intégration (branche `krc` de RTGL1) : SHaRC officiel, KRC (passes RtCache0..8), mode de
  référence ; outils pilotés par variables d'environnement : scénario de session (pause, mode,
  coupure de lampes, exposition, affichage de débogage), captures moyennées, temps GPU par étape
  (`tools/doomrt/bench.py`, `indirect_bias.py`).
- **Bug de RTGL1** : son échantillon de gradient relisait l'ancienne couleur des lampes ; le
  débruiteur ne voyait jamais un changement d'intensité (environ 100 frames de traînée dans le
  jeu d'origine). Corrigé pour toutes les méthodes.
- **Rééclairage exact par groupes de lampes** (idée de Skynizz) : groupe = lampes d'un secteur et
  de même forme ; le moteur calcule le rapport « après / avant » de chaque groupe, ou le déclare
  inconnu si ses lampes ne changent pas toutes pareil. Les cellules suivent leurs deux groupes
  principaux ; l'historique du débruiteur est rééclairé du même rapport (direct, indirect,
  spéculaire au point touché par le reflet), et le gradient en tient compte.
- Défauts de KRC trouvés et corrigés : rapport indirect mesuré sur la moyenne (bruité, faisait
  dériver l'historique du débruiteur jusqu'au blanc) ; **allocation de Neyman seule biaisée**
  (une cellule qui n'a pas trouvé de source rare ne reçoit plus de chemins : zones sombres à
  65 % de la référence ; 30 % uniforme + 70 % Neyman → 87 %) ; écrêtage des lucioles avec
  réserve ; direction de la lumière par canal (les couleurs n'arrivent pas du même côté).
- Coût ramené à celui de SHaRC : lumière directe en cache aux points des reflets (règle du cône
  de SHaRC), vue compacte des cellules, lecture au pixel sortie du shader des rayons (passe 8 :
  −0,3 ms, le code de KRC y prenait des registres), passe des pixels sur un pixel par bloc 2×2.
- Pièges de mesure : convention d'émission de RTGL1 (KRC_EMIT_PHYS) ; référence moyennée après
  l'écrêtage de l'image (remplacée par une accumulation dans le moteur) ; état de la pause qui
  varie d'une session à l'autre (comparer seulement dans une même session).
- Essayé sans gain : historique raccourci par le changement inexpliqué ; une seule cellule au
  point du reflet ; historique de 32 768 échantillons.

## v21 — bruit, biais, premier rebond tracé ; banc NVIDIA prêt

- **Mesure du bruit** (`tools/doomrt/noise.py` : 32 frames consécutives par méthode, scène figée) :
  KRC n'est pas plus bruité que SHaRC (grain E1M1 0,0042 contre 0,0062) ; son retard venait du
  **biais** (fuite de lumière autour des bords, ex. le panneau d'E1M3).
- **Premier rebond tracé** (`KRC_HYBRID` : 0 cellules au pixel, 1 tracé là où les cellules
  voisines se contredisent au-delà de leur incertitude, 2 toujours, comme SHaRC) ; cache de KRC lu
  au point touché, second rebond tracé si aucune cellule (point hors champ).
- Trois défauts trouvés en route : hasard blanc au lieu du bruit bleu de RTGL1 (4× plus de bruit
  après le débruiteur) ; cellule réutilisée en un point par sa valeur brute (pondérée par 1/cos)
  au lieu de la lumière reçue (évaluation de ses harmoniques avec la normale) ; groupe de lampes
  non envoyé par le jeu traité comme éteint (noircissement durable, la frame +128 à 25 dB).
- Résultat, mode 2 (E1M1 / E1M3) : biais 54,9 / 54,0 contre SHaRC 54,8 / 54,0, grain plus faible
  sur E1M1 ; réaction toujours meilleure (+2 à +4 dB jusqu'à +64). Mais E1M2 recule (41,5 contre
  47,5 en mode 0), fin de convergence sous SHaRC (47,4 contre 50,0 à +250) et +0,7 ms GPU.
  Le meilleur mode dépend de la carte : à trancher.
- Banc de référence : le SDK RTXGI de NVIDIA (path tracer d'exemple, SHaRC et NRC intégrés par
  NVIDIA, scène Bistro fournie) compile et tourne ici en DirectX 12 (F:\rtxgi\sdk).

## v22 — banc NVIDIA (RTXGI), ꟻLIP, cellules adaptatives, guidage

- **Banc de référence** : path tracer d'exemple du SDK RTXGI de NVIDIA (branche `krc` de
  F:\rtxgi\sdk), SHaRC et NRC intégrés par NVIDIA, scènes Bistro, salon, salle de bains. Mode sans
  fenêtre visible ni prise de focus (Donut patché, `KRC_HIDDEN`), scénario par frame, captures HDR,
  temps GPU (`tools/rtxgi/`). Piège : la référence accumulée avec jitter contre des frames NRD sans
  jitter (toutes les arêtes diffèrent) ; tout est maintenant au même échantillonnage.
- **ꟻLIP** (NVlabs/flip) en LDR et HDR, moyen sur 8 frames débruitées ; plus la variance brute avant
  NRD et le biais (moyenne de 512 frames).
- **KRC-A, cellules adaptatives** (idée de Skynizz) sur la table de SHaRC : mère deux fois plus grande
  par cellule fine, variance d'une frame à l'autre, rétrécissement de James-Stein décidé à la
  résolution et lissé. Biais du cache réduit (salon : ꟻLIP 0,066 contre 0,074, relMSE −12 %) et
  bruit brut un peu plus faible ; mais la frame débruitée par NRD est moins bonne (0,082 contre
  0,072) : NRD converge vers un résultat différent, cause encore inconnue. +0,3 ms.
- **Guidage du premier rebond** (lobe de von Mises-Fisher appris par cellule, mélange défensif
  50 %, MIS) : sans effet sur le bruit dans ces scènes (éclairage large, pas de petite source
  rare), +2 à +3 ms (atomiques 64 bits). Résultat négatif ; à retenter là où la queue lourde existe
  (Doom, Bistro de nuit) et en moins cher.
- **État de l'art** (docs/ETAT_DE_L_ART.md) : le guidage en table de hachage existe déjà (Real-Time
  Markov Chain Path Guiding, I3D 2025) ; le cache comme variable de contrôle avec correction sans
  biais aussi (Neural Two-Level Monte Carlo, 2025). Restent propres à KRC : chemins corrélés exacts,
  rééclairage par groupes de lampes et de l'historique du débruiteur, guide figé pendant un
  événement, taille des cellules par test statistique.

## v23 — banc NVIDIA : NRD compris, rééclairage exact, bibliothèque KRC

- **NRD et KRC-A** : KRC-A sans lecture de la mère redonne l'image de SHaRC ; mais la cause n'est
  pas la mère : le NRD de l'exemple perd 9 à 15 % d'énergie hors ciel, même en path tracing pur,
  et SHaRC (+5 % hors ciel) le compense. À exposition égale, KRC-A égale ou bat SHaRC.
  Anti-lucioles de REBLUR : sans rapport (bascule `nrdaf`).
- **Rééclairage exact dans le banc NVIDIA** (lampes de la scène en 3 groupes suivis) : parts des
  groupes par cellule, suivies le long des chemins de SHaRC avec ses poids (y compris quand un
  sommet relit le cache) ; passe `krcRelight` avant la mise à jour quand le moteur connaît le
  rapport. Image brute juste dès la frame 0 (salon, Bistro de nuit).
- **Historique de NRD** : la confiance d'historique de REBLUR ne suffit pas (historique ramené à
  une frame, pas à zéro). Ce qui marche : diviser l'entrée de NRD par le produit S des facteurs de
  la cellule du pixel et remultiplier sa sortie ; son historique ressort rééclairé sans y toucher.
  S lissé à l'écran (sinon les cellules se dessinent). Salon, coupure du soleil : ꟻLIP 0,094 dès
  la frame 0 contre 0,113 pour SHaRC, qui ne rattrape qu'à +128.
- **Bistro de nuit** : 4 lampes ajoutées par le banc (`lampadd`). Piège : l'exemple ne tire que
  la lampe 0 en lumière directe (`targetLight = 0`). Après NRD, gain faible et S assombrit 6 % de
  trop (rapport par cellule) : prochain correctif, rapport par pixel de la lumière directe.
- **Coût** : +2 ms au premier essai, ramené à +0,01–0,05 ms : les passes par entrée remettaient à
  zéro les 4 M d'entrées de la table à chaque frame ; elles ne nettoient plus qu'à l'éviction
  (même correctif pour KRC-A : +0,24 ms).
- **Bibliothèque** : `Libraries/Krc/include/KrcRelight.h` (GPU, conventions de SHaRC) et
  `KrcRelightHost.h` (CPU) ; l'exemple les utilise ; copie dans `krc_sdk/include`, mode d'emploi
  docs/INTEGRATION.md. Analyse des prochaines étapes : FEUILLE_DE_ROUTE, « Analyse (octobre 2026) ».

## v24 — vers un effet ReShade : labo de l'éclairage global à l'écran

- Question de Skynizz : un système universel qui ajoute le ray tracing à n'importe quel jeu, même
  DirectX 12 (Red Dead Redemption 2), avec un menu ImGui. Réponse : impossible en vrai path
  tracing (un jeu DX12 ne donne ni sa géométrie, ni ses matériaux, ni ses lampes ; RTX Remix se
  limite pour cette raison à DX8/9). Possible à l'écran via ReShade (DX9 à DX12, Vulkan, OpenGL,
  menu ImGui intégré), qui ne voit que l'image et la profondeur.
- Labo dans le banc NVIDIA (`tools/rtxgi/ssgi.py`, `Shaders/KrcScreenGI.hlsl`) : le « jeu » est la
  lumière directe seule (`direct=1`, G-buffer écrit), l'effet ajoute l'indirect, référence = path
  tracing complet. Effet : masque de visibilité (Therrien et al. 2023), mesure de GTAO dans la
  tranche, accumulation temporelle reprojetée, à-trous, radiance de la frame précédente (plusieurs
  rebonds). Ajout propre : le **ciel vu à l'écran** éclaire les directions non masquées (à l'infini,
  la direction depuis le pixel est celle de la caméra, dans le plan de la tranche).
- Premiers chiffres (1280×720) : salon (70 % d'indirect venant du ciel, en partie hors champ)
  ꟻLIP 0,563 → 0,513 avec G-buffer, 0,547 avec la seule image ; Bistro de jour 0,145 → 0,133.
  Coût 2 à 3 ms en pleine résolution : à passer en demi-résolution.
- Limite de fond : la lumière hors champ ; c'est là que viendrait le cache en espace monde de KRC.

## v25 — URTI, étape 1 validée dans Red Dead Redemption 2

- URTI (idée de Skynizz : injecteur de ray tracing universel) : add-on ReShade qui capte la géométrie
  d'un jeu DX12 sans toucher à son rendu. Pipeline « ombre » par pipeline du jeu (même vertex shader,
  sortie de flux de SV_Position, sans rastérisation), dessins rejoués pendant la capture, y compris
  `ExecuteIndirect` (intercepté directement) ; `ALLOW_STREAM_OUTPUT` ajouté aux signatures racines du
  jeu. Banc DX12 à géométrie connue : 99,80 % de couverture.
- RDR2 (DX12, OptiScaler, DLSS et génération d'images DLSS-G, 2560×1440, rendu 1706×960) : **97,5 à
  97,6 % de la profondeur du jeu reproduite** par la géométrie captée (3 captures, écart moyen 1e-4),
  personnages animés compris ; le critère de l'étape (> 95 %) est atteint. Scène : la scène de
  chargement du mode histoire, rendue par le moteur (~3 300 dessins, 520 000 à 650 000 triangles).
- Pièges du vrai jeu, un par essai : plusieurs appareils D3D12 (plantage à F9) ; une présentation
  sur deux sans dessin (génération d'images) ; budget de sommets trop large ; dessins successifs qui
  s'écrasent dans un flux partagé (barrière) ; profondeur jamais effacée de façon visible (copie de
  ReShade) ; passes d'éclairage plein écran prises pour de la géométrie (pipelines sans écriture de
  profondeur exclus).
- Tests menés sans Skynizz : lancement, menu (clic sur « Histoire » ; l'Entrée injectée est ignorée),
  captures par fichier déclencheur (`tools/reshade/urti_rdr2.ps1`). Mode histoire uniquement.
- Reste à faire pour l'étape 2 : caméra (matrices) pour passer en espace monde, géométrie
  persistante, BVH, ombres RT du soleil ; essai en monde ouvert (pas seulement la scène de chargement).

## v26 — URTI, étape 2a : la caméra retrouvée sans rien savoir du jeu

- Capture enrichie (fichier `.urti`) : signature et bornes de chaque dessin dans le flux (début et
  fin lus sur le compteur du GPU), profondeur. Analyse hors ligne `urti/tools/analyse.py`.
- **Auto-étalonnage** : w de l'espace de découpage donne la profondeur le long de l'axe de vue ; avec
  le champ de vision, chaque sommet devient un point de l'espace caméra ; un objet immobile vu dans
  deux frames doit bouger d'un seul mouvement rigide : on cherche le champ de vision le plus rigide
  (ajustement robuste, les objets qui bougent eux-mêmes sont écartés).
- Banc (caméra en orbite, vérité terrain) : 60,15° pour 60,16°, déplacement 0,390 pour 0,390.
- RDR2 en jeu (souris injectée pour tourner la caméra) : 36,95° et 36,50° sur les deux captures où la
  caméra tourne de plus de 1° ; 38,8 à 39,1° quand elle tourne à peine (mal contraint). Pas de vérité
  terrain dans le jeu ; la cohérence des estimations est le seul contrôle.
- Couverture en jeu, caméra en mouvement, sur 8 présentations : 70 à 94 % (la géométrie de plusieurs
  frames est comparée à la profondeur de la dernière) ; à refaire frame par frame.

## v27 — URTI, étape 2b : soleil retrouvé, premières ombres en ray tracing dans RDR2

- Le banc a maintenant un soleil et une passe de carte d'ombres, comme un jeu. L'add-on capte tous
  les flux de profondeur (écran et cartes d'ombres) ; le fichier `.urti` passe en version 2.
- **Direction du soleil** : le même maillage vu par la caméra et dans la carte d'ombres (projection
  parallèle) est relié par une application affine ; la direction qu'elle envoie sur l'axe de
  profondeur de la carte est celle du soleil. Ajustement par paire de dessins puis vote (un maillage
  figure dans plusieurs cascades d'un atlas). Banc : écart 0,01°. RDR2 : l'atlas 2048×8192 des
  cascades donne une direction nette (82 à 84 % des paires d'accord) ; les petites cartes (1024², 512²,
  64²) sont des vues verticales (pluie, occlusion), pas le soleil. Signe : celui qui laisse la scène
  éclairée (l'autre met tout à l'ombre, soleil sous l'horizon).
- **Ombres en ray tracing** (`urti/tools/urti_rtshadow`, DXR 1.1, requêtes en ligne) : géométrie de
  la dernière frame en espace caméra (linéaire en espace de découpage), structure d'accélération,
  pixel reconstruit depuis la profondeur du jeu (profondeur = A + B / w ajustés sur les sommets),
  rayon vers le soleil. Le résultat ne dépend pas d'une erreur de champ de vision (application
  affine : les intersections sont conservées). Banc : ombres portées justes. RDR2 (1,7 M triangles,
  BLAS 38 Mo) : ombres cohérentes (épaules éclairées, ombre portée du personnage, structure du quai) ;
  scène brumeuse, peu d'ombres nettes du jeu pour comparer.
- Champ de vision par auto-étalonnage dans RDR2 : instable (51,5° ici, 37° avant) quand la caméra
  tourne peu ; sans effet sur les ombres, mais à fiabiliser pour l'éclairage.

## v28 — URTI, ombres du soleil en ray tracing en direct dans RDR2

- Mode direct (`live=1` dans `urti.ini`, F7 : actives / coupées, F6 : réétalonner) : à chaque image,
  la géométrie de l'écran captée par flux de sortie devient une structure d'accélération (BLAS
  reconstruite, 4,6 à 8,4 M sommets en jeu), puis un rayon par pixel de la profondeur du jeu part
  vers le soleil (DXR 1.1, requêtes en ligne). L'effet `urti.fx` assombrit les pixels à l'ombre
  (force 0,55, écran partagé possible). Soleil et loi de profondeur réétalonnés toutes les 120 images.
- Photos : le jeu affiche en HDR10 ; elles sont lues dans le tampon d'affichage après les effets,
  PQ décodé, BT.2020 → BT.709, blanc à 203 nits. Corrigé aussi : F7 laissait l'ancienne image
  d'ombres affichée.
- RDR2 (gare de Saint-Denis, `results/urti/rdr2_direct/`) : les ombres RT tombent au même endroit que
  celles du jeu (auvent de la gare sur le sol), ce qui valide la chaîne (géométrie, soleil, profondeur).
  Mais le jeu a déjà ses ombres : l'assombrissement s'ajoute par-dessus et rend les zones déjà
  ombrées trop noires. Les faces tournées à l'opposé du soleil (wagons à contre-jour) sont aussi
  noircies alors que le jeu les éclaire déjà moins.
- Sous la pluie, ciel couvert : le jeu ne dessine pas de carte d'ombres du soleil, donc pas de soleil
  à retrouver et pas d'ombres RT (comportement voulu).
- Coût : environ 124 → 81 images par seconde (génération d'images DLSS active), soit −35 %.
- Suite : ne pas assombrir ce qui est déjà à l'ombre dans l'image du jeu (comparer la luminance aux
  voisins éclairés, ou n'agir que sur les pixels où l'ombre du jeu manque : lointain, petits objets),
  ignorer les faces dos au soleil (normales tirées de la profondeur), BLAS mise à jour plutôt que
  reconstruite pour réduire le coût.

## v29 — URTI : ombres RT stables dans RDR2, génération d'images, rechargement à chaud

- **Soleil mesuré à chaque image sur le GPU** : paires de dessins écran / carte d'ombres du soleil de
  même signature, ajustement affine par paire, vote ; 220 à 250 paires par image dans RDR2, mesuré
  sur 90 à 100 % des images. Plus d'étalonnage lourd périodique (seulement au départ et sur F6). Le
  sens du soleil vient de la convention de profondeur de la carte (fixée une fois), plus de
  l'image précédente : F7 off/on ne fait plus partir les ombres dans tous les sens.
- **Génération d'images (OptiScaler, DLSS-G)** : les images générées sont fabriquées à partir de
  l'image du jeu avant ReShade, d'où l'alternance « native / RT ». Les ombres sont maintenant posées
  par l'add-on sur chaque image présentée (300 / 300, dont ~140 générées), en lumière linéaire
  (HDR10 PQ décodé).
- **Ombre du jeu recalculée** depuis sa carte d'ombres (cascades repérées par leur viewport,
  application caméra -> carte mesurée à chaque image). Mode « seulement les ombres que le jeu n'a pas »
  et mode « ombres RT complètes » (plus contrastées), au choix dans le menu de ReShade (UrtiMode).
  Constat : les ombres de RDR2 sont déjà précises ; le RT apporte surtout le détail fin (roues,
  pattes du cheval) et le contraste en mode complet.
- Faces tournées à l'opposé du soleil (normale tirée de la profondeur) : pas d'ombre RT ajoutée en
  mode combiné (taches sur les façades à contre-jour supprimées). Quatre rayons par pixel dans le
  disque solaire (0,5°) et filtre 3×3 : bords d'ombre doux.
- RDR2 n'a pas de masque d'ombre séparé : relevé des passes (F8) avec instantanés pris à la fin de
  l'écriture de chaque image ; G-buffer trouvé (normales, matériaux) pour les reflets.
- **Tests rapides** : réglages (`urti.ini`) et shaders (`urti_shaders/*.cso`, `tools/reshade/urti_shaders.sh`)
  rechargés à chaud dans le jeu en ~2 s ; lancement piloté par le journal (`urti_rdr2_rapide.ps1`).
- Corrigés en route : `&` sur un `ComPtr` (le libère), paramètres de l'effet retirés par le
  compilateur de ReShade s'ils ne sont plus lus, barrières sans ressource, états ReShade -> D3D12.

## v30 — URTI : corrections du direct, premiers reflets RT

- Ombres figées à l'écran quand la caméra bouge : le masque était posé même quand il n'avait pas pu
  être recalculé (étalonnage, profondeur absente, image du jeu prise pour une image générée). Le
  masque n'est plus appliqué que s'il date de cette image ou de la précédente ; image générée =
  présentation sans aucun dessin du jeu.
- Menu de ReShade : l'effet URTI décoché ou les effets coupés suppriment les ombres RT (l'add-on les
  posait lui-même sans en tenir compte) ; mode, force, écran partagé lus et journalisés.
- Sans soleil (nuit, intérieur) : étalonnages espacés (1×, 2×, 4×… l'intervalle), loi de profondeur
  gardée seule : les reflets marchent aussi la nuit.
- **Reflets RT** : un rayon réfléchi par pixel lisse (normale tirée de la profondeur), point touché
  reprojeté et vérifié visible, couleur lue dans l'image, Fresnel. Validé sur le banc (sol qui reflète
  les cubes, `results/urti/banc_direct/reflets_rt.png`). Dans RDR2, le G-buffer est repéré et son
  image de matériau copiée à la fin de son écriture à chaque image ; reste à savoir quel canal porte
  le lissé (balayage des canaux par rechargement à chaud, à refaire devant de l'eau ou une rue
  mouillée).
- GPU perdu dans RDR2 : la composition lisait le tampon des reflets coupés au-delà du tampon lié à sa
  place ; une lecture hors d'un tampon racine n'est pas bornée. Corrigé.

## v31 — URTI : capture sans attente, stabilité temporelle (étapes 1 à 4)

- Capture : une zone du flux et un compteur par dessin (plus de barrière par dessin) : de ~20 à ~50 i/s
  dans RDR2. Zones arrondies à trois sommets (sinon les triangles se formaient à cheval sur deux objets :
  grands triangles au sol qui apparaissaient et disparaissaient).
- Démarrage instantané : étalonnage stocké (`urti_calib.ini` : convention et description de la carte
  d'ombres, convention des vecteurs de mouvement), loi de profondeur mesurée à chaque image sur le GPU,
  emplacement réservé à la carte d'ombres du soleil.
- Soleil : vote affiné à 0,75°, saut faible rejeté, grand saut accepté seulement s'il se confirme.
- Vecteurs de mouvement du jeu copiés depuis son G-buffer, convention trouvée seule (16 candidates, par
  la profondeur de l'image précédente) : RDR2 = pixels, sens direct.
- Ombres : passe de débruitage séparée (flou 3×3 guidé par la profondeur, historique reprojeté, bornage
  moyenne ± 1,5 σ, mélange adaptatif 0,1 à 0,5 selon le mouvement), motif des rayons tournant d'une image
  à l'autre, ombre pondérée par N·L (ne retire que la lumière directe), effacée au loin (brume).
- Reflets : stabilisés (historique, bornage ± σ, mélange adaptatif, couleur plafonnée) et reprojetés sur
  le point reflété : rotation de la caméra mesurée d'une image à l'autre par les cascades (partie linéaire
  de L_préc⁻¹ L_cour, la translation des cascades ne la touche pas), translation déduite du vecteur de
  mouvement de la surface et de la profondeur virtuelle (distance + distance de touche).
- Neuf vues de diagnostic (menu ReShade et `debug_view`), images générées (DLSS-G) au choix.

## v32 — URTI : bac à sable « jeu AAA » (urti_sandbox)

Un mini-jeu D3D12 qui reproduit les conditions de RDR2, pour tester URTI en quelques secondes au lieu
de lancer le vrai jeu (`urti/sandbox/main.cpp`, `tools/reshade/urti_sandbox.sh`).

- Rendu comme un jeu récent : G-buffer différé à 2/3 de la résolution avec décalage sous-pixel (comme
  DLSS), mêmes formats que RDR2 (matériau RGBA8 avec le lissé dans le canal 1 de la cible 2, vecteurs de
  mouvement R16G16 en pixels), profondeur inversée à l'infini D32S8, profondeur à mi-résolution, atlas de
  4 cascades 2048×8192 D16 qui suivent la caméra, éclairage plein écran, mise à l'échelle, sortie HDR10
  (PQ). Signature racine sans stream-output, ExecuteIndirect, génération d'images en option.
- Scène difficile : rue, bâtiments à vitres lisses, flaques, poteaux et câbles fins qui pendent et se
  balancent au vent, arbres (branches et feuilles au vent), charrettes et passants qui avancent, clôtures
  (un piquet = un dessin), tonneaux. 226 objets, ~1 200 dessins par image. Caméra scriptée (marche,
  regards, demi-tours rapides), identique à chaque lancement ; cadence fixée à 90 i/s.
- URTI le reconnaît comme un jeu : loi de profondeur exacte (0,15 / w = plan proche), soleil sur 98 %
  des images, cascades, rotation de la caméra, vecteurs de mouvement dans la même convention que RDR2 (2).
- Photos à des présentations précises (`photo_frames=800-830` dans urti.ini) ; `--photos` fait deux
  lancements identiques (RT puis natif) et `urti_sandbox_compare.py` mesure l'effet du RT, le
  scintillement ajouté et la carte des points qui sautent. Référence : effet 2,1 %, scintillement ajouté
  +0,16 %, 0,9 % de pixels qui sautent, concentrés sur les reflets lointains dans les flaques — le
  défaut vu dans RDR2 (`results/urti/bac_a_sable/`).
- Test complet (deux lancements et mesures) : ~30 s.

## v33 — URTI : suite de 7 tests, vérité terrain, essai de l'ancrage géométrique

- Bac à sable plus proche d'un jeu : TAA (historique reprojeté, borné, mise à l'échelle) — l'image
  native est stable d'une image à l'autre comme dans RDR2 ; rue mouillée réglable (`URTI_SANDBOX_WET`),
  objets mobiles figés à la demande, mur pour l'occlusion, décalage sous-pixel
  désactivable (`URTI_SANDBOX_JITTER=0`).
- **Vérité terrain** (`URTI_SANDBOX_TRUTH=1`, `--verite`) : le sol reflète la scène exacte (rendue en
  miroir), mélangée avec la même règle qu'URTI (Fresnel f0 0,04, lissé > 0,35, plafond 4). On mesure
  l'écart des reflets RT à l'image idéale ; un reflet qui bouge normalement ne compte plus comme un saut.
- Nouvelle mesure : **pixels qui clignotent** (l'écart à la référence change de sens d'une image à la
  suivante), distincte d'une erreur qui se déplace avec la scène.
- `tools/reshade/urti_sandbox_suite.sh` : bâtiment immobile, arbre immobile, arbre au vent, personnage,
  caméra fixe, caméra en mouvement, occlusion (~3 min 30). Référence (`results/urti/bac_a_sable/suite/`) :

  | scène | écart | ajouté | sautent | clignotent |
  |---|---|---|---|---|
  | bâtiment | 3,31 % | +0,015 % | 0,012 % | 0,101 % |
  | arbre immobile | 2,97 % | +0,036 % | 0,084 % | 0,163 % |
  | arbre au vent | 2,94 % | +0,044 % | 0,086 % | 0,164 % |
  | personnage | 2,75 % | +0,033 % | 0,191 % | 0,093 % |
  | caméra fixe | 1,19 % | +0,049 % | 0,144 % | 0,118 % |
  | caméra en mouvement | 1,15 % | +0,044 % | 1,257 % | 0,871 % |
  | occlusion | 1,99 % | +0,024 % | 0,181 % | 0,093 % |

- Diagnostic sur la marche (le seul cas mauvais) : sauts dus pour moitié aux ombres RT (0,59 % sans
  elles), le reste aux reflets autour de l'horizon. Écartés par la mesure : le décalage sous-pixel (même
  résultat sans), le bornage, la tolérance de profondeur et la part d'historique (0,59 % ± 0,03 quel que
  soit le réglage), la synchronisation des deux lancements (vérifiée), la composition (test à vide :
  0,000 %). La vérité terrain a son propre crénelage (0,49 % de clignotement contre le natif) : c'est le
  plancher de la mesure.
- **Ancrage géométrique** des reflets (`anchor_alpha=`) : identité stable par dessin (signature + rang),
  sommets de l'image précédente gardés, miroir et point reflété retrouvés dans la géométrie captée (même
  triangle, mêmes barycentriques, déformations comprises), point virtuel de l'image précédente → endroit
  exact de l'historique, gardé seulement si le même objet se reflétait dans le même miroir. Fonctionne
  (deux tiers des pixels reflétants, `ancrage_couverture.png`) mais **ne réduit pas le clignotement** :
  0,909 % contre 0,871 % en marche, un peu moins bon caméra fixe (le passage d'un filtre à l'autre selon
  les pixels en ajoute). Gardé, coupé par défaut, sans coût quand il est coupé.
- Réglages d'essai dans urti.ini : `refl_alpha_min`, `refl_alpha_max`, `refl_gamma`, `refl_depth_tol`,
  `anchor_alpha`, `anchor_gamma`.

## v34 — URTI : ombres et reflets, l'un ne s'applique plus sur l'autre

- Question : les ombres RT tremblent-elles *à travers* les reflets ? La composition mélangeait le reflet
  puis assombrissait le tout par le masque d'ombre : une flaque à l'ombre voyait son reflet assombri
  (faux : l'ombre ne retire que la lumière directe de la surface) et le tremblement de l'ombre s'ajoutait
  à celui du reflet.
- Mesure par événement (pixel × image, `tools/reshade/urti_sandbox_overlap.sh` : cinq lancements, ombres
  seules contre le natif, reflets seuls et les deux contre la vérité terrain ; les événements se
  chevauchent, on ne soustrait pas des pourcentages) sur la marche :
  sauts avec les deux 1,257 %, expliqués par les ombres 0,682 %, par les reflets 0,513 %, **dus à la
  seule superposition 0,062 % (5 %)**. Les ombres seules sautent sur 0,738 % : le plus gros morceau.
- Correction : l'ombre s'applique avant le reflet (qui la remplace en partie). Sauts 1,257 → 1,168 %,
  superposition 0,062 → 0,037 % ; sur les 7 scènes, écart à la vérité et sauts en baisse partout
  (`results/urti/bac_a_sable/suite/tableau_v34.txt`, `results/urti/bac_a_sable/superposition/`).

## v35 — URTI : feuillage découpé par alpha test (trous reconnus par la caméra et par le soleil)

- Pourquoi le feuillage de RDR2 « saute » et pas celui du bac à sable : les jeux dessinent le feuillage
  (herbe, franges, grillages) en plaques dont la texture découpe les feuilles (alpha test). URTI capte la
  géométrie, pas la texture : pour lui chaque plaque est un rectangle plein ; un rayon qui tape un trou
  est rejeté (le reflet disparaît) ou fait une ombre en rectangle, et le vent fait alterner.
- Bac à sable : feuillage en plaques croisées découpées (écran, carte d'ombres, miroir de la vérité
  terrain), `URTI_SANDBOX_LEAVES=blocs` pour l'ancien ; cadrage du test arbre avec son reflet dans la
  flaque ; carte d'ombres réaliste (profondeur sur 400 m, décalage de 40 pas D16 au lieu de 600 : ~25 cm
  au lieu de plusieurs mètres). Le défaut est reproduit : reflet de l'arbre troué et fait de rectangles
  (`results/urti/bac_a_sable/feuillage/`).
- Correction (`alpha_screen=1`, par défaut) : chaque triangle rencontré par un rayon est examiné.
  - À l'écran : si la caméra voit plus loin que ce point (ou le ciel), c'est un trou : le rayon continue.
  - Hors de l'écran (ombres de la couronne au-dessus de la caméra) : la carte d'ombres du jeu est la vue
    du soleil, alpha test compris ; le point est un trou si le premier obstacle qu'elle a noté est au
    moins à mi-chemin entre lui et le récepteur (le jeu note ses obstacles un peu plus loin qu'ils ne
    sont : un seuil fixe faisait disparaître toutes les ombres RT, vu au masque et corrigé). La carte du
    jeu est donc aussi copiée hors du mode combiné (+0,17 ms).
- Mesures (avec les plaques) : marche, sauts 1,294 → 1,177 %, clignotement 0,952 → 0,853 %, écart à la
  vérité 1,14 → 1,01 % ; arbre, écart 2,67 → 2,46 %. Les 7 scènes : `suite/tableau_v35.txt` (le bac à
  sable a changé : feuillage et carte d'ombres, les tableaux précédents ne se comparent pas ligne à ligne).

## v36 — URTI : fin du « pop / dépop » des reflets, structure d'accélération deux fois moins chère

- Retour du jeu : les reflets ne partent plus dans tous les sens mais apparaissent et disparaissent en
  boucle, très vite. Nouvelle vue de diagnostic 9 (part du reflet RT réellement appliquée) et mesure des
  bascules (`tools/reshade/urti_sandbox_popping.sh` : la part change de plus de 0,3 d'une image à
  l'autre ; aller-retour : dans un sens puis dans l'autre aussitôt). Marche : 0,404 % d'allers-retours
  des pixels reflétants par image, concentrés dans le feuillage, les câbles et les vitres.
- Cause 1 : le mode « sol mouillé » (actif par défaut dans RDR2) rendait lisse toute surface dont la
  normale tirée de la profondeur pointe vers le haut ; dans un feuillage cette normale est du bruit, le
  pixel devenait réfléchissant une image sur deux. Le mode mouillé ne vaut plus que sur une surface plane
  (les points à 4 pixels de part et d'autre, sur chaque axe, alignés avec lui) : 0,404 → 0,276 %.
- Cause 2 : une surface lisse dont le point reflété est caché ou hors de l'écran (vitre qui reflète
  l'autre côté de la rue vu de biais, feuilles) passait à zéro, puis revenait dès qu'il redevenait
  visible. C'est maintenant une donnée manquante (poids négatif) : la stabilisation garde l'historique,
  qui s'estompe (× 0,9 par image), et le bornage ne compte que les voisins connus : 0,276 → 0,140 %
  (−65 % en tout). Contre la vérité terrain, pas de traînée : marche, sauts 1,177 → 1,133 %, clignotement
  0,853 → 0,802 %, écart 1,01 → 0,98 % ; les autres scènes égales ou meilleures (`suite/tableau_v36.txt`).
- Vitesse : la structure d'accélération était construite sur toute la capacité réservée (8,4 millions de
  sommets dans RDR2), partie vide comprise ; elle ne couvre plus que les sommets de l'image. Et tant que
  la liste des dessins est la même qu'à la dernière construction, elle est mise à jour (refit) au lieu
  d'être reconstruite, avec une reconstruction au moins toutes les 8 images (`blas_refit=`, 0 : toujours
  reconstruire). Bac à sable : 0,65 → 0,35 ms, image identique ; le journal donne maintenant les sommets
  par image et le nombre de reconstructions et de mises à jour.

## v37 — URTI : budget de géométrie, tailles apprises, interface du jeu épargnée

- Le journal de RDR2 (v36) l'a montré : **8 388 605 sommets sur 8 388 608** — l'emplacement de capture
  est plein à chaque image. Les dessins suivants sont refusés, mais des petits passent encore : les objets
  présents dans la scène RT changent d'une image à l'autre (ombres et reflets qui sautent), la structure
  couvre toute la capacité (pas de gain de vitesse) et la liste change toujours (0 mise à jour). Le bac à
  sable (122 000 sommets) ne pouvait pas le voir.
- **ExecuteIndirect** : chaque dessin réservait 65 536 sommets (nombre inconnu avant le dessin). Taille
  maintenant apprise par identité (mesurée, + 25 % et 64 de marge). Bac à sable : la demande passe de
  122 000 à 60 000 sommets — la moitié de l'emplacement était du vide.
- **Budget de géométrie** (`vertex_budget=`, 4 millions par défaut ; 0 : sans) : chaque objet capté est
  mesuré sur le GPU (`live_regionstats.hlsl` : distance minimale et taille dans l'espace caméra) ; seuls
  les plus importants (taille apparente) entrent dans la scène RT, bonus de 30 % à ceux qui y sont déjà
  (pas de va-et-vient) ; un objet trop gros pour la place restante est sauté. Les objets hors budget ne
  sont pas captés du tout (moins de dessins rejoués) et sont re-mesurés par roulement dans une zone de
  mesure en fin d'emplacement, hors de la structure (`probe_budget=`, 1 million de sommets par image) :
  un objet dont on s'approche rentre. Le budget ne vaut que pour l'écran principal (une seconde passe de
  profondeur, comme celle à mi-résolution du bac à sable, faisait changer d'écran principal) ; les
  identités sont calculées avant tout refus (elles glissaient quand l'emplacement était plein).
- Saturation reproduite (`slot_limit=` : emplacement réduit) : marche, sauts 1,133 → 1,402 % sans budget ;
  avec budget, l'image est plus proche de l'idéale (écart 1,16 → 1,08 %, scintillement ajouté +0,067 →
  +0,021 %). Dans le bac à sable les dessins refusés sont toujours les mêmes ; dans RDR2 ils changent, et
  c'est cette instabilité que le budget supprime : à confirmer en jeu (le journal donne désormais
  sommets réservés, objets dans la scène RT, hors budget, re-mesurés, refusés).
- **Interface du jeu** (menus reflétés au sol) : les reflets lisaient leurs couleurs dans l'image finale,
  interface comprise, et la composition dessinait ombres et reflets par-dessus les menus. URTI
  photographie maintenant l'écran au premier dessin de chaque liaison de l'image d'écran (jusqu'à quatre
  par image) ; `live_uicompare.hlsl` compte les pixels de chaque photo identiques à l'image finale ; la
  première photo identique à plus de 60 % est l'image d'avant l'interface. Les reflets y lisent leurs
  couleurs, et les pixels qui en diffèrent (l'interface) sont laissés tels quels. Bac à sable avec une
  interface (mini-carte, barres, menu ouvert 2 s sur 4) : photo 0 trouvée seule (88,7 % identique) ; sans
  elle, le menu se reflète au sol et la scène reflétée passe par-dessus ; avec, le menu est propre
  (`results/urti/bac_a_sable/interface/`). `ui_photo=0` pour comparer.
- 7 scènes égales ou meilleures (`suite/tableau_v37.txt`) ; pop / dépop 0,138 %.

## v38 — URTI : identités stables, budget respecté, sans OptiScaler

- Journal de la session v37 dans RDR2 : budget sans effet (8,37 millions de sommets réservés, ~200
  dessins refusés par image) car **120 000 identités pour ~800 objets à l'écran** : l'identité incluait
  le nombre d'instances et la première instance, qui changent à chaque image (végétation par instances).
  Elle ne garde plus que le maillage (nombre d'indices ou de sommets, début dans ses tampons) ; un
  ExecuteIndirect, sa signature de commande ; plus le rang. Bac à sable : 381 mesures en mémoire.
- Budget respecté : un objet inconnu n'entre dans la scène RT que tant que la place du budget le permet ;
  au-delà il est mesuré dans la zone de mesure (dans sa limite par image). Saturation reproduite :
  30 697 sommets pour un budget de 30 000, 0 dessin refusé.
- Photo d'avant l'interface : dans RDR2 le rang de la bonne photo change d'une image à l'autre (le jeu
  enregistre ses commandes sur plusieurs fils). Choix sur le GPU dans la même image
  (`live_uiselect.hlsl`) : photo de cette image (identique à plus de 60 % à l'image finale et plus
  proche d'elle que de l'image affichée précédente, sinon c'est une ancienne image), la moins identique
  (la plus tôt, le moins d'interface) ; copiée dans un tampon que reflets et composition lisent.
- « Une image sur deux » : OptiScaler avait la génération d'images active (`FGOutput = DLSSG`) ; URTI ne
  s'appliquait qu'aux images réelles. L'utilisateur l'a retiré.
- **Plantage au démarrage sans OptiScaler** (ucrtbase, 0xc0000409) : ReShade, chargé directement
  (`dxgi.dll`), décharge et recharge les add-ons à chaque appareil que RDR2 crée au démarrage pour tester
  la carte ; les entrées de tables de fonctions remplacées par URTI (`CreateRootSignature`,
  `ExecuteIndirect`) pointaient alors vers le code d'un add-on déchargé. Elles sont maintenant remises à
  l'origine au déchargement. Vérifié : le jeu démarre (`tools/reshade/rdr2_demarrage.ps1` : démarrage sans
  clic, surveille processus et plantages).

## v39 — URTI : débruiteurs de NVIDIA (NRD / SIGMA) intégrés ; le jitter, cause du tremblement restant

- **NRD 4.17.3** (NVIDIA, licence RTX SDK, autorisée pour l'intégration dans une application ; source et
  bibliothèques hors du dépôt, dans `urti/third_party/NRD`, compilées avec NRI en statique, shaders DXIL
  seulement, **sans le support Agility SDK** : sinon NRI appelle `ID3D12Device15::TryCreate…View`, absent
  du DirectX 12 du système (`ID3D12Device14`) → écriture n'importe où, plantage dans D3D12Core). CMake
  l'inclut s'il est compilé (`URTI_NRD`).
- Intégration (`urti_nrd.inl`, réglage `nrd=1`) : SIGMA (ombres) via la couche NRDIntegration sur NRI, qui
  enveloppe l'appareil D3D12 du jeu, sa file de commandes et nos textures (barrières classiques). Entrées :
  `live_shadow` en mode SIGMA (un rayon par pixel, pénombre = distance de l'obstacle × tan du rayon du
  soleil, et le poids des corrections d'URTI), `live_nrdprep` (profondeur linéaire, normales encodées par
  `NRD_FrontEnd_PackNormalAndRoughness`), vecteurs de mouvement du jeu tels quels (échelle selon leur
  convention). NRD tourne sur sa propre liste de commandes (il installe ses tables de descripteurs : rien ne
  fuit dans l'état de ReShade), soumise entre nos passes ; `live_nrdpost` recompose le masque. Un
  plantage dans NRD est rattrapé (pile d'appels symbolisée dans le journal) et URTI repasse à son
  débruitage ; symboles de débogage générés en Release.
- Mesures (bac à sable) : SIGMA **moins bon** que le débruitage d'URTI (arbre au vent, clignotement 0,115 →
  0,301 % ; marche 0,801 → 0,929 %) ; seuil de disocclusion, nombre d'images, taille du soleil : sans effet.
  **Sans décalage sous-pixel** (`URTI_SANDBOX_JITTER=0`), l'arbre au vent devient presque parfait avec les
  deux (URTI 0,000 %, SIGMA 0,010 %) : le tremblement restant d'une scène fixe vient du **jitter** du jeu
  (DLSS / TAA). URTI calcule sur la profondeur décalée et compose sur l'image déjà stabilisée ; NRD exige
  ce décalage (`cameraJitter`), qu'URTI ne connaît pas. SIGMA coupé par défaut en attendant.
- Piste : le jitter apparaît dans le passage d'une image à l'autre mesuré par les cascades (live_sunvote)
  comme un cisaillement, séparable de la rotation (`A[0][2] + A[2][0]`, `A[1][2] + A[2][1]`).

## v40 — URTI : décalage sous-pixel (jitter) et champ de vision du jeu mesurés et compensés

- Le décalage sous-pixel du jeu (DLSS / TAA) est mesuré à chaque image sans rien savoir du jeu : la
  projection décalée ajoute à l'espace caméra un cisaillement x' = x + a w, y' = y + b w ; le passage d'une
  image à l'autre mesuré par les cascades vaut A = K_préc S R S⁻¹ K_cour⁻¹. Au premier ordre
  a_préc - a_cour = A[0][2] + s² A[2][0] (b avec t²), où s, t sont l'étirement de l'espace caméra d'URTI
  (champ de vision supposé) par rapport au vrai. s² et t² sont mesurés par régression (sommes
  décroissantes) sur les images où la caméra tourne : sans eux, la rotation fuyait dans la mesure (dérive
  de ~16 pixels en marchant). Décalage absolu : somme des écarts moins sa moyenne glissante (la suite
  d'un jeu est centrée) ; bac à sable : ±0,4 pixel, le cycle de Halton à 8 images retrouvé.
- **Champ de vision du jeu mesuré** (s = tan(champ vrai / 2) / tan(champ supposé / 2)) : bac à sable
  54,9° à 55,1° pour 55° (URTI supposait 50°). Journalisé ; pas encore réinjecté dans la projection.
- Compensation (`jitter=2` par défaut ; 1 : seulement l'écart, 0 : rien) : la composition lit masque et
  reflets à l'endroit non décalé (l'image présentée est stabilisée par le TAA), les reflets prennent leur
  couleur à l'endroit non décalé, les historiques sont reprojetés avec l'écart de décalage. Recaler
  seulement l'historique est pire (il suit alors le décalage au lieu de le moyenner).
- 7 scènes, toutes meilleures (`suite/tableau_v40.txt`) : clignotement bâtiment 0,077 → 0,003 %, arbre
  immobile 0,124 → 0,054 %, arbre au vent 0,115 → 0,050 %, personnage 0,076 → 0,034 %, caméra fixe
  0,098 → 0,081 %, occlusion 0,126 → 0,081 %, marche 0,801 → 0,803 %.
- `jitter_log=1` : décalage mesuré à chaque image dans le journal.

## v41 — URTI : réserve de géométrie des ExecuteIndirect apprise par signature

- Première session de la v40 dans RDR2 (lancée par URTI) : le jeu démarre, champ de vision mesuré 49,5° à
  53,8° (il varie : visée, caméra), 49 à 53 i/s. Nouveau bilan des plus gros objets : **les ExecuteIndirect
  réservent 65 535 sommets et en écrivent de 0 à 10 000** (86289/86289 65535/2787 65535/10242 65535/0…) :
  sur 8,3 millions de sommets réservés, 1,3 à 1,5 million écrits — plus de 80 % de l'emplacement est du
  vide, d'où sa saturation (50 à 230 dessins refusés par image) et le coût de la structure.
- L'apprentissage par identité ne prenait pas : l'ordre des ExecuteIndirect change d'une image à l'autre
  dans RDR2. Taille apprise maintenant **par signature de commande** : le plus grand nombre de sommets
  écrits par ses dessins (décroît de 2 % par image) ; une zone remplie jusqu'au bout (tronquée) double la
  taille apprise ; réserve = taille apprise × 1,25 + 64. Bac à sable inchangé ; à vérifier dans RDR2.

## v42 — URTI : sol mouillé retiré, champ de vision corrigé tout seul, ExecuteIndirect identifiés par leurs arguments

- Retour de jeu : le sol mouillé ajouté par URTI fait des bugs (RDR2 n'a pas ce rendu) ; les reflets
  semblent parfois « flotter au-dessus » des flaques. Sol mouillé coupé par défaut (`UrtiWet` = 0 dans
  urti.fx ; le réglage reste pour essai).
- **Projection d'URTI corrigée par le champ de vision mesuré** (`fov_auto=1` par défaut) : quand la mesure
  est fiable (assez de rotation de la caméra) et s'écarte de plus de 3 %, p11 est corrigé de 80 % de
  l'écart, la mesure repart de zéro et les 6 lectures suivantes (trois images de retard) sont ignorées.
  Bac à sable : 50,0° → 54,1° → 54,9° (vrai : 55°), stable. Dans RDR2 le champ mesuré varie de 49,5° à
  60,6° (visée, caméra). Avec des angles exacts, la normale du sol et la direction des reflets ne sont plus
  faussées. Suite : marche, écart à la vérité 0,96 → 0,90 %, scintillement ajouté +0,054 → +0,025 % ;
  les autres scènes égales ou meilleures (`suite/tableau_v42.txt`).
- Journal de la session v41 : la réserve était encore pleine (« 65536/22176 65536/65535 65536/0 ») : une même
  signature de commande sert à des ExecuteIndirect de toutes tailles, son maximum imposait 65 536 sommets à
  tous. Identité d'un ExecuteIndirect : signature, tampon d'arguments et position dans ce tampon (plus de
  rang) ; taille apprise par identité (0 compris : réserve de 66 sommets) ; une zone remplie jusqu'au bout
  repart à la réserve maximale, la mesure suivante redonne la vraie taille.

## v43 — URTI : les ombres ne retirent que la lumière du soleil ; carte d'ombres du jeu enfin bien lue

- Demande : une ombre est une visibilité du soleil, elle ne doit retirer que la lumière directe, pas
  assombrir toute la couleur (`c *= 1 - force × (1 - masque)`, même proportion partout, même par temps
  couvert).
- **Part du soleil mesurée dans l'image du jeu** (`live_sunlight.hlsl`, à chaque présentation, avant la
  composition) : un point tiré par bloc de 4×4 pixels de rendu, face au soleil (N·L > 0,2), à moins de
  100 m, nettement éclairé ou nettement à l'ombre dans la carte du jeu (voisinage 5×5 dans le même état) ;
  partenaire à 10 pixels sur le même plan, dans l'autre état. Rapport des couleurs de l'image :
  ombré / éclairé = ciel / (ciel + soleil × N·L), d'où x = soleil / ciel par canal et en luminance,
  accumulé dans des histogrammes (log2, quarts d'octave) dans le tampon du soleil (octets 1232 à 1887),
  médiane lue sur le processeur, lissée en log (5 % par image). Moins de 32 paires : valeur gardée.
- **Ombre physique** : le masque retire x N·L / (1 + x N·L) de la luminance (au lieu de la pente fixe
  min(1, 2,5 N·L)) ; la composition l'applique par canal avec la teinte mesurée (le soleil est plus chaud
  que le ciel : l'ombre bleuit comme celles du jeu). `UrtiStrength` = 1 par défaut : aussi sombre que les
  ombres du jeu. `sun_measure=0` : ancienne règle.
- **Bug trouvé : la carte d'ombres du jeu était mal lue depuis le début.** RDR2 (et le bac à sable) l'ont
  en R16 (format 53, deux octets par texel) ; URTI lisait quatre octets comme un flottant : valeurs
  absurdes, lectures hors des lignes. Touchés : le mode « seulement les ombres que le jeu n'a pas »
  (décidé au hasard pixel par pixel, qui bouge avec les cascades : une cause probable des ombres qui
  gigotent dans le jeu, où ce mode est choisi) et le test des trous du feuillage hors de l'écran. Lecture
  16 bits ajoutée (bit 32 des drapeaux de `live_shadow`).
- **Combinaison avec l'ombre du jeu** : part éclairée filtrée sur 3×3 texels (sa pénombre comprise) et
  visibilité finale = la plus faible des deux (URTI retire max(0, jeu - RT)), au lieu d'une ombre du jeu
  élargie de deux texels (tout ou rien) multipliée. Mode par défaut : « seulement les ombres que le jeu
  n'a pas » (`UrtiMode` = 1) : le mode « complet » retire deux fois le soleil là où le jeu ombre déjà.
  `game_min=0`, `game_dilate=` : ancienne combinaison, pour essai.
- Bug évité : un `float3` dans les constantes de la composition, déplacé à la frontière de 16 octets
  suivante, ne recevait que le rouge : trois nombres séparés.
- **Nouveau test** `tools/reshade/urti_sandbox_ombres.sh` : la carte du « jeu » oublie poteaux, piquets,
  mur, charrettes et passants (`URTI_SANDBOX_GAMESHADOW=partiel`) ; on compare au lancement où le jeu les
  ombre (sol sec, reflets coupés). Ombre oubliée rattrapée, avant → après :
  rue 56 → 90 %, occlusion 57 → 91 %, marche 55 → 68 % ; noirceur dans la rue (lumière RT / jeu, rouge
  vert bleu) 2,5 1,9 1,6 → 1,08 1,12 1,11 (teinte juste) ; écart hors de la zone 1,9 / 1,3 / 0,5 → 0,15 /
  0,11 / 0,19 % (plus de double ombre) ; scintillement dans la zone en marche 6,4 → 5,2 %
  (`ombres/tableau_avant.txt`, `ombres/tableau_v43.txt`). Soleil / ciel mesuré dans le bac à sable : 4,5
  (rouge 5,8, vert 4,3, bleu 3,0).
- Suite des reflets (vérité terrain = ombres du jeu seules) : écart bâtiment 2,13 → 0,08 %, passants
  1,60 → 0,47 %, marche 0,90 → 0,60 % (sautent 1,10 → 0,50 %, clignotent 0,81 → 0,57 %), occlusion
  1,32 → 0,36 % (`suite/tableau_v43.txt`).
- Reste : en marche, les ombres fines et lointaines (poteaux, câbles) sont trop lissées (68 %).

## v44 — URTI : reflets RT seulement sur les surfaces planes ; la carte d'ombres de RDR2 ne concorde pas

- Retour de jeu (v43, jardin de Saint-Denis au soleil) : les reflets tremblent encore, des choses sautent,
  **les arbres ont des points gris flous qui tremblent**, et les ombres ne font **aucune différence**
  activées ou non. Journal : soleil / ciel mesuré 0,06 à 0,16 (6 à 13 % de la lumière retirée : ombres
  invisibles) alors que la scène a des ombres franches ; 50 à 115 objets sur ~800 dans la scène RT ;
  48 i/s, ~7 ms d'URTI par image. Préréglage du joueur à la fin : mode « ombres RT complètes », force 1.
- **Points gris** : les reflets RT se posaient sur toute surface que le jeu déclare lisse (cheveux,
  feuilles, peau) ; la normale tirée de la profondeur y est du bruit, le reflet partait au hasard et
  clignotait (vu sur la nuque du personnage dans une capture). Le contrôle de planéité (jusqu'ici réservé
  au sol mouillé) s'applique maintenant à tous les reflets : voisins à 2 pixels (à 4, au loin ils
  tombaient sur un autre objet : sauts en marche 0,50 → 0,91 %), et une surface n'est refusée que si elle
  a du relief dans les deux directions (le bord d'une flaque contre un mur reste plan). Bac à sable
  (reproductible à l'identique : contrôle coupé = v43 au chiffre près) : sautent / clignotent, marche
  0,501 / 0,565 → 0,484 / 0,557 %, arbres au vent 0,019 / 0,041 → 0,016 / 0,036 %, rue fixe
  0,066 / 0,075 → 0,057 / 0,063 %. Le jeu garde ses propres reflets sur les surfaces refusées.
- **La carte d'ombres de RDR2 ne concorde pas** avec ce qu'URTI en attend (session lancée par URTI dans le
  bayou, temps couvert) : contrôle ajouté dans `live_sunlight` (points face au soleil que la carte dit
  éclairés / à l'ombre / au bord / hors des cascades, luminance moyenne des deux premiers) : **100 % au
  bord** ; la vue « ombre du jeu recalculée » est blanche partout. Dans le bac à sable : 65 % éclairés,
  31 % à l'ombre, rapport de luminance 5,9. Le test de profondeur dans la carte de RDR2 n'avait jamais pu
  être vérifié (lecture fausse jusqu'à la v43) : la profondeur que la projection d'URTI donne au point
  n'est probablement pas celle que le jeu écrit (plage de profondeur du viewport, décalage…). Ajouté au
  journal toutes les 300 images : histogramme de la profondeur des points dans la cascade et de l'écart
  carte - point (bac à sable : points dans 0,375 à 0,625 ; écart nul pour les points éclairés, très
  négatif pour ceux à l'ombre). À relever dans RDR2 **au soleil** pour corriger.
- **Garde-fou** : la mesure soleil / ciel n'est prise que si la carte concorde avec l'image (points
  éclairés au moins 1,3 fois plus clairs que ceux à l'ombre) ; sinon la valeur par défaut reste (2,2 :
  69 % de la lumière retirée face au soleil, l'ancien rendu) au lieu d'effacer les ombres.
- Bug de l'outil de contrôle : `show_mask=1` dans urti.ini cochait « masque seul » dans l'effet, sans
  jamais la décocher : écran blanc (masque sans ombre) après le contrôle. Décochée maintenant quand la
  ligne est retirée. Le jeu, lancé par URTI pour ce contrôle, a été fermé (aucune progression jouée).
