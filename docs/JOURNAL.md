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
