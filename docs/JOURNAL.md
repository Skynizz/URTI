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
