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
