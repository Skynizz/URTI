# Journal de bord

Les étapes v1 à v24 (KRC, le cache de radiance étudié d'abord) sont dans le dépôt [KRC](https://github.com/Skynizz/KRC).

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

## v45–v46 — URTI : mesure dans le jeu (rafales) ; la scène RT de RDR2 était tirée au hasard à chaque image

- Demande : « les reflets sautent, tu dis que c'est corrigé mais ça ne l'est jamais ». Les mesures du bac
  à sable ne suffisent pas : il faut mesurer dans RDR2.
- **Rafale** (touche F8 ou fichier déclencheur `urti_rafale`) : 30 images consécutives enregistrées sans
  URTI (copie de l'image avant la composition) et avec, dans `urti_rafale_N\`. `tools/reshade/urti_rafale.py`
  : contribution d'URTI C = après - avant, ce qui saute = variation de C d'une image à la suivante
  (indépendante de la scène) ; zones ombres / reflets ; objets entrés / sortis de la scène RT par image
  (lignes RAFALE du journal) ; carte des sauts et gros plan de la pire zone. Résultats dans
  `results/urti/jeu/rafales/`.
- Réglages de RDR2 relus (system.xml) : RTX 4070, DX12, DLSS, HDR, flou de mouvement coupé, netteté au
  maximum, reflets de l'eau en espace écran actifs (le jeu garde ses reflets là où URTI n'en met pas),
  plusieurs sources d'ombres (cascades, lointaines, relief, espace écran, longues). Rien qui fausse URTI.
- Séance dans RDR2 lancée par URTI (Saint-Denis la nuit, pavés mouillés) : URTI changeait 0,2 % des pixels ;
  au plus fort saut, le reflet orange d'un lampadaire apparaissait deux ou trois images puis disparaissait.
  Journal : **0 objet connu dans la scène RT, 6,1 M sommets jamais mesurés, 183 499 mesures en mémoire
  pour ~930 objets à l'écran**. Cause : l'identité d'un ExecuteIndirect (v42) comprenait l'adresse et la
  position de ses arguments, qui tournent dans RDR2 : nouvelle identité à chaque image, jamais mesurée,
  réserve maximale ; les inconnus remplissaient le budget dans l'ordre d'enregistrement (variable) : la
  scène RT était un tirage différent à chaque image. Le bac à sable (arguments fixes) ne pouvait pas le voir.
- **Identité stable** : signature de commande, pipeline, tampons d'indices et de sommets liés (événements
  `bind_index_buffer` / `bind_vertex_buffers` suivis par liste de commandes), nombre maximal de commandes.
  Dans RDR2 : 671 objets à l'écran, **671 dans la scène RT, 0 jamais mesuré, 785 mesures en mémoire,
  0 hors budget**, 0,4 objet entré ou sorti par image. Rafale caméra fixe, zone des reflets d'URTI :
  tremblent 8,2 → 1,5 %, sautent 0,88 → 0,12 %. Budget porté à 6 M sommets (place d'un emplacement : 8 M).
  Bac à sable inchangé au chiffre près.
- Coût : la scène RT contient maintenant de vrais triangles (4,5 M sommets) : construction de la structure
  2,6 → 4,1 ms, reflets 0,2 → 1,0 ms ; ~51 i/s dans cette scène de nuit (64 avant, avec une scène RT
  vide de sens). La structure est reconstruite à chaque image (300 sur 300) : la mise à jour exige la même
  disposition, or la place de chaque objet dans le flux suit l'ordre d'enregistrement des dessins, qui
  change d'une image à l'autre (RDR2 enregistre sur plusieurs fils). À faire : une place fixe par objet.
- Non vérifiable cette nuit-là : reflets de jour, ombres (pas de soleil). La rafale caméra en mouvement
  mêle le déplacement normal du reflet aux sauts : à affiner.

## v47 — URTI : F4, avant / après en plein écran

- Demande du joueur : un bouton pour voir la différence en direct. **F4** bascule entre l'image du jeu seule
  et l'image avec URTI, en plein écran et sans délai : seule la pose sur l'image est sautée, le calcul RT
  continue (les images par seconde ne changent donc pas : pour elles, F7 coupe tout). F5 reste l'écran
  partagé (gauche sans, droite avec).

## v48 — URTI : case « URTI activé » dans le menu (tout le calcul coupé), F7 synchronisée

- Demande : un bouton qui coupe complètement le ray tracing (plus aucun calcul) et le relance. Case
  `UrtiOn` dans le menu de ReShade : décochée, plus de capture de géométrie, de structure, d'ombres, de
  reflets ni de pose ; recochée, tout reprend. F7 fait la même chose et la case suit. Sans effet si
  urti.ini n'a pas `live=1` (les lancements natifs du bac à sable restent natifs ; suite inchangée).
- « Aucune différence » avec F4 : la partie du joueur était de nuit (soleil suivi sur 0 % des images, pas
  de carte d'ombres du soleil). La nuit, URTI ne fait pas d'ombres (seulement celles du soleil) et ses
  reflets, limités aux surfaces planes, reflètent une scène sombre : 0,2 % des pixels changés (rafale).
  Les reflets visibles sur les pavés mouillés la nuit sont ceux du jeu.

## v49 — URTI : plus de menus dans les reflets ; plus d'ombres invisibles qui coûtent 4 ms

- Retour de jeu (Saint-Denis, jour de pluie, sol mouillé par le jeu) : les reflets tremblent, **les menus
  s'affichent dans les reflets au sol**, des reflets se décollent du sol, aucune différence pour les
  ombres ; 20 à 27 i/s. Journal : soleil suivi, carte d'ombres concordante (éclairés 3,2 fois plus clairs
  que les points à l'ombre), mais soleil / ciel mesuré 0,06 (ciel couvert : 6 % de la lumière retirée) ;
  GPU : ombres 3,8 ms, reflets 3,1 ms, soleil et structure 3,9 ms. « Pas de photo avant l'interface »
  27 fois dans la partie.
- **Menus dans les reflets** : la photo d'avant l'interface (couleurs des reflets) n'était gardée que si
  elle ressemblait plus à l'image courante qu'à la précédente. Image immobile (le joueur regarde le sol,
  un menu est ouvert) : égalité, aucune photo, couleurs lues dans l'image finale, menus compris. Le GPU
  garde maintenant son choix d'une image à l'autre (`live_uiselect`, octets 64 / 68 de ses comptes) : la
  photo précédente reste si elle ressemble encore à au moins 30 % à l'image finale (un grand menu en
  couvre une part) et n'est pas une copie de l'image affichée précédente (photo prise trop tôt). Si
  aucune photo n'est sûre, les reflets RT s'effacent en douceur (-15 % par image) et reviennent avec elle.
  Bac à sable, plan fixe sans décalage sous-pixel, menu ouvert 2 s toutes les 4 s : photo perdue 2 fois
  sur 600 images → 0.
- **Ombres invisibles** : soleil / ciel mesuré sous 0,15 (moins de 13 % de la lumière retirée : ciel
  couvert, crépuscule) : plus de rayons d'ombre (3,8 ms gagnées dans cette scène) ; ils reprennent quand
  la lumière directe revient (la mesure continue).
- Suite des reflets inchangée (`suite/tableau_v49.txt`). Non vérifié dans le jeu : à mesurer (rafales F8
  sur le sol mouillé, et une avec un menu ouvert).

## v50–v52 — URTI : une seule case, occultation ambiante RT, ombres du soleil coupées (mesuré : invisibles)

- Retour du joueur : « 0 différence » et beaucoup d'images par seconde perdues ; trop d'options ; demande un
  RT « à la NVIDIA » (une case, tout automatique), l'occultation ambiante, voire du path tracing et la Ray
  Reconstruction ; une option de beau temps ; plantage en ouvrant le Social Club (touche Home, « erreur
  mémoire »).
- Analyse de sa partie : **force des reflets à 0 dans son menu** (`UrtiReflect=0`) et ombres suspendues
  (ciel couvert) : URTI ne pouvait rien changer, en coûtant 4 à 5 ms (structure, suivi du soleil).
- Recherches : le « ray tracing » vu sur RDR2 est fait de shaders ReShade en espace écran (RTGI / PTGI de
  Pascal Gilcher, sans direction du soleil) ; la Ray Reconstruction ne s'injecte (forks expérimentaux
  d'OptiScaler) que dans des jeux qui ont déjà du path tracing ; beau temps à la demande : ScriptHookRDR2 et
  un trainer (fichiers tiers, non installés sans accord).
- Plantage Social Club : pas reproduit (touche Home envoyée : pas de menu, pas de plantage) ; aucun
  plantage dans le journal de Windows depuis le 31/10 ; ReShade s'ouvre sur Page suivante (pas de
  conflit) ; mémoire vidéo 7,0 / 12 Go. À revoir avec le texte exact de l'erreur.
- **Menu réduit à « URTI activé »** : les autres réglages sont cachés et ignorés (l'ancien `UrtiReflect=0`
  du préréglage n'éteint plus rien) : reflets à 1, ombres « seulement celles que le jeu n'a pas », pas de
  sol mouillé ; urti.ini garde la main pour les essais.
- **Occultation ambiante RT** (`live_shadow`, ao=0,8, ao_radius=1,5 m) : deux rayons courts par pixel
  (hémisphère pondéré par le cosinus, motif tournant), obstacle à t : occultation (1 - t / portée)² ; trous
  du feuillage reconnus ; ne retire que la lumière du ciel (toute à l'ombre, 1 / (1 + x N·L) au soleil, x
  soleil / ciel mesuré) ; débruitée avec le masque. **Seulement sur les surfaces planes** (sol, murs,
  routes) et à moins de 50 m : sur l'herbe et les grilles, chaque brin est de la géométrie, tout
  s'assombrissait (le jeu y a déjà son occultation) et tremblait.
- **Ombres du soleil RT coupées par défaut** (`shadows=1` pour les rallumer ; drapeau 64 : le soleil reste
  connu pour l'occultation) : rafale au soleil sans occultation : **0,0 % de l'image changé pour 3,2 ms**.
  Les ombres de RDR2 n'en laissent presque aucune à ajouter.
- Débruitage du masque quand il ne porte que l'occultation : plus d'images accumulées (part courante
  0,05 à 0,3) et bornage plus large (2 écarts-types).
- Mesures dans RDR2 (rafales, `results/urti/jeu/rafales/`) :
  - v50 (occultation partout) : 32,5 % de l'image changé, pixels qui sautent 0,21 %, ombres + occultation
    6,1 ms ;
  - v50 occultation plane : 20,2 %, 0,12 %, 5,1 ms ;
  - v51 (sans ombres du soleil, brouillard) : 8,2 %, 0,08 %, 1,2 ms, 56-58 i/s ;
  - v52 (bayou au soleil) : 9,4 %, 0,05 %, zone assombrie tremble 1,2 % (4,1 % en v51), 2,3 ms, 44 i/s.
- Restent :
  - des flashs d'une image quand des objets entrent ou sortent de la scène RT (images 6, 11-12, 15 de la
    rafale v52 : 4 à 10 objets ; le jeu ne dessine pas certains objets une image) ;
  - les reflets sur l'eau qui tremblent (zone de reflets : 17 % tremblent) ;
  - la reconstruction de la structure à chaque image (3,8 à 5,7 ms) ;
  - la photo d'avant l'interface encore perdue par moments dans RDR2 (ressemblance des photos maintenant
    journalisée).

## v53–v55 — URTI : une place fixe par objet : plus de flashs, structure mise à jour au lieu de reconstruite

- Avis du joueur : laisser les ombres au jeu (les siennes sont trop bonnes) ; étape 1 : stabilité et vitesse.
- Cause des flashs d'une image (rafale v52) : 8 à 9 objets par image entraient ou sortaient de la scène RT,
  sans refus ni dépassement de budget : le jeu ne dessine pas tout à chaque image (végétation, niveaux de
  détail, masquage). Et la place de chaque objet dans le flux suivait l'ordre d'enregistrement des dessins,
  variable : la structure était reconstruite 300 fois sur 300 (3,8 à 5,7 ms).
- **Places fixes** (`g_livePlaces`, urti.ini `persist=0` pour l'ancienne règle) : chaque objet de l'écran
  principal garde sa place d'une image à l'autre (premier espace libre assez grand, un quart de marge,
  espaces libres fusionnés) ; un objet que le jeu ne dessine pas pendant au plus 3 images garde sa dernière
  géométrie dans la scène RT ; au-delà, sa place est rendue. Les comptes de sommets écrits sont recopiés
  dans une case par place (`live_counts.hlsl`), que la conversion lit ; la conversion couvre toutes les
  places tenues (table triée par début). Une place refusée ou plus de 70 % de l'emplacement occupé : tout est
  redistribué à l'image suivante.
- Triangles absents : plats (sommets à l'origine) au lieu de NaN : une mise à jour de la structure ne peut
  pas changer l'état actif / inactif d'un triangle. **Mise à jour tant que la partie convertie garde sa
  taille** (reconstruction complète au plus toutes les 8 images).
- v53 : la partie occupée remplissait tout l'emplacement (82 dessins refusés par image, 406 objets dans la
  scène RT) : d'où la marge, la fusion et la redistribution de v54.
- Mesures dans RDR2 (v54, Saint-Denis au soleil) : 871 places tenues (71 Mo), partie occupée 81 Mo sur
  128, 0 nouvelle place par image, 0 redistribution ; **33 reconstructions et 267 mises à jour sur 300** ;
  étape « soleil et profondeur » 1,3 ms (3,8 à 5,7) ; **0 objet entré ou sorti sur les 30 images de la
  rafale**, 0,015 % des pixels qui sautent ; 65 i/s. Bac à sable : suite identique au chiffre près avec ou
  sans places fixes.
- Occultation ambiante en mouvement (bac à sable, marche) : sautent 0,47 % sans elle, 0,78 % avec ; débruitage
  du mode occultation seule élargi (flou 5×5 guidé par la profondeur, part de l'image courante en mouvement
  0,2) : 0,69 % (clignotent 0,84 → 0,71 %). Reste à réduire.

## v56–v66 — URTI : décalques, photo d'avant l'interface impossible dans RDR2, mises à jour de la structure abandonnées

Toutes les mesures ci-dessous sont prises dans RDR2, lancé par URTI ; scènes différentes d'un lancement à
l'autre (la sauvegarde recharge ailleurs) : les comparaisons valables sont celles d'un même lancement.

- **Occultation en mouvement** : 4 rayons au lieu de 2 là où l'image bouge de plus d'un pixel et demi
  (vecteurs de mouvement) ; feuillage exclu des surfaces qui la reçoivent (points voisins à 6 pixels vus
  nettement plus loin : le fond à travers les feuilles).
- **Reflets sur l'eau** : normale par différence centrée sur 4 pixels de part et d'autre quand les deux côtés
  sont sur la même surface (vaguelettes lissées) ; pas de rayon de reflet sous 3 % de poids (Fresnel de
  face), portée 500 m.
- **v56 : photos supplémentaires de l'écran (1er, 2e, 8e, 32e dessins) : RDR2 plante dès son menu**
  (erreur fatale « mémoire »), retirées (v57).
- **Taches sombres au sol à bords droits** (vues par le joueur) : les décalques du jeu (boue, marques,
  flaques), posés juste au-dessus de la route sans écrire la profondeur, arrêtaient les rayons
  d'occultation à quelques centimètres. Obstacles à moins de 6 cm ignorés (5 cm pour les reflets) : tache
  disparue (assombrissement moyen du sol 3,5 → 0,1 niveau).
- **Photo d'avant l'interface** : rafale enrichie (photo brute, photo gardée). Dans RDR2, **l'écran ne reçoit
  qu'un seul dessin par image**, qui assemble la scène et l'interface préparées ailleurs : la photo prise
  avant lui est une image précédente, interface comprise (mini-carte identique à l'image finale) ; aucune
  photo sans interface n'existe. Pire : la photo « gardée » (v58) ne ressemblait plus qu'à 2 % à l'image
  courante. Corrections :
  - dans RDR2, `ui_photo=0` (urti.ini du jeu) ;
  - plus jamais de photo gardée ;
  - interface repérée avec une tolérance (grain du jeu d'un ou deux niveaux) ;
  - **zone d'interface fixe** `ui_zone=0.012,0.69,0.16,0.97` (mini-carte et icônes) : mini-carte modifiée
    7,5 → 1,7 niveaux.
  - La photo prise juste après le premier dessin (seulement dans une liste directe) ne fait plus planter
    le jeu.
- Convention des vecteurs de mouvement remesurée dans le jeu : la même (n° 2). En mouvement, l'effet suit
  l'image à **98 %** (`tools/reshade/urti_rafale_decalage.py` : glissement de l'effet / glissement de l'image
  par corrélation de phase).
- **Décalage sous-pixel** : compensation coupée dans RDR2 (`jitter=0`) : tremblent 0,19 → 0,13 % ; zone des
  reflets tremble 2,57 → 1,79 %, saute 0,10 → 0,06 %. Garde la compensation dans le bac à sable.
- **Mises à jour de la structure** :
  - v58 : places redistribuées à chaque image (seuil de 70 % dépassé) alors que la mise à jour restait
    permise : 4,5 i/s. Corrigé (reconstruction obligatoire après une redistribution).
  - Triangles plats à l'origine (la caméra) : une mise à jour qui les rendait réels étirait des boîtes à
    travers toute la vue (rayons 10 fois plus lents) ; posés sur le premier sommet de leur place.
  - Régulateur au coût mesuré (reconstruction contre ralentissement des rayons).
  - Mesure finale, même scène : mises à jour 1,2 + 3,7 + 6,8 ms (structure, occultation, reflets), 38 i/s ;
    reconstruction à chaque image 5,7 + 1,2 + 0,5 ms, 45 i/s. Une seule mise à jour rend les reflets
    quatorze fois plus lents (lots de végétation triés par le GPU : les triangles changent de place).
  - **Reconstruction à chaque image par défaut** (`blas_refit=0`) ; places sans marge, redistribuées dès
    qu'un cinquième de la partie occupée est perdu en trous.
  - Gare de triage, ciel couvert : URTI 3,9 ms au total, 73 i/s.
- Avis du joueur sur la v66 : « parfait, le rendu est dingue », reste à gagner des images par seconde.
- **Bac à sable** (`suite/tableau_v66.txt`) :
  - arbres et personnage nettement mieux (sautent 0,010 → 0 %) ;
  - **caméra en mouvement moins bonne** : sautent 0,63 → 1,24 % (clignotent 0,68 → 0,55 %), écarts à la vérité
    plus grands partout. À comprendre : changements des reflets (seuil de poids, portée minimale, normale
    centrée) ou de la photo d'avant l'interface (photo après le premier dessin, tolérance).

## v67 — URTI : optimisation (budget de géométrie au coût mesuré, occultation au quart, composition allégée)

- Demande : les images par seconde. Coûts mesurés dans RDR2 (v65-v66) : la reconstruction de la scène RT
  domine (1,8 ms dans la gare de triage, 5,7 à 8,8 ms dans une rue sous la pluie) ; occultation ~1 ms,
  reflets 0,2-0,5 ms, composition 0,8 ms.
- **Budget de géométrie réglé sur le coût mesuré de la reconstruction** (`build_ms=2.5`, 0 : budget fixe) :
  toutes les 30 images, -15 % si la reconstruction dépasse la cible de 10 %, +8 % sous 75 % de la cible,
  entre 1,5 M sommets et `vertex_budget` (6 M). Les objets de plus petite taille apparente (boîte / distance)
  sortent d'abord.
- **Occultation au quart** (`ao_quarter=1`) : un pixel de chaque bloc 2×2 lance ses rayons à chaque image (les
  quatre en quatre images), deux fois plus de rayons par pixel (4 au repos, 6 en mouvement) ; les autres sont
  écrits -1 et reconstruits par le débruitage (flou 5×5 sur les seuls pixels calculés, historique seul sans
  voisin calculé). Bac à sable : passe d'occultation 0,86 → 0,39 ms, mesures de qualité inchangées.
- **Composition** : masque lu en bilinéaire (4 lectures au lieu de 16) quand il ne porte que l'occultation :
  0,51 → 0,46 ms (bac à sable).
- **Recul du bac à sable en caméra mouvante expliqué** (sautent 0,63 → 1,25 % depuis v62) : occultation coupée,
  0,48 % (le niveau d'avant). La vérité terrain n'a pas d'occultation : un assombrissement qui suit le décor
  y compte comme des sauts. Avant v62, la comparaison à l'identique de l'interface empêchait l'occultation
  sur une grande part des pixels (effet plus faible). Dans le jeu, l'effet suit le mouvement à 98 %
  (`urti_rafale_decalage.py`).
- À vérifier dans RDR2 par le joueur : images par seconde, coût par étape (journal), objets manquants quand le
  budget baisse.

## v68 — URTI : optimisation validée dans RDR2 ; sommets aberrants (scène RT invisible depuis v66)

- v67 dans RDR2 : occultation et reflets sans aucun effet (0,0 % de l'image, masque blanc, passe à 0,13 ms).
  Les rayons ne touchaient rien. Coupable : la combinaison places fixes + reconstruction à chaque image
  (v66) ; places fixes coupées, ou mises à jour permises : tout revient. Cause : des sommets aberrants (non
  finis ou immenses, places gardées ou jamais écrites) ; une reconstruction complète étire la boîte de
  toute la scène et les boîtes intérieures, rangées avec une précision relative à elle, ne valent plus rien.
  La conversion replie maintenant tout sommet non fini ou à plus de 20 km sur le premier sommet de sa zone.
  La v66 jugée « parfaite » par le joueur tournait donc sans occultation RT dans certaines scènes.
- Optimisation mesurée dans le jeu (même partie, Saint-Denis, deux fois de suite) : occultation complète 3,75
  / 3,88 ms, 54,8 / 52,9 i/s ; **au quart 1,99 / 2,16 ms, 59,1 / 57,3 i/s**. Rafale : 0,001 % des pixels
  sautent. Reconstruction 2,25 ms (sous la cible de 2,5 ms : budget inchangé ici).

## v69 — URTI : G-buffer de RDR2 décodé, choix du G-buffer corrigé, cache de surfaces, occultation réglée sur son temps

- **G-buffer de RDR2** (relevé des passes, texels bruts `.raw` de tous les canaux, `tools/reshade/urti_gbuffer.py`) :
  cible 0 = couleur (albedo, RGBA8 sRGB ; alpha : 8 valeurs, un repère de matériau) ; cible 1 = normale du monde
  (z vers le haut, codée 0..1, longueur 1,00), alpha = repère des personnages ; cible 2 = matériau : r type
  (0,5 commun, 0,2 feuillage, 0,31 peau), **g lissé** (flaque 0,60, laiton 0,49, peau 0,53, chemise 0,03),
  **b métal** (boules en laiton 0,80, poteaux de fer 0,57, grilles 0,44, ailleurs 0), a occultation ou
  humidité (réécrit plus tard dans l'image par une autre passe) ; cible 3 = r presque 1 partout, b
  diffusion sous la peau (peau 0,53) ; cible 4 = vecteurs de mouvement (RG16F).
- **Défaut trouvé** : le G-buffer était la dernière liaison de trois cibles ou plus sur la profondeur de
  l'écran. Une passe plus tardive (transparents : 087, 088, 089 + deux du G-buffer) le remplaçait par
  moments, et le G-buffer lui-même est lié avec 3 cibles pour une partie des objets (couleur, normale,
  mouvement) : la « cible 2 » était alors les vecteurs de mouvement, lus comme lissé. Maintenant : le jeu de
  cibles où le jeu dessine le plus, revu toutes les 30 images, en gardant pour une même première cible la
  liaison la plus large. Vérifié dans RDR2 : `5 cibles (formats 29 28 28 28 34)`, stable.
- **Cache de surfaces** (`live_surfcache.hlsl`) : un pixel par bloc 4×4 et par image (tout l'écran en 16
  images), rayon depuis la caméra jusqu'au triangle vu, couleur de l'image retenue par triangle (RGB9E5 +
  identité de l'objet, 8 octets, 21 Mo pour 2,8 millions de triangles). Un reflet dont le point touché est
  caché ou hors de l'écran prend cette couleur au lieu de rester inconnu. Bac à sable : reflets des
  fenêtres prolongés au bord de l'écran, écart à la vérité inchangé (2,59 %, presque tout y est visible à
  l'écran). RDR2 : marche, +0,7 ms, gain visible faible dans les scènes vues (reflets RT sur 2 % de l'image).
- **Constat RDR2** : sol des arcades de Saint-Denis lissé 0,36 (pierre polie, reflet flou), sous le seuil des
  reflets RT (0,5) : les reflets RT ne couvrent que 2 % de l'image. Des reflets flous (rayons tirés selon la
  rugosité + débruitage) sont une étape à part.
- **Occultation : 5,5 ms sous les arcades** (68 i/s sans elle, 48 avec). Le coût ne dépend ni de la portée
  (0,2 m comme 1,5 m), ni des rayons opaques ou du test des trous, ni de la taille de la scène (3 fois moins
  de sommets : pareil) ; il double avec le nombre de rayons. Sous des arcades, presque tout l'écran est une
  surface plane proche (dehors, ciel et lointain sont sautés). Nombre de rayons par pixel calculé maintenant
  réglé toutes les 30 images sur le temps de la passe (cible 2,5 ms, `ao_ms=`, de 4 à 2 rayons).
- Table des zones de la scène RT copiée en mémoire vidéo à chaque image (conversion, reflets et cache y
  cherchaient leur zone à travers le bus PCIe).
- Outils : `urti_rdr2_rapide.ps1` ne prend plus l'ancien journal ni « première image » pour une partie en
  cours, reconnaît le menu sans capture d'étalonnage, ne clique pas quand quelqu'un se sert du PC.
- À vérifier dans RDR2 : réglage des rayons d'occultation et gain d'images par seconde sous les arcades ; coût
  du cache avec la table en mémoire vidéo.

## v70 — URTI : rebond de lumière (éclairage indirect) ; cache de surfaces enfin actif dans RDR2 ; mini-carte protégée

- **Correction de v69** : dans RDR2, le cache de surfaces n'était jamais lu. Le test « le cache couvre la scène »
  (triangles × 3 ≥ capacité) échouait de 2 sommets : la capacité (8 388 608) n'est pas un multiple de 3 (dans
  le bac à sable, si). L'écart mesuré en v69 (0,4 % de pixels) était du bruit. Maintenant : triangles ≥
  capacité / 3.
- **Cache par groupes de huit triangles** : dans RDR2, la plupart des triangles sont plus petits qu'un pixel ;
  un pixel échantillonné par bloc 4×4 ne tombait presque jamais sur le triangle qu'un rayon touche ensuite
  (un rayon d'occultation sur quatre trouvait une entrée, presque aucune du bon objet). Une entrée couvre huit
  triangles consécutifs (voisins dans le maillage) ; entrée vide ou d'un autre objet : les entrées voisines
  du même objet.
- **Rebond de lumière** (`gi=`, 1 par défaut) : les rayons d'occultation qui touchent un obstacle y prennent la
  couleur retenue par le cache (la lumière que la surface renvoie), avec le même poids que l'occultation
  (la lumière du ciel retirée dans cette direction est remplacée par celle de l'obstacle). Trois plans R, G, B
  au quart, débruités par le même filtre que l'occultation (étendu à trois plans, mêmes poids), reçus selon
  la couleur propre de la surface (albedo du G-buffer, copiée à chaque image). Pas de rayon de plus.
  Bac à sable : sable éclairé qui renvoie une lumière chaude au pied des murs et sous les tonneaux,
  scintillement inchangé ; +0,4 ms. RDR2 : +0,85 ms (61 → 58 i/s sur le quai).
- **Personnages exclus du rebond** : alpha de la normale du G-buffer (cible 1, copiée) = repère des
  personnages (0,41 sur Arthur, 0 sur le plancher). Ils bougent, leur historique est rejeté à chaque image et
  le rebond, plus bruité que l'occultation, y tremblait (rafale : 0,20 % de pixels qui tremblent, tous sur
  le personnage).
- **Mini-carte jamais protégée depuis la zone d'interface fixe** : dans la composition, `float4 uiZone` après
  18 nombres était placé par HLSL à la frontière de 16 octets suivante, 8 octets plus loin que ce que
  l'add-on écrit (les deux derniers nombres hors des constantes). Même défaut dans le cache de surfaces.
  Quatre nombres séparés ; vérifié sur tous les shaders (aucun autre).
- Rechargement à chaud aussi pour le cache de surfaces et le débruitage.
- **Mesures RDR2** (quai mouillé de Saint-Denis, rafales en régime établi) : sans rebond, effet 10,2 % des
  pixels (reflets RT du ciel sur le quai : 9,7 %), tremble 0,01 %, saute 0,000 % ; avec rebond, effet 10,8 %,
  tremble 0,01 %, saute 0,001 %.
- Pièges de mesure : une rafale prise quelques secondes après un rechargement de `urti.ini` montre les reflets
  absents par moments (état remis à zéro, à examiner) : attendre 20 s. Un shader de diagnostic rechargé à
  chaud reste actif après l'effacement de son `.cso` (jusqu'au prochain rechargement ou relancement) : une
  première rafale « parfaite » avait été prise avec un rebond gris de diagnostic.

## v71 — URTI dans un deuxième jeu : Horizon Forbidden West (moteur Decima, DX12)

Premier essai hors de RDR2, sans rien adapter d'abord : ReShade 6.8 (dxgi.dll) et l'add-on posés dans le
dossier du jeu (aucun fichier du jeu remplacé), lancement par Steam (lanceur du jeu puis « Continuer » :
`tools/reshade/urti_hfw_rapide.ps1`).

- **Ce qui marche tout seul** : chargement avec Streamline (DLSS) ; capture de la géométrie (1 600 dessins
  captés sur 5 800, 3 millions de sommets dans la scène RT) ; loi de profondeur et projection ; occultation RT
  visible (dessous des planchers, pieds des poteaux) ; G-buffer trouvé (7 cibles, rendu natif 2560×1440).
- **Coût trop élevé** : 69 i/s sans URTI, 43 avec (désert de la Côte embrasée), soit 9 ms. Les objets de
  Decima sont lourds (jusqu'à 464 000 sommets, 13 millions à l'écran contre ~2,5 dans RDR2) : la
  reconstruction de la structure prend 3 à 11 ms, le budget tombe au plancher (1,5 M sommets), 46 dessins
  seulement dans la scène RT et beaucoup d'entrées et sorties par image (rafale : tremble 1,9 %, saute
  0,15 %, contre 0,01 % / 0,001 % dans RDR2). Image native 1440p (pas de mise à l'échelle) : occultation et
  composition plus de deux fois plus chères que dans RDR2 (1707×960).
- **G-buffer de Decima** (relevé des passes) : cible 0 couleur propre (RGBA8 sRGB) ; cible 2 normale en
  octaèdre (RG16 UNORM, deux canaux autour de 0,5) ; cible 5 vecteurs de mouvement (RG16F, seul le
  personnage bouge à l'arrêt) ; cible 6 vide ; cibles 1, 3, 4 réutilisées par le jeu avant la copie de fin
  d'image (à relever). Les réglages de RDR2 y lisaient de mauvais canaux : lissé des reflets pris dans la
  normale, repère des personnages dans une cible R11G11B10 sans alpha.
- **Corrections** : vecteurs de mouvement : avec plusieurs cibles RG16F, la première était prise (presque
  vide dans Horizon : toutes les conventions à égalité) ; après trois mesures sans vainqueur, la suivante est
  essayée (gardée dans `urti_calib.ini`, `motion_target=`). Repère des personnages réglable
  (`person_mask=0` : aucun). Relevé des passes : deux instantanés au plus par image (une image réécrite à
  chaque étape prenait toutes les places avant le G-buffer), 64 au total.
- `urti.ini` d'Horizon : `person_mask=0`, `reflect=0` (reflets coupés tant que le canal de lissé n'est pas
  trouvé).
- **Génération d'images** (DLSS-FG, signalée par l'utilisateur) : pas active d'après le journal (une image
  générée arrive sans dessin du jeu : on en verrait une sur deux). Mais une présentation sur six arrive sans
  dessin compté et certaines captures d'étalonnage tombent sur 0 dessin : Decima enregistre sans doute ses
  listes de commandes en avance, à cheval sur deux présentations (à examiner : URTI prend ces images pour
  des images générées et y garde l'image RT précédente).
- À faire : relever les cibles 1, 3, 4 (rugosité, métal) ; budget de géométrie adapté aux objets lourds
  (structures par objet gardées d'une image à l'autre plutôt qu'une seule reconstruite) ; comptage des
  dessins par présentation.

## v72 — URTI : capture rangée dans l'ordre d'exécution ; génération d'images (DLSS-G) reconnue

Suite d'Horizon Forbidden West, vers un URTI qui découvre le fonctionnement de chaque jeu au lieu de le supposer.

- **Diagnostic** (compteur par liste de commandes : présentation de son premier dessin, présentation de son
  exécution) : Horizon écrit **35 % de ses dessins avant la présentation précédente** (listes de l'image suivante
  préparées en avance) ; RDR2 : 0 %. Rangés à l'écriture, les dessins mêlaient deux images : rang parmi les
  dessins de même signature (donc identité) différent à chaque image, zones lues avant d'être remplies,
  compteurs repris par d'autres dessins. D'où les objets qui entraient et sortaient de la scène RT (60 à 130 par
  image), 80 à 110 nouvelles places par image, une scène RT gonflée de trous (4,2 M sommets pour 1,5 M de
  budget, reconstruction 8,7 ms), des captures d'étalonnage à 0 dessin et une présentation sur six prise pour une
  image générée.
- **Capture dans l'ordre d'exécution** (événement `execute_command_list` de ReShade). À l'écriture, un dessin
  capté reçoit seulement une zone dans la partie du flux de sa fenêtre d'écriture (trois parties tournantes par
  emplacement) et un compteur de cette partie. À l'exécution de sa liste : rang, identité, place dans la scène
  RT et présentation à laquelle il appartient. La conversion lit chaque place depuis sa zone du flux (les places
  sont maintenant dans l'espace de la scène RT, plus dans le flux) ; place gardée sans être dessinée : laissée
  telle quelle ; hors de toute place : triangle plat à l'origine. Compteurs de la partie suivante remis à zéro à
  la fin de chaque image (avant toute liste écrite dans la nouvelle fenêtre). Liste exécutée plus de deux fenêtres
  après son écriture : ignorée (jamais vu). Budget de géométrie par maillage (tous ses exemplaires) : l'identité
  d'un exemplaire n'est connue qu'à l'exécution, le choix de capter se fait à l'écriture.
- **Génération d'images** : activée par l'utilisateur dans Horizon (DLSS-G). ReShade est sous Streamline : il voit
  deux présentations par image du jeu, sur la même file (le jeu exécute ses dessins sur une autre). Horizon ne
  dessine jamais dans l'image présentée (image intermédiaire de Streamline), RDR2 si.
  Règles :
  - une présentation sans dessin exécuté n'avance pas la fenêtre de capture ;
  - image générée = image où le jeu n'a pas dessiné ; sinon, quand les présentations alternent, celle qui suit
    les dessins du jeu, car une génération par interpolation montre l'image fabriquée avant la vraie (URTI les
    inversait : le mélange tombait sur la vraie image) ;
  - le calcul RT se fait dès que les dessins sont exécutés ; l'image fabriquée reçoit le mélange de l'avant-dernier
    et du dernier résultat, la vraie son résultat ;
  - le rebond de lumière est gardé sur les images générées (coupé une image sur deux : clignotement).
  Ordre « fabriquée d'abord » déduit du principe de l'interpolation, pas encore vérifié en mouvement.
- **Mesures Horizon** (même endroit, génération d'images active) : tremble 1,91 % -> 0,04 %, saute 0,148 % ->
  0,002 % ; nouvelles places 80-111 -> 9-15 par image ; refusés 100-460 -> 6-11 ; scène RT 4,2 M -> 2,0 M sommets ;
  coût 9 -> 6 ms par vraie image (97 i/s affichées avec URTI, 137 sans). Mais l'effet tombe à 0,5 % des pixels
  (8,5 % au premier essai) : 609 maillages et 14 M de sommets à l'écran pour un budget au plancher de 1,5 M, les
  grandes structures en bois n'y entrent plus. Suite : structures par objet gardées d'une image à l'autre dans un
  espace stable (mouvement de la caméra mesuré sur notre propre géométrie) au lieu d'une reconstruction à chaque image.
- **RDR2, vérification de non-régression** (quai mouillé de Saint-Denis) : effet 10,2 %, tremble 0,01 %, saute
  0,000 % (comme v70) ; 0 dessin en retard, 518 objets stables (0 nouvelle place, 0 entrée ou sortie par image),
  64,6 i/s.
- Horizon, restes : vecteurs de mouvement jamais départagés (toutes les cibles RG16F essayées) ; cibles 1, 3, 4 du
  G-buffer jamais passées en lecture (le relevé ne peut pas les copier) ; touches envoyées ignorées par le jeu
  (Entrée, F7 : fichiers déclencheurs à la place) ; « Continuer » repéré sur une capture du menu
  (`urti_hfw_menu.py`, sa place change avec la ligne PlayStation Network), parfois deux clics.
- Outils : `premier_plan.ps1` (le jeu est-il au premier plan, sans rien lire d'autre).

## v73 — URTI : mouvement de la caméra mesuré sur notre géométrie ; plantage au passage HDR / Alt+Tab ; zone rendue (DLSS, résolution dynamique) ; loi de profondeur NaN

- **Mouvement de la caméra sans le jeu** (module « mouvement » de l'auto-découverte). La conversion relève des
  paires de sommets : position à l'image précédente et à celle-ci, pour les objets dessinés à la même place dans les
  deux images. Elle en prend une quinzaine par objet, pour qu'un personnage qui suit la caméra pèse autant qu'un
  rocher. Seuls comptent les dessins directs d'un seul exemplaire, dont le maillage n'est dessiné qu'une fois dans
  les deux images : Horizon trie ses dessins, et le n-ième exemplaire d'un maillage n'est pas le même objet d'une
  image à l'autre. `live_camfit` fait l'ajustement rigide (méthode de Horn), robuste, repris cinq fois en écartant
  ce qui bouge ; il repart de l'immobilité si la caméra change d'allure. `live_mvsynth` fabrique les vecteurs de
  mouvement à partir de ce mouvement et de la profondeur (convention 0).
  - Bascule automatique quand ceux du jeu restent illisibles : deux cibles essayées sans vainqueur, ou 3 s de
    caméra en mouvement (notre mesure) sans convention trouvée. Réglage `motion_source=` : 0 automatique, 1 jeu,
    2 nôtres. Gardé dans `urti_calib.ini` (`motion_source=camera`).
  - Bac à sable (caméra qui marche, vérité connue) : translation mesurée 2,7 cm par image, exactement la marche
    du script. Accumulation aussi bonne qu'avec les vecteurs du jeu : scintillement ajouté +0,045 % contre
    +0,046 %, points qui sautent 0,514 % contre 0,522 %.
  - Horizon : encore trop bruité (caméra presque immobile, 3 à 6 cm par image et 3 à 14 cm d'écart). Le filtre
    « un seul exemplaire » vient d'être ajouté (écart 34 mm) ; pas encore satisfaisant.
- **Plantage au passage en HDR ou à l'Alt+Tab** (signalé par l'utilisateur) : l'image d'écran recréée change de
  format (RGBA8 -> RGB10A2). Les photos d'avant l'interface étaient copiées avec l'ancienne disposition, d'où une
  copie invalide, une liste de commandes refusée (E_INVALIDARG) et le plantage. La liste des images d'écran gardait
  aussi les adresses d'images détruites.
  - Corrigé : disposition reprise à chaque changement de format ou de taille, copie sautée si elle ne correspond
    pas, liste des images d'écran refaite à chaque image.
  - Vérifié : Horizon passe en HDR sans planter (plusieurs lancements). L'Alt+Tab lui-même n'est pas encore
    retesté.
- **Zone rendue** : avec le DLSS et la résolution dynamique, Horizon dessine en 1280×720 à 1704×960 dans des
  cibles de 2560×1440. URTI lisait toute la cible : ombres et reflets étirés de 1,5 fois.
  - La zone rendue est maintenant la plus grande des zones de dessin fréquentes des dessins de l'écran (repères 3D
    pleine taille et passes à mi-résolution écartés). Elle est comptée à l'exécution et ne change qu'après dix
    images de suite. Les images restent allouées pleine taille.
  - Vérifié : 1704×960 stable dans Horizon. Réglage d'essai : `render_area=0`.
- **Loi de profondeur NaN** dans `urti_calib.ini`, écrite au moment de la bascule des vecteurs de mouvement. Relue
  au lancement, elle rendait chaque point invalide, et l'envoi du soleil la marquait valide : plus d'occultation
  ni de reflet dans Horizon (vue de diagnostic toute blanche).
  - Corrigé : jamais écrite, jamais relue, jamais adoptée si elle n'est pas finie ; l'envoi du soleil ne la
    marque plus valide. Le fichier d'Horizon est réparé (dernière loi valide).
  - Retour de l'occultation : pas encore vérifié.
- **Reflets de l'eau** (demandés par l'utilisateur) : la surface de l'eau n'est pas dans la profondeur que lit URTI.
  Le marais est dessiné après, transparent : URTI y voit le fond (mode « sol mouillé » : poids mouchetés sur l'eau).
  Le relevé des passes devant l'eau est fait. Prochaine étape : trouver la couche où le jeu écrit la surface de
  l'eau (module « surfaces transparentes »).
- Le script d'Horizon : bonne détection de la partie (motif sans accent : le script est lu en ANSI).

## v74 — URTI : reflets de l'eau (couche des surfaces transparentes planes) ; effet qui disparaissait toutes les cinq secondes ; ombres vérifiées en HDR

- **Effet qui disparaissait et revenait toutes les quelques secondes** (signalé par l'utilisateur) : sans carte
  d'ombres du soleil reconnue, l'étalonnage complet échouait et recommençait toutes les 480 images ; la capture de
  la scène RT s'arrêtait pendant ce temps. Il n'est plus relancé quand les ombres RT du soleil sont coupées (réglage
  par défaut) et la loi de profondeur connue (mesurée aussi à chaque image). Horizon : un seul étalonnage par
  lancement.
- **Ombres (occultation) dans Horizon** : vues blanches dans les captures d'écran de Windows, qui écrêtent une image
  HDR. Les valeurs brutes de la rafale, décodées (PQ), montrent l'occultation (feu de camp, rocher, sol : jusqu'à
  35 %). La loi de profondeur mesurée à chaque image est valide (1,07e-6 + 0,177 / w) ; elle est maintenant écrite
  dans le journal.
- **Reflets de l'eau** (module « surfaces transparentes » de l'auto-découverte). Horizon dessine son eau à
  mi-résolution, dans ses propres images et sa propre profondeur (relevé des passes devant le marais : 17 dessins
  en 1280×720, deux images R11G11B10 et une R32F, la surface du marais visible sous une ligne d'horizon). Elle
  n'est jamais dans la profondeur principale : URTI y voyait le fond.
  - Capture à part (couche 2 de la scène RT, masque 2) de deux sortes de dessins :
    - sur la profondeur principale, ceux qui testent la profondeur sans l'écrire, hors du G-buffer choisi ;
    - tous ceux d'une autre vue de la caméra (même forme que l'écran, résolution réduite).
  - Conversion dans leur propre tampon (1,5 M sommets au plus) ; ces zones sont adressées dans tout le flux.
  - `live_planes` garde les surfaces planes (normales alignées, sommets près du plan moyen, plus d'un mètre, pas
    tournées vers la caméra comme une particule) et rend le reste invisible aux rayons (NaN) : fumée,
    particules, volumes de lumière, ciel.
  - Le rayon de la caméra des reflets cherche cette couche devant le point de la profondeur : le reflet part alors
    de la surface d'eau, avec la normale de son triangle, lisse.
  - Tous les autres rayons (occultation, ombres, cache, reflets) ne voient que la couche 1.
  - Horizon, devant le marais : 225 dessins transparents par image, 15 surfaces planes gardées (58 000
    triangles), eau trouvée sur 9,5 % de l'image (vue des poids), part appliquée pleine sur l'eau.
  - Essai miroir parfait (`reflect_f0=1`) : le marais renvoie les arbres. Mais le reflet est en gros blocs, et des
    taches apparaissent sur la berge (sol au niveau de l'eau).
  - Avec le Fresnel normal de l'eau, de nuit dans la brume, l'effet est discret (le jeu a déjà ses propres
    reflets) : à revoir de jour.
  - `urti.ini` d'Horizon : `reflect=1`, `material_slot=7` (pas de canal de matériau : seule l'eau reflète).
    Réglage `water=0` : pas de couche transparente.
- Le critère « mélangé à l'image » ne suffisait pas : l'eau d'Horizon calcule sa réfraction elle-même et écrit sa
  couleur d'un bloc. Relevés en jeu : environ 1 300 dessins « profondeur testée, pas écrite » par image (le passage
  principal redessine la scène dans le G-buffer après une passe de profondeur), dont 110 mélangés ; pas de
  tessellation.
- « Continuer » d'Horizon : repéré par les lignes repères du menu à plusieurs seuils (captures HDR délavées), pas
  par le nombre de lignes ; l'écran des sauvegardes est refusé (un clic de secours y avait ouvert la liste).
- Pas encore vérifié : RDR2 avec la couche transparente (non réinstallé ; RDR2 garde la version vérifiée en v72),
  les blocs du reflet de l'eau.

## v75 — URTI : cache de géométrie (toute la forêt d'Horizon dans la scène RT) ; reflet de l'eau au masque du jeu ; causes du « filtre / pas filtre »

Essais faits dans le jeu, Horizon Forbidden West au feu de camp du marais (nuit, brume, DLSS + DLSS-G + HDR).

- **Mauvaise profondeur choisie pour toujours (bogue de v74)** : les dessins des autres vues passaient en « transparents »
  et ne comptaient plus pour choisir l'écran principal ; une mauvaise profondeur choisie juste après le menu le restait.
  La scène RT tenait 15 objets au lieu de la forêt. Choix sur une moyenne glissante, tous les dessins comptés.
- **Compteurs de capture effacés (cause d'objets vides au hasard)** : la remise à zéro des compteurs de la fenêtre suivante
  s'exécutait à la fin du calcul, sur la file des présentations, pendant que le jeu dessinait déjà sur la sienne. Remise à
  zéro une fenêtre à l'avance, quatre parties du flux au lieu de trois. Couche de surfaces : 13 % -> 97 % des fenêtres
  complètes. Barrière GPU ajoutée entre la file du jeu et celle des présentations (`queue_sync=0` pour la couper).
- **Cache de géométrie** : le budget par image ne tenait que 37 maillages sur 535 (14 M de sommets à l'écran, reconstruire
  2,5 M coûte 5 ms). Chaque objet hors du budget, capté à son tour (500 000 sommets par image), a maintenant sa propre
  structure, gardée dans une réserve de 1 Go (tampon circulaire) et posée dans la structure de tête par la pose de la
  caméra. Environ 1 000 objets et 3 à 4 M de triangles dans la scène, 250 Mo. `geo=0` pour le couper, `geo_mb=` pour la réserve.
- **Pose de la caméra sans dérive** : accumuler le mouvement image par image dérivait jusqu'à 0,9 m caméra immobile (chaque
  image sans mesure perdait un morceau du décalage sous-pixel du jeu). Des témoins (petits objets immobiles, à plus de 4 m,
  captés à chaque image) gardent seize points de référence dans le repère du cache (`live_refpairs`) ; la pose est recalée sur
  eux. Immobile : stable à 3 cm près ; 96 témoins, environ 900 paires par image, mouvement mesuré sur 290 images sur 300.
- **Reflet de l'eau au masque du jeu** : les pistes « couche transparente » (v74) et « nappe plane » étaient fausses : les
  surfaces planes trouvées sont des tuiles de terrain (19 000 triangles, à 1,89 m sous la caméra), des plaques de brume (les
  grands rectangles sombres vus à l'écran) et des dalles sous le terrain. Un recensement de tous les dessins montre qu'il n'y a
  pas de dessin d'eau à part ; le relevé des passes montre que la 5e cible du G-buffer (format 28), à sa dernière écriture
  (en calcul), vaut exactement 0 sur l'eau, contours de l'herbe compris. `urti.ini` d'Horizon : `material_slot=4`,
  `reflect_channel=0`, `reflect_invert=1`, `smooth_min=0.9`. La copie du matériau se fait à la fin de la dernière écriture
  en calcul, sinon la copie du G-buffer de l'image suivante l'écrase. Couche transparente coupée par défaut (`water=1` pour
  la remettre) : elle coûtait du temps et faisait les faux reflets.
- **Reflet accepté dans deux cas de plus** : rien de touché et l'écran montre du lointain (plus de 20 m : forêt noyée de brume) ;
  point touché derrière un autre objet lointain (feuillage en profondeur : même matière, même brume). Le jeu seul fait ici
  des pavés verts pixelisés ; avec URTI, un miroir continu.
- **Causes du « filtre / pas filtre »** : (1) le reflet était coupé sur chaque image crue générée, une présentation sur deux ;
  (2) la loi de profondeur mesurée à chaque image était fausse (0,08 à 0,13 au lieu de 0,19 : elle fait la moyenne de sommets
  qui n'ont pas tous la même loi), le reflet décrochait sur tout le lac par séries d'images : une loi connue prime maintenant
  (`depth_law_fixed=`) ; (3) le masque était coupé après 2 présentations sans calcul, le rebond après 1 (8 maintenant) ; (4) le
  budget tombait de 105 à 27 objets quand une fenêtre contenait deux images du jeu (hausse limitée à 25 % par fenêtre).
- **Occultation** : réglages d'Horizon `ao=1`, `ao_radius=2.5`, `ao_rough=1` (l'herbe reçoit l'occultation, pente d'ensemble ;
  surfaces rugueuses à moins de 20 m : au-delà, un grain sombre par-dessus la brume).

Mesures, Horizon, réglages finaux, trois rafales, caméra immobile : effet 23 à 26 % des pixels (ombres 18 à 20 %, reflets 5 à
7 %), tremble 3,8 à 6,8 %, saute 0,3 à 1,3 % (encore loin de RDR2 : forêt au vent, personnage qui bouge, brume) ; 300 présentations
sur 300 avec l'effet, reflet jamais coupé (0 sur 120 images relevées). Caméra qui tourne : effet qui suit le lac, sans coupure. Vrai Alt+Tab, aller et retour, deux fois : pas de plantage, effet stable juste
après (15,4 / 15,5 / 15,5 %, sans `ao_rough`). GPU par image réelle : structure du cache 2,3 ms, structure de l'image et
soleil 2,9, ombres 5,3, reflets 0,9, composition 1,6, soit 13 ms ; 137 -> environ 77 images présentées par seconde.

RDR2 (mode histoire, quai au crépuscule, autre lumière que la mesure à 10,2 % de v72, donc effet non comparable) : effet 0,9 %,
tremble 0,09 à 0,13 %, saute 0,04 à 0,06 %, 63,7 images par seconde (64,6 en v72), identique avec `geo=0`, pas d'artefact vu.
Add-on installé, ancien gardé en `urti.addon64.avant_v75`.

Outils : `urti_hfw_test.sh` : `URTI_FORCE`, `URTI_BURSTS`, `URTI_PASSES`, rafales `tourne*`, captures `ecran*` (au rythme réel, une
rafale gèle le jeu), `alttab*` ; `urti.ini` toujours remis ; arrêt sans clic si Steam Cloud attend une réponse (une perte de
connexion de Steam a bloqué un lancement sur « Jouer quand même ») ; `alt_tab.ps1`. Diagnostics : `motion_log=1` (mouvement,
couche, recensement des dessins, planéité des objets), `reflect_diag=`, `trace_draw=`, vue 5 : rouge = point reflété caché.

Pas encore vérifié : joueur qui marche ou court (le jeu ignore les touches injectées : seule la rotation de la caméra est
essayée), voyage rapide et cinématiques (vidage du cache), plein jour (ombres du soleil), autres eaux (rivière, mer) ; le masque
à 0 de la 5e cible n'est relevé que dans ce marais. Le reflet est un miroir plat : pas encore les vaguelettes de la normale du
G-buffer. Réglages d'Horizon dans `urti.ini` : la reconnaissance automatique du masque d'eau reste à faire pour les autres jeux.

## v76 — URTI : la caméra du jeu lue dans sa mémoire au lieu d'être déduite de la géométrie (Horizon : « tout saute », effet / pas effet)

Retour de jeu après v75 : « tout saute dans tous les sens, effet / pas effet, ça tremble de partout ». Les essais de v75 n'avaient
vu que la caméra immobile ou qui tourne en rafale ; le journal de la partie du joueur, lui, montre ce qui se passe quand on bouge :

- **Mouvement mesuré sur les témoins** : valide sur 91 à 188 images sur 300, écart de 460 à 870 mm, 46 à 70 % des paires gardées ;
  encore 320 mm à l'arrêt ensuite (les points de référence relevés pendant une pose fausse restent faux).
- **Cache de géométrie vidé** une à deux fois par fenêtre de 300 présentations (« mouvement impossible à mesurer ») : effet / pas effet.
- **Champ de vision « corrigé » huit fois en une minute**, de 39° à 58° (vrai : 43,0°) : à chaque fois, toute la géométrie changeait
  d'échelle. La condition (assez de rotation) n'est jamais remplie caméra immobile : aucun essai ne pouvait le voir.

Remède : ne plus deviner. Une recherche dans les tampons visibles du processeur (`urti_camscan.inl`, fichier `urti_camscan` à côté
de l'add-on) a trouvé un tampon de constantes de 512 octets, écrit avant la création de sa vue, plusieurs fois par image :
`+240` projection par colonnes (`p00 0 0 0`, `0 p11 0 0`, `jx jy a -1`, `0 0 b 0`, avec `jx`, `jy` le décalage sous-pixel du DLSS),
`+304` caméra vers monde (droite, haut, arrière, position). `urti.ini` : `camera_cb=512` (réglage d'Horizon, lu au chargement) ;
`camera=0` revient à l'ancienne mesure. Crochet sur `CreateConstantBufferView` : seules les vues de cette taille sont relues (environ
45 par image). Le dessin capté retient le numéro de la caméra en cours, le calcul prend le plus récent de sa fenêtre.

Pose, mouvement (vecteurs fabriqués), champ de vision et loi de profondeur viennent maintenant de cette caméra, à chaque image. Plus
de témoins, de vidage pour mesure impossible ni de correction du champ. Le décalage sous-pixel est retiré des sommets mis en cache
et remis dans la pose de chaque image. `motion_log=1` garde la mesure sur les témoins, à part, comme vérification indépendante.

Le jeu accepte une touche maintenue, par code matériel (`hold_key.ps1`, seulement si le jeu est au premier plan) : les rafales
`marche*` et `course*` font enfin avancer le personnage.

Mesures, Horizon, marais de nuit, rythme réel, `motion_log=1` (écart de la caméra lue avec les témoins, médiane / 95 %) :

| Phase | Correction | Rotation | Paires gardées | Images par seconde |
|---|---|---|---|---|
| Immobile | 0,18 / 0,64 cm | 0° | 95 % | 78 à 80 |
| Rotation lente | 0,15 / 0,90 cm | 0° | 92 % | 60 à 77 |
| Rotation rapide | 0,32 / 2,69 cm | 0° | 85 % | 55 à 64 |
| Marche | 0,27 / 1,40 cm | 0° | 86 % | 51 à 68 |

300 présentations sur 300 avec l'effet à chaque bilan de 300 présentations de ces phases, aucun vidage du cache, champ fixe à 43,00°, loi
identique à celle stockée. Rafales (rythme ralenti, sans `motion_log`) : effet continu, immobile 23,5 à 28 %, rotation 22 à 33 %,
marche 18 à 25 %, course 14 à 30 %.

Reste à faire : les ombres passent de 4,4 ms à 8 à 15 ms quand on bouge (occultation doublée en mouvement, forêt dense, cache de 7 M
triangles) ; le tampon reconnu est propre à Horizon (la recherche servira pour les autres jeux) ; course au rythme réel, voyage
rapide, cinématiques, plein jour, autres eaux pas essayés. RDR2 : `camera_cb` absent, chemin inchangé, pas réessayé en jeu avec ce code.

## v77 : une image du jeu à la fois (le vrai « effet / pas effet »)

Retour de l'utilisateur sur la v76 : « ça saute tout le temps, en pause on voit ombres et reflets apparaître et disparaître ». La v76 avait
corrigé la caméra, et ses bilans (« 300 présentations sur 300 avec l'effet ») restaient parfaits : ils comptaient les présentations, pas ce
que le calcul avait réellement vu. Deux outils ont tout changé :

- **la pause du jeu comme banc d'essai** : l'image du jeu n'y bouge plus, tout ce qui varie vient d'URTI (`urti_pause_ana.py`) ; et une rafale
  se déclenche par fichier, sans toucher au clavier ni à la souris ;
- **une trace par présentation** (`urti.ini frame_trace=1`, `urti_trace_ana.py`) : dessins exécutés, zones, caméra, et une rangée de profondeur
  et de masque relue à chaque calcul.

Caméra immobile, jeu pas en pause, la profondeur moyenne de la rangée sautait de 0,0262 à 0,0188, et l'effet moyen de 0,71 à 0,83 ; en
pause, rien (0,0208 à 0,0209).

**Cause.** Avec DLSS-G, le jeu exécute ses listes sur une autre file que celle des présentations. La profondeur était copiée par ReShade à la
présentation, à ce moment-là : le jeu avait parfois déjà commencé l'image suivante, la copie était à moitié dessinée. Le calcul n'était pas
non plus calé sur les images : une fenêtre de capture contenait 0, une partie, une ou deux images du jeu (`princ 1672 3351 0 1 422 1658`).

**Remède (`frame_sync`, avec la caméra du jeu seulement).** La profondeur principale est copiée dans la liste du jeu, à chaque fin d'écriture
(barrière quittant `DEPTH_WRITE`), dans un anneau de 12 tampons en état commun (passent d'une file à l'autre sans barrière). Un repère dans la
liste, relu à l'exécution, dit si cette copie a vu au moins 60 % des dessins de l'image précédente ; le premier qui le fait ferme la fenêtre, la
suite attend le calcul suivant. Un calcul par image complète, plus aucun autre. Plus de repère depuis 16 présentations (menus) : ancienne règle.

**Autres causes corrigées, vues au banc d'essai :**

- décalage sous-pixel : mesuré sur les cascades, il n'existait que sur une image sur cinq dans Horizon (le reste du temps l'écart valait 0 et
  l'absolu restait figé) : masque et reflets posés à côté de l'image, historique relu au mauvais endroit. Il vient maintenant de la caméra du jeu ;
- scène RT instable au bord du budget : 6 à 7 maillages entraient ou sortaient par image, caméra immobile (le nombre de dessins varie avec le
  décalage sous-pixel, cycle de 18 images). Un maillage garde sa place dans le budget + 20 %, deux entrées au plus par image, pas de retour
  dans les deux secondes, et il passe au cache dès l'image suivante : 0,2 à 1,5 par image ;
- maillages énormes (herbe, feuillage) captés par parts : chaque part écrasait dans le cache celle du tour d'avant (identité = rang parmi les
  dessins captés). Chaque part a maintenant ses identités ;
- places gardées sans être dessinées (3 images au plus) : leurs sommets restaient là où la caméra les voyait ; ils suivent maintenant la caméra.

**Deux plantages en route (`DEVICE_REMOVED`), corrigés :**

- `INVALID_CALL` : le jeu met sa profondeur en `COPY_SOURCE` (état 0x800), et ma transition allait d'un état vers lui-même ;
- `DEVICE_HUNG` au chargement, une fois sur trois : l'étalonnage de vérification (capture complète de 512 Mo) tombait pendant que le jeu charge.
  Inutile avec la caméra du jeu : retiré dans ce cas. Par prudence aussi : tampons retirés détruits après 120 présentations (trois ne font
  qu'une image et demie avec la génération d'images), attente si une place d'anneau est reprise avant que le GPU l'ait lue (0 attente mesurée).
  La cause exacte du blocage n'est pas prouvée : le test est passé de 1 plantage sur 3 à 0 sur 4.

Mesures, Horizon, marais de nuit, rythme réel, génération d'images active ; sauts de plus de 8 % de l'effet moyen de la rangée d'un calcul au
suivant :

| Phase | Avant | Après |
|---|---|---|
| Immobile | 75 sur 412 calculs | 0 sur 510 |
| Rotation | 36 sur 489 | 0 sur 536 |
| Marche | 63 sur 322 | 1 sur 402 |
| Course | 25 sur 320 | 3 sur 442 |
| Rotation rapide | 22 sur 350 | 1 sur 451 |

Calculs sur une rangée à plus de 5 % de ciel : 0 partout (0 à 2 par phase avant). Vue `debug_view=11` (bords de la profondeur en rouge sur
l'image, capture à rythme réel, caméra qui tourne) : effet à 3 pixels de l'image au plus (écran de 2560, 29 pixels par image du jeu à cette vitesse). 4 lancements de suite sans
plantage, deux Alt+Tab. Environ 77 images présentées par seconde à l'arrêt (78 à 80 en v76), 51 à 74 en bougeant, trace active.

Reste à faire, vu pendant ces essais : **le HUD et le menu se reflètent dans l'eau** (« 0 / 14 » en double, texte du menu ; pas de photo d'avant
l'interface dans Horizon, le jeu ne dessine pas dans l'image d'écran avec DLSS-G) : c'est une partie de « les reflets changent en pause », avec
le menu du jeu qui assombrit et floute l'image d'où viennent les couleurs ; URTI assombrit aussi le texte du menu ; du grain fin reste sur les
reflets ; 6,5 copies de profondeur par image, à réduire ; plein jour, voyage rapide, cinématiques pas essayés. RDR2 garde la v75 : les changements
de budget et de parts le concernent, à revérifier avant de l'installer.

## v78 : les éclats des reflets

Retour de l'utilisateur sur la v77 : « en pause les reflets se calculent au fur et à mesure et deviennent parfaits, mais en jouant ils ne sont pas
nickel, et des éclats bizarres restent à l'écran ».

Rafales dans le jeu, chaque effet coupé à tour de rôle : sans les reflets, plus un seul point brillant ajouté par URTI. Tout venait des reflets, et
surtout en bougeant (0,015 % de pixels éclaircis de plus de 5 % à l'arrêt, 0,37 % en marchant, sur le feuillage et les bords d'objets).

**Causes et remèdes :**

- **le masque de l'eau d'une autre image que la profondeur** : la cible de matériau est copiée à part, et la copie pouvait être de l'image d'avant
  (et lue pendant que le jeu écrivait la suivante). En mouvement, un pixel de feuille gardait le matériau du ciel (0, comme l'eau) et recevait un reflet
  parti au hasard. Copies dans un anneau de 4 tampons, avec un repère à l'exécution : le calcul prend celle de son image, et l'attend deux présentations
  au plus ;
- **la normale de l'eau tirée de la profondeur** : décalée à chaque image avec le sous-pixel, elle envoyait le reflet ailleurs à chaque image. D'où le grain
  qui ne se nettoyait qu'en s'accumulant, jeu en pause. L'eau est plate et horizontale : sa normale est maintenant le haut du monde, lu dans la caméra
  du jeu (`reflect_up=0` pour l'ancienne règle). Un pixel de matériau « eau » n'est un miroir que s'il regarde à peu près vers le haut et que trois de ses
  quatre voisins sont aussi de l'eau à la même distance ;
- **l'historique des reflets sur ce qui n'est pas de l'eau** : relu à côté en mouvement, il posait le reflet d'un pixel d'eau voisin sur les herbes et l'y
  laissait s'éteindre lentement (ça reste à l'écran). Un pixel sans reflet n'en reçoit plus aucun, un pixel d'eau tout juste découvert prend le reflet tout
  de suite, le voisinage du bornage ne compte que les pixels qui ont un reflet, et un point très clair (particule lue dans l'image) pèse peu dans
  l'historique ;
- **la rotation de la caméra pour reprojeter les reflets** venait des cascades, une image sur dix : elle vient de la caméra du jeu.

**Bug trouvé en route, dans le `frame_sync` de la v77 :** après un gros à-coup, le retard sur le jeu ne se rattrapait jamais (un calcul par image), les zones
du flux étaient reprises avant d'être lues, la scène de l'image restait vide (`3 sommets`), sans aucune erreur. Plusieurs images complètes en attente :
seule la dernière est gardée. Le journal de la partie de l'utilisateur en v77 ne montre pas ce cas.

Mesures, Horizon, rafales (rythme ralenti) ; pixels que les reflets éclaircissent de plus de 5 %, points brillants isolés ajoutés par image :

| Où | Ancienne règle | Nouvelle |
|---|---|---|
| Forêt, en marchant (v77 / v78) | 0,37 %, 32 points | 0,02 %, 17 points |
| Bord de l'eau, pas de côté (`reflect_up=0` / 1) | 6,65 %, 2228 points | 0,17 %, 18 points |
| Bord de l'eau, caméra qui tourne | 1,07 %, 100 points | 0,49 %, 80 points |
| Bord de l'eau, immobile | 0,34 %, 42 points | 0,93 %, 66 points |

Les deux règles du bord de l'eau n'ont pas été prises au même endroit (le personnage a bougé entre les deux) : l'ordre de grandeur du pas de côté est
net, la ligne « immobile » ne prouve rien (cadrage différent, plus de ciel clair reflété). 3 lancements de suite et 2 Alt+Tab sans plantage ; environ 81
images par seconde à l'arrêt en forêt.

Reste à faire : le reflet du jeu lui-même, en gros blocs basse résolution (visible sur l'image sans URTI), reste visible sous le reflet RT là où celui-ci est
faible ; `reflect_f0` à 0,15 et 0,3 ne change presque rien à l'œil, gardé à 0,04 ; le texte du HUD et du menu se reflète toujours dans l'eau ; l'eau est un
miroir plat, sans les vaguelettes (normale du G-buffer à essayer) ; les particules n'ont pas été testées à part. RDR2 garde la v75 : à revérifier avant de
l'installer.

## v79 : la caméra perdue quand les reflets du jeu sont coupés

Retour de l'utilisateur : « j'ai désactivé les reflets du jeu, et quand je cours c'est une bouillie de pixels, le débruiteur fait mal son travail ». Le journal de sa partie dit autre
chose : à l'instant où il change l'option, `caméra du jeu : 300 images, 0 nouvelles, 0.00 cm`, et toutes les lectures refusées, alors qu'il court. La dernière caméra restait figée :
mouvement nul pour le lissage (historique relu au mauvais endroit, d'où la bouillie), objets gardés mal placés. Rien ne le signalait. Au lancement suivant, l'option gardée, le tampon de 512
octets ne contient plus la caméra du tout : il sert au reflet du jeu lui-même.

**Corrigé :**

- caméra perdue (plus de valeur depuis 30 présentations) : l'ancienne mesure sur les témoins prend le relais, le cache est vidé, et de même à son retour. Plus de caméra figée ;
- recherche d'une autre source tant qu'elle manque : vues créées et tampons liés à la racine, deux formes (projection + matrice caméra, vue-projection + vue). Adoption seulement si le décalage
  sous-pixel change, si la source est vue à au moins 60 % des présentations pendant au moins 30, avec peu de valeurs par image, et sans téléportation (5 m). Ensuite, écartée si elle dépasse six
  valeurs par présentation ou fait des sauts de plus de 10 m.

**Pas résolu : aucune source sûre avec les reflets du jeu coupés.** Il ne reste que les tampons des dessins (monde-vue-projection + monde-vue). Les objets tournés y ont l'allure d'une caméra
(faux candidats : 256, 768, 4608 octets, 70 000 « caméras » pour 300 images, sauts de centaines de mètres : c'est ce qui a imposé la règle de continuité). Essai de la valeur la plus fréquente de
chaque image (les objets sans déplacement portent la vraie caméra) : médiane 0,44 cm contre les témoins mais 95 % à 172 cm ; elle alterne avec la caméra vue du repère d'Aloy, puis, ce repère écarté,
avec celui de gros objets (60 % de continuité). Retirée : une caméra parfois fausse est pire que la mesure sur les témoins.

Vérifié : avec les reflets du jeu coupés, 2 lancements et 2 Alt+Tab sans plantage, mesure sur les témoins active, scène RT pleine (800 000 à 900 000 sommets), environ 78 images par seconde à
l'arrêt. Sur une capture en courant, le contour d'Aloy reste en pixels éparpillés dans ce mode ; pas su si cela vient d'URTI ou de la génération d'images (image générée au hasard de la capture).

Pas vérifié : ce build avec les reflets du jeu activés (l'option se change dans le menu du jeu, l'essai automatique ne la touche pas) ; le chemin de lecture du tampon de 512 octets n'a changé que de
forme (mêmes places, même contrôle). Ni que la bouillie disparaît avec eux : l'utilisateur l'a signalée juste après avoir coupé l'option.

Reste à faire : une source sûre sans les reflets du jeu (passes d'écran qui restent : éclairage, brume, DLSS) ; ou masquer les blocs du reflet du jeu en le gardant actif.

## v80 : reflets pris avant le HUD, interface laissée intacte, couleurs apprises des objets

Retour de l'utilisateur : « on voit la barre d'affichage et les menus dans l'eau ; ce serait pas plus intelligent d'utiliser nos propres
reflets qu'on apprend ? ». Les reflets d'URTI prenaient leurs couleurs dans l'image finale de l'écran, HUD et menus compris : aucune
photo d'avant l'interface dans Horizon (`photos 0/0`), le jeu ne dessine jamais dans l'image d'écran avec DLSS-G. Et la composition
assombrissait le HUD et le texte du menu comme le reste de l'image.

**Image du jeu sans son interface.** Le relevé des passes (`urti_passes`) montre, à la fin de chaque image, une cible 2560×1440 en RGBA
16 bits flottants que le jeu écrit avant de poser son HUD : celle que DLSS-G reçoit sans interface. Son alpha est exactement la forme de
l'interface (5,3 % des pixels sur la capture : boussole, invites, santé ; la même que celle du calque d'interface écrit juste après).

- reconnue à l'exécution, sans réglage : parmi les images de la taille de l'écran en RGBA 16 bits flottants à la fin de leur écriture,
  la première finie après le masque de l'eau de l'image (après la profondeur si pas de masque). Adoptée quand c'est la même ressource au
  moins 8 fois sur 10 (en jeu : 40 sur 40, une seule candidate, à chaque lancement) ;
- copiée dans les listes du jeu à la fin de son écriture (anneau de 4, état commun, comme la profondeur), repère lu à l'exécution ;
- remise dans la disposition de l'image d'écran (`live_hudless.hlsl`), une fois par nouvelle copie (les images générées reprennent la
  dernière) : les reflets, le cache de surfaces et la mesure de la lumière y prennent leurs couleurs ;
- masque d'interface de la composition (laissée intacte) : l'alpha (le HUD), plus les pixels immobiles (identiques dans la copie
  précédente) nettement différents de l'image finale deux conversions de suite (le menu de pause, qui n'est pas dans l'alpha). Écart
  toléré 0,06 + 15 % de la valeur. Un pixel qui bouge n'est pas comparé ;
- `hudless=0` dans urti.ini revient à l'ancienne voie ; `hudless=2` fait la conversion sans s'en servir (mesure de son coût).

**Piège : l'image d'écran n'est pas codée comme cette image.** Les valeurs de la cible correspondent à celles des autres cibles du jeu
(PQ), mais l'image d'écran vaut environ deux fois la cible (codes PQ ; relevé brut d'une rafale : rapport constant, erreur médiane 1,3 %,
bien mieux qu'une loi PQ ou une racine). Le jeu applique ce facteur après. Copiée telle quelle, la photo était quatre fois trop sombre dans
les hautes lumières (les reflets assombrissaient l'eau), et la comparaison prenait 95 % de l'image pour de l'interface (plus d'effet du
tout, caméra immobile). Le rapport est appris en continu sur le GPU (`live_hudgain.hlsl`) : médiane du rapport pixel par pixel sur les
pixels immobiles hors interface (histogramme log2, 1,1 % par case), moyenne glissante. Il varie avec la scène : 2,05 à 2,14 au bord de
l'eau, 1,37 à 1,78 au feu de camp et au chargement, 1,73 à 1,89 en pause (le jeu assombrit le fond derrière le menu). Premier essai avec
une somme filtrée par le rapport déjà appris : un rapport faux pris au chargement écartait les bons pixels et restait faux (1,3 à 1,5) ;
la médiane non filtrée ne se piège pas.

**Couleurs apprises des objets du cache de géométrie.** Un reflet qui touche un objet caché ou hors de l'écran n'avait de couleur que
pour la géométrie de l'image (cache de surfaces par triangle) ; sur un objet du cache, rien : le reflet bloqué du jeu se voyait dessous.
Chaque objet du cache a maintenant une place dans une table de 256 couleurs (une par groupe de triangles voisins), apprises par le cache
de surfaces quand l'objet est à l'écran ; l'identité de l'instance porte la place et sa génération, le champ du groupe de touche le
nombre de triangles. Le reflet les reprend quand le point touché n'est pas visible. Pas de mesure séparée de son effet.

**Mesuré en jeu** (marais, jour, caméra du jeu lue, rafales au rythme réel) :

- texte du menu de pause : 97 à 98 % des pixels clairs laissés intacts (avant : assombri de 15 à 38 niveaux) ; HUD en jeu caméra immobile
  64 à 88 % intact (avant : 24 %, assombri de 25 niveaux). La photo des reflets ne contient plus ni le menu ni le HUD ;
- interface prise pour 0,0 à 0,2 % de l'écran en jeu, 1,8 % en pause ;
- variation de l'effet d'une image à la suivante (pixels > 0,05), caméra immobile : 0,83 % (ancienne voie 0,70 % ; sans les deux
  conversions de suite 1,38 %, les herbes faisaient clignoter le masque) ; en pause 0,18 % (ancienne voie 0,62 %) ;
- points brillants ajoutés : 29 en pause, 86 immobile, 114 à 120 en mouvement (ancienne voie, autre vue : 240, 69, 133 à 488) ;
- coût au même endroit, caméra immobile : composition 1,47 → 1,93 ms par présentation (conversion 0,25 ms, usage 0,25 ms), environ
  73 au lieu de 75 images par seconde ;
- une dizaine de lancements d'essai, puis 4 lancements de vérification et 4 Alt+Tab sur la version finale, sans plantage.

Pas fait : le rebond de lumière ne reprend pas encore les couleurs apprises des objets ; masquer le reflet en blocs du jeu.

## v81 : le reflet ne saccade plus avec DLSS-G, rayons plus rapides, mesure par film au rythme réel et ꟻLIP

Retour de l'utilisateur sur v80 : « il faut corriger les fps, on perd trop ; à l'arrêt les reflets se calculent et deviennent bien, mais en
mouvement ils se décalent, ne suivent pas, le personnage se décale, ça gigote dans l'eau, on dirait des couches au-dessus de l'eau ;
il faut une analyse d'image qui montre si c'est bien ou non (le truc de NVIDIA) ».

**Outils de mesure.**

- Film au rythme réel (fichier `urti_film`) : 30 présentations de suite, générées comprises, avant et après URTI, à demi-résolution,
  gardées en mémoire vidéo et écrites seulement à la fin (une rafale écrit chaque image : le jeu ralentit et seules les images calculées
  y sont). Une ligne `FILM` par présentation (générée, calculée) ; à la fin, la profondeur et le masque de l'eau du dernier calcul
  (`profondeur.bin`, `materiau.bin`, ligne `FILMCAM`).
- `tools/reshade/urti_film_ana.py` : ꟻLIP (NVIDIA, flip-evaluator 1.7, déjà installé) entre chaque présentation et la suivante, sur l'image
  du jeu et sur l'image avec URTI ; l'excédent est ce qu'URTI ajoute comme changement perçu (tremblement, saut). Aussi le décalage de
  l'image et de la couche URTI d'une présentation à la suivante (corrélation de phase).
- `tools/reshade/urti_eau_hauteur.py` : hauteur de chaque pixel d'eau d'après la profondeur du film.
- Minutage GPU détaillé (journal « GPU détail ») : chaque étape du calcul et de la composition.

**Cause du décalage en mouvement : la couche d'URTI figée sur les images générées.** Avec DLSS-G, une présentation sur deux ou trois n'a
pas de calcul : URTI y reposait l'ombre et le reflet du dernier calcul tels quels. Film d'un pas de côté : l'image avance de 1 à 3 px à
chaque présentation, la couche d'URTI reste à 0,01 px une fois sur trois puis rattrape d'un coup (9 présentations sur 29 figées).
Corrigé : le résolveur des reflets garde, pour chaque pixel, le mouvement de la surface et celui du point virtuel derrière le miroir ;
sur une image générée, la composition lit l'ombre et le reflet à « ici + 0,5 × mouvement » (`fg_shift=`, 0 : ancienne règle). Mesuré
dans la même partie : 1 présentation figée sur 29 (au lieu de 9), la couche suit l'image à chaque présentation (rapport 1,1, constant).

**Pas la cause :**

- la hauteur du miroir : sur l'eau ouverte, la profondeur du jeu est un plan (−1,59 à −1,61 m sous la caméra) : c'est bien la surface ;
- les taches sombres autour des jambes d'Aloy dans l'eau : elles sont dans l'image du jeu sans URTI (génération d'images sur des
  jambes qui bougent vite). Mais des morceaux d'Aloy (équipement, cheveux) pouvaient entrer dans le cache de géométrie, où un objet est
  supposé immobile : objets à moins de 6 m de la caméra exclus du cache (environ 300 écartés par tranche de 300 images).

**Coût.** Sans URTI, au même endroit : 134 images par seconde ; avec v80 : 69 à 85. Détail par calcul (bord de l'eau) : cache de
géométrie 1,9 ms, structure de l'image 1,8, occultation 2,2, débruitage 2,0, reflets 2,3 ; par présentation : image sans interface 0,35,
reflets stabilisés 0,37, composition 0,5.

- Rayons : chaque rayon d'occultation et de reflet examinait chaque triangle rencontré (sol, rochers, troncs) pour savoir si c'était un trou
  de feuille. Maintenant un rayon opaque d'abord (le matériel trouve le plus proche) ; si c'est un trou vu à l'écran, rayon opaque suivant
  juste après lui (jusqu'à six), la recherche lente seulement au-delà. Même résultat. Mesuré au même endroit (alterné, 30 s chacun) :
  face à l'eau 80,5 au lieu de 71 images par seconde (reflets 2,2 au lieu de 3,9 ms, occultation 1,95 au lieu de 2,85), face à la forêt
  86,3 au lieu de 79,5. `rayons_lents=1` revient à l'ancienne règle.
- Débruitage : l'occultation n'est calculée que sur un pixel par bloc 2×2 à chaque image ; le flou 5×5 lisait 25 pixels pour n'en garder
  que 4 à 9. Il ne lit plus que ceux-là (même résultat) : 2,0 → 1,4 à 1,6 ms.
- Cache de géométrie : 80 objets et 400 000 sommets reconstruits à chaque image ; maintenant les nouveaux d'abord (300 000 sommets), les
  objets déjà gardés remis à jour à 100 000 sommets par image (`geo_new=`, `geo_refresh=`) : 1,2 → 0,5 ms.
- Occultation : plus de rayons supplémentaires là où l'image bouge (`ao_boost=1` pour les remettre).

Au départ (feu de camp, immobile) : 81 → 92 images par seconde ; au bord de l'eau 69,5 → 80,5 (sans URTI : 134). Une dizaine de lancements
d'essai, puis 2 lancements et 2 Alt+Tab de vérification sur la version finale, sans plantage.

**Reste :** structure de l'image reconstruite à chaque calcul (2,2 ms), occultation et reflets dans le feuillage dense (jusqu'à 10 ms en
marchant), composition à chaque présentation. Les reflets d'objets cachés ou hors de l'écran prennent des couleurs apprises par
triangle ou par groupe de triangles : aplats (troncs reflétés en barres pâles), peut-être les « couches » que voit l'utilisateur.

## v82 : un budget de temps sur l'occultation et les reflets, plus de travail inutile hors de l'eau

Retour de l'utilisateur sur v81 : « il faut optimiser l'eau, on descend à 15 fps, tout se calcule tout le temps, les reflets mettent parfois du temps à se calculer,
énormément de zones sont calculées en trop ».

**Ce que dit le journal de sa partie.** Par moments, en moyenne sur 300 images : occultation 45 ms, reflets 20 ms (course, 50 cm et 1,5° par image, végétation dense, cache de 2400 objets).
Avant v81 le maximum était de 15 à 20 ms : **v81 avait aggravé le pire cas**. Le saut de trou en trou (jusqu'à six rayons opaques) était suivi d'une recherche sans limite si le dernier point
était encore un trou : dans l'herbe dense, jusqu'à sept traversées par rayon. Il n'avait été mesuré que caméra immobile.

**Bugs trouvés :**

- le régulateur des rayons d'occultation lisait le mauvais intervalle (repères 2 à 3 : la copie de la carte du jeu, 0,02 ms, au lieu de 3 à 4) depuis qu'un repère avait été ajouté : il croyait toujours
  avoir de la marge, l'occultation restait à quatre rayons quel que soit son coût ;
- un rayon de plus par pixel d'eau pour l'ancrage des reflets, alors que l'ancrage est coupé (`anchor_alpha=0`) ;
- les reflets calculaient normale et planéité sur tout l'écran avant de lire le matériau ;
- l'occultation seule (ombres du soleil coupées) calculait deux normales et lisait la carte du jeu sur tous les pixels, et la planéité plus un voisinage de 8 points (deux fois) même sur les 3 pixels sur 4 sans rayon à cette image.

**Corrigé :**

- rayons bornés : quelques sauts au plus (occultation 0 à 2, reflets 1 à 3), jamais de recherche sans limite (`rayons_lents=1` pour comparer). Au bout des sauts : occultation sans obstacle, reflet sur le dernier point touché ;
- matériau d'abord : hors de l'eau, un reflet coûte une lecture ; caméra connue : reflets sur l'eau seulement (`reflect_all=1` pour les autres surfaces lisses) ;
- occultation : les pixels sans rayon sortent tout de suite ;
- budget de temps par passe, tenu à chaque image, cibles `ao_ms=4` (occultation et débruitage) et `refl_ms=2,5` (0 : pas de budget). Le niveau (0 à 4) monte dès que la passe dépasse sa cible et redescend lentement.
  Occultation : rayons 4 à 1, sauts 2 à 0, portée 50 à 10 m (effacée en douceur). Reflets : sauts 3 à 1, portée 500 à 100 m, un pixel sur 1, 2 ou 4 reçoit un rayon (le motif tourne), les autres prennent la couleur de leurs voisins (`live_reflresolve`).

**Mesuré en jeu,** même parcours (course aller-retour dans la végétation et au bord de l'eau, deux tours alternés, journal « budget de temps ») :

| | sans budget | avec budget |
|---|---|---|
| passage le plus lourd | 39 à 46 images par seconde (occultation 12 à 17 ms, reflets jusqu'à 12 ms) | 74 à 82 (3,6 ms, 2,7 ms) |
| pire image, occultation | 32 ms | 5 à 7 ms, un pic à 16 ms en entrant dans la zone |
| pire image, reflets | 27 ms | 6 ms |

Toutes les zones à reflet sont de l'eau (`reflect_stats=1` : 8 à 25 % de l'écran, 80 000 à 600 000 rayons par image). Film au bord de l'eau avec le budget : image propre (pas de damier ni de blocs), ꟻLIP du même ordre qu'avant.

Reflets qui mettent du temps : à l'arrêt, le reflet ne prenait que 10 % de la nouvelle valeur par image ; passé à 25 % (`refl_alpha_min=`). Mesuré avec ꟻLIP (alterné, deux fois) : à l'arrêt, pas plus de tremblement
(excédent 0,0060 et 0,0044 au lieu de 0,0058 et 0,0053) ; après un mouvement de caméra, moins de changement parasite (0,0156 et 0,0148 au lieu de 0,0214 et 0,0294).

Une dizaine de lancements d'essai, puis 2 lancements et 2 Alt+Tab de vérification sur la version finale, sans plantage ; 92 à 94 images par seconde à l'arrêt au feu de camp.

Pas vérifié : la mer, les grands lacs, la nuit. L'image aux niveaux les plus bas du budget (un rayon d'occultation à moins de 10 m, un pixel de reflet sur quatre) n'a été regardée que sur les films du marais.

## v83 : le vert du marais, les blocs du reflet du jeu, Red Dead mis à jour

Retour de l'utilisateur sur v82 : « dans Horizon c'est full bug, le vert du marais partout c'est dégueulasse ; sur Red Dead c'est clean ; on a des fps mais c'est full bug,
les reflets bug ; sur Red Dead, en ville quand tu marches il y a des traits d'ombres qui apparaissent ».

**Mesuré d'abord : URTI n'ajoute pas de vert.** Films au marais, chaque effet coupé à tour de rôle : part du vert dans l'image 0,343 sans URTI, 0,341 avec ; ni le rebond, ni
l'occultation, ni les reflets n'en ajoutent. Le vert est la couleur de l'eau du jeu.

**Ce qui le rendait visible partout : le budget de v82.** Budget des reflets forcé au plus bas (`refl_ms=0,4`) : la moitié droite de l'eau devient vert vif. La portée des rayons
réfléchis descendait de 500 à 100 m : le reflet sombre des arbres lointains disparaissait et laissait voir le vert du jeu. Dans sa partie, le niveau des reflets était souvent à 2 ou 3.
Corrigé : la portée ne baisse plus jamais ; le budget joue sur les sauts, et sur un pixel sur deux au plus (plus un sur quatre) ; un pixel sans rayon à cette image garde surtout
son historique (borné par ses voisins), la moyenne des voisins ne fait que le corriger. Occultation : la portée descend au plus à 25 m (10 m avant), et au loin un ou deux rayons
suffisent (au-delà de 25 m, un seul). Cible des reflets 3,5 ms.

**Les « couches » : le reflet du jeu sous celui d'URTI.** Le jeu dessine dans l'eau son propre reflet, en gros blocs basse résolution ; à travers le reflet net d'URTI (poids de
Fresnel seul), ces blocs restaient visibles : deux reflets qui ne coïncident pas. Corrigé (`live_waterbase.hlsl`, caméra du jeu connue) : couleur de l'eau du jeu lissée (moyenne des
pixels d'eau par bloc de 8 pixels de rendu, flou 7×7 pondéré par la part d'eau), qui remplace la couleur du jeu là où le reflet compte. Le reflet du jeu est bien plus fort que le
seul Fresnel : lissée sans plus, l'eau devenait terne. Le reflet d'URTI pèse maintenant 3 fois Fresnel sur l'eau (`water_gain=`, 2 à 4 essayés) : l'eau reflète la forêt en sombre,
le vert vif disparaît, les cailloux du premier plan restent visibles. `water_base=0` revient à l'ancienne composition.

**Red Dead.** Traits d'ombre : pas retrouvés. Rafales en marchant dans la cour de Saint-Denis (avec et sans les places gardées, `persist=0`), version installée puis nouvelle version :
0 trait fixe, au plus un trait fugitif par image. Les longues ombres fines des fils (tram, télégraphe) sont dans l'image du jeu sans URTI (capture v50 en écran partagé). Nouvelle
version installée dans Red Dead (l'ancienne gardée, `urti.addon64.avant_v83`) : 76 à 78 images par seconde au lieu de 71 à 76 au même endroit, occultation 1,8 → 1,4 ms,
composition 0,9 → 0,6 ms, pas de plantage. Le reflet y garde l'ancien lissage (10 % par image) : sans caméra du jeu, sa normale vient de la profondeur.

**Mesures de la version finale (Horizon, marais) :** saccades : 2 présentations figées sur 29 en pas de côté (9 avant v81). Avec un reflet trois fois plus présent, l'excédent ꟻLIP
d'URTI monte en mouvement (pas de côté 0,030, marche 0,018) : sa parallaxe se voit davantage ; à l'arrêt 0,008, surtout sur les bords des arbres reflétés. `refl_alpha_min` 0,12
essayé contre 0,25 (alterné, deux fois) : pas mieux (0,0086 et 0,0083 contre 0,0080 et 0,0079) : 0,25 gardé. Sans URTI 124 images par seconde au même endroit, avec 77 à 87.
2 lancements et 2 Alt+Tab sans plantage dans Horizon, un lancement dans Red Dead, même version installée dans les deux jeux.

Images avant / après du README (`docs/images/`) tirées des films de cette version.

Pas vérifié : les reflets sur sol mouillé de Red Dead avec cette version (temps sec), la mer, la nuit.

## v84 : cache de radiance du monde, calque recalé sur l'image affichée, HUD protégé en mouvement

Retour de l'utilisateur sur v83 : « les reflets sont cassés dans Horizon ; il nous faut du vrai ray tracing, on possède les informations, il faut juste tout injecter
comme il faut ; utilise des techniques de NVIDIA ».

**Ce que montraient les films (jeu à gauche, URTI à droite).** L'eau d'URTI était terne et grise, le ciel ne s'y reflétait presque plus, et en marchant le reflet se
morcelait en éclats de couleur plate et en taches vertes. Cause : un point reflété caché ou hors de l'écran recevait une seule couleur par 256e d'objet (un arbre de
200 000 triangles : 800 triangles par couleur), et un rayon parti vers le ciel hors de l'écran ne recevait rien (l'eau du jeu réapparaissait).

**Cache de radiance du monde** (`live_radcache.hlsl`, `live_radhash.hlsli`, d'après SHaRC de NVIDIA ; caméra du jeu connue). Table de hachage spatiale de 2^23 cases
(128 Mo) dans le repère du monde : à chaque image calculée, un pixel sur quatre dépose la couleur que l'image montre dans la case où il se trouve, à trois niveaux (case
de 0,5 % de la distance, soit quelques pixels, et deux plus grossières). Un reflet qui touche un point caché ou hors de l'écran y relit la couleur vue là, mêlée sur les
huit cases voisines. Chaque case retient aussi sa part pleine : un pixel sur seize relance le rayon de la caméra dans la scène RT, chaque triangle traversé avant la
surface visible est un trou (plaque découpée du feuillage) ; hors de l'écran, le rayon réfléchi traverse les feuillages selon cette part apprise. Carte du ciel et du
lointain (octaèdre 512 × 512, par direction du monde) : un rayon qui ne touche rien y prend sa couleur ; les directions jamais vues se remplissent de proche en proche.
Sortie en plus par pixel (`reflMix`) : couleur déjà connue et part à lire dans l'image. `radcache=0` revient aux couleurs par objet. Comptes avec `reflect_stats=1`.
Premier essai : table de 2^22 pleine en moins d'une minute (397 000 dépôts refusés par image) ; avec 2^23 et une case reprise après 2 s sans usage : 2 000 à 14 000.

**Le calque d'URTI avait deux images du jeu d'avance sur l'écran.** Trouvé en refaisant le masque du HUD : avec DLSS-G, l'image finale d'une présentation n'est pas
celle de la dernière copie de l'image sans interface, mais d'une copie plus ancienne (comparaison sur un pixel sur 64 : la plus ancienne des trois dernières le plus
souvent). Vue 11 en tournant : bords de la profondeur du calcul à 27 à 37 pixels (demi-résolution) de ceux de l'image, pour 8 à 9 pixels de mouvement par présentation.
Corrigé : `live_hudgain` suit quelle image du jeu chaque présentation montre et écrit l'avance du calque (1,4 à 1,7 image du jeu en moyenne) ; `live_composite` fait
glisser l'ombre et le reflet d'autant le long du mouvement (remplace le demi-pas des images générées de v81). Même rotation après : 0 à 1 pixel sur la plupart des
présentations. `lag=0` : ancienne règle ; `lag_bias=` : correction.

**HUD.** L'alpha de l'image sans interface vaut 0 partout dans la partie (il ne couvre que certains écrans) : le masque ne tenait que sur fond immobile (v80), le
reflet passait sur le HUD au-dessus de l'eau. Refait : masque = écart entre l'image finale et la copie qui lui correspond, part d'interface de 0 à 1, refait seulement
aux présentations où une copie concorde à 96 % (150 à 290 sur 300). La loi entre les deux images n'est pas un rapport : final = k × (sans interface)^1,13 à 1,25 par
canal (mesuré), apprise en continu (médianes des pixels sombres et des pixels clairs).

**Composition.** Avec le vrai ciel dans le reflet, le poids de 3 × Fresnel de v83 faisait un miroir laiteux qui cachait le fond de l'eau : 1,5 par défaut (1 à 3
comparés au même endroit), et l'eau du jeu n'est lissée que là où le reflet d'URTI domine.

**Mesures (marais, RTX 4070, DLSS + DLSS-G).** 88 à 92 images par seconde à l'arrêt au bord de l'eau, 65 à 80 en mouvement (v83 : 92, 70 à 84) ; cache du monde
environ 0,4 ms par présentation. Deux lancements, course, deux Alt+Tab : pas de plantage. Reste : taches claires ou blocs par endroits dans le reflet (cases grossières,
feuillages), bande au bord de l'écran sans recalage en mouvement, eau parfaitement plate (pas de vaguelettes), Red Dead pas encore essayé avec cette version (v83 y reste).

## v85 : file d'attente des calques (plus d'à-coups), eau du jeu laissée vivante

Retour de l'utilisateur sur v84 : « les reflets sont magnifiques mais ça saccade dans tous les sens, le personnage lag et même le sol ; près de l'eau la mousse pop et
dépop, l'eau se fige par moments, et on perd énormément de fps ».

**À-coups.** v84 faisait glisser le dernier calque de l'avance mesurée (1,5 à 2 images du jeu) le long du mouvement : juste en moyenne, faux dès que le rythme du jeu
varie (film en rotation : image immobile, calque à -3,8 puis -7,4 puis 0 pixel d'une présentation à l'autre). L'appariement exact du calcul et de sa copie sans
interface (repère laissé dans l'ordre d'exécution des listes) n'y changeait rien : la copie du calcul est la dernière 96 % du temps. Corrigé par une file d'attente
(`live_delay.hlsl`) : le calque de chaque image calculée (ombre, reflet stabilisé, mouvement, décalage sous-pixel) est rangé dans une de trois tranches avec le numéro
de son image du jeu ; `live_composite` pose sur chaque présentation la tranche de l'image du jeu qu'elle montre, et ne rattrape le long du mouvement que le reste (une
demi-image, images fabriquées par DLSS-G). Même film après : image -4,70 / -12,73 / -5,19 pixels, calque -4,10 / -10,15 / -4,71. Variation du calque d'une présentation
à l'autre en marchant : 16 et 12 niveaux -> 7,4 et 5,9 ; excédent ꟻLIP d'URTI : 24 % de ce que le jeu change lui-même -> 15 % (marche), 22 % -> 11 % (rotation).

**Eau.** L'eau du jeu lissée sous le reflet (v83) n'était refaite qu'aux images calculées et effaçait la mousse là où le reflet passait un seuil : eau figée par
moments, mousse qui apparaît et disparaît. `water_base=0` par défaut : l'eau du jeu reste telle quelle sous le reflet. Rayon d'ancrage (un rayon de plus par pixel
d'eau, sans effet sur les objets du cache de géométrie) supprimé avec le cache du monde.

Reste : coût des reflets près d'une grande étendue d'eau (3 à 5 ms, budget au plus bas dans la partie du joueur), reflets hors de l'eau (`reflect_all=1`) à valider,
taches de couleur grossières par endroits dans le reflet.

## v86 : moitié moins de rayons de reflet sans scintillement, et où part vraiment le temps

Retour de l'utilisateur sur v85 : « c'est beaucoup mieux mais il y a encore pas mal de bruit ; il faut de bonnes performances ». Journal de sa partie : occultation et
reflets au niveau 4 en permanence, 6 ms chacun quand même, 48 à 55 images par seconde.

**Bruit.** Au-delà du niveau 2, un pixel d'eau sur deux n'avait pas de rayon et prenait pour moitié la moyenne de ses voisins : une image sur deux un vrai rayon, la
suivante une moyenne, les objets fins du reflet scintillaient. Un pixel sans rayon garde maintenant son seul historique (il a reçu un rayon à l'image d'avant) ; du
coup un pixel sur deux à tout niveau (`refl_full=1` : ancienne règle). Occultation : plus jamais un seul rayon, c'est la portée qui baisse (50 à 12 m). Portée des
reflets réduite quand le budget est dépassé (500 à 90 m) : avec le cache du monde, un rayon plus court finit sur la carte du ciel et du lointain, plus sur l'eau verte.
Cache du monde rempli une image calculée sur deux, masque du HUD refait une présentation sur quatre.

**Mesures au bord de l'eau.** Reflets 2,8 à 3,7 ms -> 2,0 à 2,6 ; composition 1,9 à 2,75 -> 1,4 à 1,6. Excédent ꟻLIP à l'arrêt 0,0099 -> 0,0067 ; en marche et en
course inchangé (0,019 / 0,021 ; 0,023 / 0,018).

**Où part le temps (chaque effet coupé à tour de rôle, à l'arrêt).** Jeu seul 131 images par seconde ; URTI avec occultation, rebond et reflets coupés : 98 ; reflets
seuls coupés : 93 ; tout actif : 84 à 90. Les effets ne coûtent que 5 images par seconde chacun : le gros est fixe, par image du jeu : structure de l'image 2,1 ms,
débruitage 1,2 ms (même occultation coupée), cache de géométrie 0,55, structure de la scène 0,3. Baisser les budgets ne change presque rien (occultation 3,4 ms au
niveau 4 contre 3,9 au niveau 0). `blas_refit=1`, `geo_new=100000`, `vertex_budget=200000` : structure de l'image 2,05 -> 1,24 ms, 1 à 2 images par seconde.
Prochaine piste : ne plus tout refaire à chaque image du jeu (la file d'attente des calques permet de servir deux images avec un calcul).

## v87 : objets qui disparaissaient des reflets, Red Dead vérifié

**Red Dead Redemption 2 avec v86** (jamais essayée là) : deux lancements, marche, rotation, rafales : image correcte, mini-carte intacte, 70 à 77 images par seconde
(v83 : 76 à 78), pas de plantage. Installée (ancienne : `urti.addon64.avant_v86`). Le film au rythme réel (`urti_film`, outil de mesure fait pour Horizon) y fait
perdre le périphérique (DXGI_ERROR_INVALID_CALL) : ne pas l'y déclencher.

**Objets qui apparaissent et disparaissent (Horizon).** Journal du joueur : 975 objets chassés de la réserve du cache de géométrie en 300 présentations, réserve à
moitié vide. La réserve est un tampon circulaire : en mouvement (300 000 sommets nouveaux par image) sa tête en fait le tour en quelques secondes et chassait ce
qu'elle rencontrait, objets visibles compris, qui manquaient aux reflets et à l'occultation jusqu'à leur prochaine capture. La tête saute maintenant les objets captés
depuis moins de 600 images. Même course : 315 à 333 chassés (les anciens). Sommets mesurés par image : 500 000 -> 250 000.

`reflect_all=1` en forêt : aucune différence visible (pas de surface lisse hors de l'eau à cet endroit), 0,4 ms : laissé coupé.

## v88 : flashs de toute la couche après un à-coup, reflets des objets fins, essais du débruiteur

Retour de l'utilisateur : « du bruit sur les objets fins dans les deux jeux, il faut bien gérer le débruiteur ».

**Où est le bruit (mesuré).** Red Dead, rafale à l'arrêt : l'excédent d'URTI d'une image à l'autre est sur 0,3 % des pixels. Horizon, film à l'arrêt : presque tout
dans le reflet de l'eau, sur les objets fins reflétés et la ligne de la berge. Les moyennes par film variaient du simple au triple pour un même réglage : un événement
intermittent dominait.

**Flashs de toute la couche.** Trois films sur dix à l'arrêt contenaient un saut : pendant une présentation, tout le calque change (reflet absent ou d'une autre
teinte, occultation du personnage), puis revient. Trace : chaque fois après un à-coup du jeu (présentation à 37 ms au lieu de 12), URTI enchaîne trois calculs de
rattrapage dont les calques diffèrent des autres, montrés deux images plus tard par la file d'attente. Un calque calculé dans les trois calculs qui suivent une
présentation plus de 2,2 fois plus longue que d'habitude n'est plus rangé dans la file : le précédent reste, recalé sur le mouvement (jusqu'à quatre images d'écart).
Seize films à l'arrêt ensuite : aucun saut au-dessus de 0,05 (avant : 0,10 à 0,16), moyenne 0,005 à 0,008 (avant 0,007 à 0,018). Première règle essayée (calcul sur
la présentation qui suit un calcul) : écartait jusqu'à 80 calques sur 300 présentations en mouvement, le calque ne suivait plus l'image en rotation ; abandonnée.

**Reflets.** Couleur reflétée lue sur une petite zone (quatre lectures, grille tournée, 1 pixel : `refl_soft=`) au lieu d'un point : avec le décalage sous-pixel de
chaque image, un miroir parfait tombait tantôt sur une branche, tantôt à côté. Pire saut à l'arrêt 0,06 à 0,13 -> 0,016 à 0,034. Pré-filtre spatial avant
l'accumulation (`refl_spatial=0.5` : le pixel mêlé à la moyenne de ses voisins à reflet) : petit gain.

**Débruiteur de l'occultation.** Longueur d'historique par pixel (`denoise_len=`) : un pixel découvert fait la moyenne vraie de ses premiers échantillons. Gain non
mesurable sur les films ; gardé (sans coût). Flou élargi tant que l'historique est court : +2 ms, retiré. Objets fins remplis avec l'occultation de leur voisinage :
huit fois plus de bruit dans le feuillage de Red Dead (2,8 % des pixels au lieu de 0,3 %), retiré.

**Vérifié.** Horizon : deux lancements, course, deux Alt+Tab, pas de plantage ; 88 à 94 images par seconde à l'arrêt. Red Dead : marche, rotation, rafales, pas de
plantage, 67 à 71 images par seconde, bruit inchangé (0,34 % des pixels). Installée dans les deux jeux.

## v89 : scènes lourdes, relecture du cache du monde à une case

Journal de la partie du joueur (v88) : à un endroit chargé, occultation et reflets au palier d'économie maximal en permanence, et quand même jusqu'à 14,5 et
17,4 ms (142 images sur 300 au-dessus de 8 ms pour les reflets), 41 à 71 images par seconde. Deux causes : la relecture du cache du monde lisait les huit cases
voisines (jusqu'à quarante lectures en mémoire par point, à chaque traversée de feuillage) ; le dernier palier ne descendait pas assez.

Relecture à une seule case, à une position tirée au hasard dans un voisinage de la taille d'une case, nouvelle à chaque image (comme SHaRC de NVIDIA) : le lissage
se fait par l'accumulation des reflets. Dernier palier : reflets un pixel sur quatre et sans saut de feuillage, occultation sans saut. Au bord de l'eau, même endroit,
contre v88 : reflets stabilisés 1,2 -> 0,9 ms ; palier maximal forcé : reflets 0,55-0,9 -> 0,26-0,69 ms. Scintillement inchangé (à l'arrêt 0,0055-0,0071 contre
0,0061-0,0069 ; en marchant 0,0155 contre 0,0160). Pas pu retourner à l'endroit lourd du joueur (la partie repart au feu de camp).

Vérifié : Horizon deux lancements, course, deux Alt+Tab ; Red Dead marche, rotation, rafales, 62 à 70 images par seconde, bruit inchangé (0,35 % des pixels).
Installée dans les deux jeux.

## v90 : traits qui scintillent dans les rues de Red Dead, calques d'images presque vides

**Red Dead, « traits » en ville.** Rafales à l'arrêt sur le balcon de Saint-Denis, chaque effet coupé à tour de rôle : pixels qui scintillent (plus de 3 niveaux
d'une image à l'autre, en plus du jeu) 0,46 % tout actif, 0,11 % sans les reflets, 0,38 % sans le rebond, 0,38 % sans l'occultation. Les reflets en font l'essentiel,
et la carte montre des traits en diagonale dans la rue : rails du tram, bords de pavés, trottoirs. Un pixel lisse isolé recevait un reflet dont la direction, tirée de
la profondeur (pas de caméra du jeu dans Red Dead), changeait à chaque image. Reflet maintenant seulement sur une surface lisse étendue : au moins 5 des 8 voisins à
2 pixels lisses aussi et sur la même surface (`refl_coherent=0` : ancienne règle). Même endroit, deux fois chacun : 0,19-0,20 % -> 0,06 %.

**Horizon, flashs restants.** Un film à l'arrêt montrait encore deux sauts forts (0,10-0,12) : un calcul dont l'image n'avait que 13 dessins du jeu exécutés avant
la présentation (d'habitude 3 800 ; les listes du jeu étaient passées dans la fenêtre d'avant), sans à-coup assez long pour la règle de v88. Un calcul avec moins
d'un dixième des dessins habituels n'est plus rangé dans la file d'attente des calques. Seize films à l'arrêt : plus aucun saut au-dessus de 0,06 ; trois petits
(0,05-0,06, couche 7 niveaux au lieu de 15 à 28).

Vérifié : Horizon deux lancements, course, deux Alt+Tab ; Red Dead marche, rotation, rafales, 67 à 72 images par seconde. Installée dans les deux jeux.

## v91 : reflets qui tremblent, régulateur du budget qui suivait les pics

Retour de l'utilisateur : « énormément de bruit sur les textures, scintillement sur les endroits fins, les reflets tremblent ; ça bug beaucoup sur Horizon ».

**Qui fait le bruit dans Horizon.** Même aller-retour au bord de l'eau, chaque effet coupé (excédent ꟻLIP moyen, à l'arrêt / en marchant) : tout 0,0091 / 0,0180 ;
sans occultation ni rebond 0,0104 / 0,0230 ; sans rebond 0,0093 / 0,0191 ; sans reflets 0,0057 / 0,0120 ; occultation sans l'herbe 0,0082 / 0,0180. Les reflets.

**Reflets.** À l'arrêt, deux fois chacun : base 0,0069 et 0,0059 ; 10 % de neuf par image (au lieu de 25) 0,0045 et 0,0053 ; un rayon par pixel 0,0044 ; les deux
0,0040. En marchant, pas de différence (0,010-0,011). Gardés les deux (`refl_alpha_min` 0,1, `refl_full=1` : un pixel sur deux seulement quand le budget le demande).
Coût des reflets au bord de l'eau 2,4 -> jusqu'à 3,5 ms (le budget s'en charge).

**Régulateur.** Journal du joueur : pics d'« occultation » de 25 à 50 ms (le jeu prend la file au même moment), un pic montait le palier de deux, la qualité ne
revenait qu'un palier toutes les 40 images : occultation au palier 2 à 3,7 en moyenne, donc moins de rayons et de portée, plus de bruit. Décision maintenant sur la
médiane des 9 dernières images (montée d'un palier si elle dépasse la cible de 25 %, retour si elle est sous 65 %, toutes les 30 images).

Vérifié : Horizon deux lancements, course, deux Alt+Tab ; Red Dead rafales, 67 à 70 images par seconde, bruit 0,10 % des pixels. Installée dans les deux jeux.

## v92 : où part le coût fixe (mesures), compteurs au journal

Nouvelles lignes du journal toutes les 300 présentations : durée des présentations (médiane, P95, P99, pire), temps processeur passé à intercepter les dessins,
dessins et sommets rejoués par sorte (écran principal, carte du soleil, transparents, autres écrans, zone de mesure).

**Horizon, au bord de l'eau, à l'arrêt** (durée médiane d'une présentation) : URTI coupé 7,5 ms (131 images par seconde) ; URTI sans effets, presque rien rejoué
9,2-9,3 ms (108) ; sans effets 10,1-10,5 ms (98-101) ; tout actif 13,0-14,3 ms (74-80).

- Interception des dessins : 3,2 à 4,5 ms de processeur par présentation (~2 µs par dessin, ~2 000 dessins). Supprimer le verrou global des événements de
  liaison (état de chaque liste lu sans verrou) n'a rien changé : ni le temps de capture ni les images par seconde. Retiré.
- Sommets rejoués : 1,4 million par présentation, dont 1 million pour la carte du soleil, inutile dans Horizon (ombres du soleil coupées, et direction du soleil
  mesurée sur 0 % des images). Rejouée par huitièmes (`sun_eighth=1`) : 0,6 million, gain dans le bruit de mesure (± 3 images par seconde). Laissé coupé par défaut.
- Le coût fixe s'explique par le travail GPU chronométré : par image du jeu, scène de l'image ~2 ms, passe d'ombres et débruiteur ~1,2 ms (même occultation
  coupée), composition ~0,45 ms par présentation, structure 0,6 ms : ~5,2 ms, l'écart mesuré (98 contre 131 images par seconde).
- Scène de l'image : 822 000 sommets -> 2,0 ms ; budget 250 000 -> 533 000 sommets, 1,56 ms, ~80 images par seconde ; budget 120 000 -> 241 000 sommets, 1,30 ms mais
  66 images par seconde (occultation et composition plus chères) ; mise à jour au lieu de reconstruction : la scène change presque à chaque image, gain nul. Environ
  1,1 ms de la reconstruction ne dépend pas de la taille.

Red Dead : 70 à 71 images par seconde, 3 800 dessins vus par présentation, 5,1 ms de processeur à les intercepter, 1,55 million de sommets rejoués (écran principal).

## v93 : débruiteur hors de portée sauté ; deux pistes fermées

- Débruiteur de l'occultation et du rebond : les pixels au-delà de la portée de l'occultation (la passe des rayons y écrit « rien ») ne sont plus filtrés
  (`denoise_far=0` : ancienne règle). 1,18-1,26 -> 0,94-1,05 ms par image du jeu, image inchangée.
- Essayé et retiré : sommets inutilisés de la scène de l'image marqués inactifs (NaN) au lieu de triangles plats. Reconstruction 2,1 -> 1,9 ms, mais rayons un peu
  plus lents (occultation 1,54 -> 1,64-1,80 ms, reflets 2,36 -> 2,67-2,95) : rien de gagné.

Bilan du coût d'URTI dans Horizon (tout actif, par image du jeu, ~10,5 ms, deux présentations par image avec DLSS-G) : scène de l'image ~2 ms, occultation ~1,5 ms,
débruiteur ~1 ms, reflets 2,3-2,7 ms, composition et reflets stabilisés ~2,1 ms, structure 0,65 ms. Pas de gros gaspillage isolé : la suite passe par moins de
travail par image (effets calculés à tour de rôle) plutôt que par des économies de 0,2 ms.

Vérifié : Horizon deux lancements, deux Alt+Tab ; Red Dead 71 images par seconde, bruit 0,17 % des pixels. Installée dans les deux jeux.

## v94 : normales du G-buffer pour les reflets ; orientation de la caméra de Red Dead

- Normales exactes du jeu lues dans le G-buffer (`gnormal_slot=`, `gnormal=`, `gnormal_signs=`) : Horizon les range en octaèdre RG16, repère de la caméra, z inversé
  (`gnormal_slot=2`, `gnormal=1`, `gnormal_signs=4`, accord 0,73-0,84 avec la normale tirée de la profondeur) ; Red Dead en RGB8 dans le repère du monde (`gnormal_slot=1`, `gnormal=2`).
- Red Dead : la caméra du jeu n'est pas lue, l'orientation est donc résolue à chaque image sur la carte graphique (`live_rotfit`, méthode de Horn) à partir de
  ~13 000 pixels plans : elle suit la souris, le haut de la caméra retombe sur le Z du monde. Accord 0,46-0,82 selon la scène (le relief des textures est dans
  ces normales) ; la mauvaise rotation donnerait ~0 : seuil 0,35. Un échantillonnage tombait une image sur quatre (occultation au quart) : corrigé.
- Reflets : la normale du jeu remplace celle de la profondeur quand elle regarde la caméra et s'accorde à peu près avec elle (`gnormal_reflect=1`, actif dès que
  `gnormal=` est réglé).
- Mesuré (rafales, part des pixels à plus de 3 niveaux) : Red Dead 0,09 / 0,04 / 0,04 % avec contre 0,04 / 0,05 % sans, rien de gagné (le relief des pavés bouge
  avec le décalage sous-pixel) ; Horizon : résultats mêlés (0,22 contre 1,16 % à l'arrêt, puis 4,64 contre 2,72 %). Laissé coupé par défaut dans les deux jeux.
- Bruit d'URTI dans Red Dead, effet par effet (rafales à l'arrêt et en tournant la caméra) : 0,00 à 0,06 % des pixels, alors que l'image du jeu varie elle-même de
  2 à 5,5 niveaux par image (vent dans le feuillage, lissage temporel du jeu). Dans Horizon en marchant, l'excédent est surtout sur l'eau (reflets nets qui défilent,
  à départager du vrai scintillement).

Vérifié : Red Dead cinq lancements (un plantage causé par l'ouverture du Social Club), Horizon un lancement. Installée dans les deux jeux (réglages par défaut : comme v93).

## v95 : soleil retrouvé dans Red Dead, cases du menu de ReShade

- **Soleil perdu depuis la v74 dans Red Dead** (0 % des images) : un étalonnage avait pris pour carte du soleil une profondeur 1024 × 1024 vue dans l'axe
  de la caméra (soleil (0,002 0,059 0,998)) et l'avait enregistrée dans `urti_calib.ini` ; comme les ombres RT du soleil sont coupées, plus aucun
  étalonnage ne la remplaçait. Corrigé : une direction à moins de 14° de l'axe de la caméra est refusée ; dans un jeu sans caméra lue, le soleil est
  recherché tant qu'il n'est pas suivi, **seulement en partie** (scène RT d'au moins 20 dessins), de plus en plus espacé sans soleil (jusqu'à 32 intervalles).
  Un essai qui cherchait dès le menu, toutes les 120 images, a mené Red Dead à « mémoire saturée » : écarté. Mesuré : soleil suivi sur 100 % des images,
  46 paires par image, 4 cascades, 0,04° d'une image à l'autre ; soleil / ciel mesuré 0,21-0,25 (soir couvert). 60 images par seconde.
- **Menu de ReShade** : cases « Reflets ray tracing », « Occultation ambiante ray tracing (RTAO) » (force, portée en mètres, 2,5 par défaut : 1,5 avant
  dans Red Dead), « Éclairage indirect ray tracing (RTGI) » (force) ; relues à chaque présentation, elles multiplient les réglages d'urti.ini. Le rebond
  garde ses rayons même si l'occultation est décochée. Pas encore de case pour les ombres du soleil (le mode actuel coupe le rebond).
- Constat (rafale, même image avant / après) : URTI assombrit 3,5 % des pixels de plus de 5 % (lierre, coins du mur, pied du mur), 0,3 % de plus de 15 % ;
  le rebond n'éclaircit rien de visible. Point de départ de l'audit pour un vrai éclairage RT.

## v96 : occultation RT sur toutes les surfaces et visibilité du ciel à grande portée

- **Normales exactes du jeu pour l'occultation** (`gnormal_use=1`) : dans Red Dead, celles du G-buffer (repère du monde) tournées par l'orientation de la
  caméra résolue à l'image précédente (live_rotfit). L'occultation vaut maintenant aussi pour les surfaces qui ne sont pas planes (troncs, rochers,
  objets, personnages), où la normale tirée de la profondeur n'était que du bruit. Le feuillage vu à travers reste exclu. La comparaison et la rotation
  ne se mesurent que sur les surfaces planes (sinon le bruit de la profondeur les fausserait).
- **Visibilité du ciel à grande portée** (`ao_sky=0.6`, `ao_sky_range=12` m ; curseur « Ombre d'ambiance à grande portée » dans ReShade) : un ou deux
  rayons longs par pixel calculé disent si la canopée, un toit, une falaise cache le ciel ; ne retirent que la lumière du ciel. **Seules les directions
  au-dessus de l'horizon comptent** (renvoyées par symétrie, pondérées par leur hauteur) : un premier essai comptait le sol sous les faces verticales comme
  du ciel caché et assombrissait en bloc murs et personnages (26,7 % des pixels de plus de 5 %, mur uniformément gris). Le haut du monde vient de
  l'orientation de la caméra : sans elle (Horizon pour l'instant), pas de rayons longs.
- Mesures dans Red Dead (rafales, même image avant / après, part des pixels assombris de plus de 5 % / 15 %) : v95 4,2 / 0,4 % ; contact avec les
  normales du jeu 6,6 / 0,5 % ; avec le ciel 13,2 / 3,1 % (pied du mur, dessous de la charrette, arbres, sol abrité). Scintillement ajouté 0,11 % des
  pixels à l'arrêt, 0,00 % en tournant la caméra. Passe d'occultation 1,2 -> 2,5 ms. Horizon vérifié (pas de plantage, 62-67 images par seconde).
- Réglages de Red Dead ajoutés : `gnormal_slot=1`, `gnormal=2`, `gnormal_use=1`, `gnormal_reflect=0` (urti.ini d'avant gardé en `urti.ini.avant_v96`).

## v97 : occultation « franche » par défaut, objets fins écartés

- Choix du joueur : un effet franc. Valeurs physiques au lieu des valeurs prudentes : `ao=1` (0,8 avant), `ao_sky=1` (0,6), `ao_sky_range=16` m (12).
- Premier essai : 15-19 % des pixels assombris de plus de 5 %, mais scintillement ajouté 0,37-0,78 % des pixels à l'arrêt (0,07 % sans les rayons longs),
  sur les branches, le lierre, la grille : objets fins qui bougent au vent, où un ou deux rayons longs passent ou non entre les feuilles.
- **Surfaces continues seulement** pour l'occultation par les normales du jeu et pour les rayons longs : à 3 pixels dans les quatre directions la
  profondeur reste proche (sol, murs, troncs, rochers, personnages) ; branches, brindilles, bords de feuillage écartés.
- Mesures (Red Dead, rafales) : 14-19 % des pixels assombris de plus de 5 %, 4,5-6,2 % de plus de 15 % (v95 au même endroit : 8,4 / 1,8 %) ;
  scintillement ajouté 0,01-0,16 % à l'arrêt, 0,01 % en tournant. Passe d'occultation 2,7 ms.

## v98 : occultation et rebond décochés par défaut, carte du soleil plus rejouée sans emploi

- Verdict du joueur sur la v97 : l'occultation laisse des **traînées partout** (son accumulation dans le temps ne suit pas le mouvement), le résultat
  « vraiment pas ouf » ; seuls les reflets sont bons. Les rafales mesuraient le scintillement, pas ces traînées en mouvement : le chiffre ne les voyait pas.
- Cases « Occultation » et « Rebond » décochées par défaut dans urti.fx et dans les préréglages des deux jeux (copies `ReShadePreset.ini.avant_v98`).
- Sans occultation, sans rebond et sans ombres du soleil, **la carte du soleil n'est plus rejouée** et le soleil plus cherché (il revient dès qu'une case
  est recochée) : Red Dead 3,8 -> 1,7 millions de sommets rejoués par présentation, capture 5,9 -> 4,8 ms de processeur ; passe d'ombres 0,3 ms.
- Horizon : scintillement ajouté 0,34 -> 0,05 % des pixels (l'occultation en était aussi la source), 66 images par seconde.

## v99 : reflets sur les surfaces brillantes (parquet, bois ciré, métal, pavés)

- Demande du joueur : les reflets sont ce qui marche ; les étendre. Jusqu'ici seules les surfaces au lissé ≥ `smooth_min` (0,5 dans Red Dead) avaient
  un reflet RT ; les sols de Red Dead (~0,36) en étaient exclus.
- `gloss_min=` (0 : coupé ; 0,2 dans Red Dead) : de ce lissé à `smooth_min`, le reflet entre en douceur (poids Fresnel × rampe × `gloss=`, 1 par défaut) ;
  au-dessus de `smooth_min`, rien ne change. Case « Reflets sur les surfaces brillantes » dans ReShade.
- **Flou selon la rugosité** : demi-angle du lobe ~ alpha = rugosité², empreinte au point reflété = distance × alpha / (distance de l'œil + distance),
  au plus 60 pixels de rendu ; rangé avec la distance du point reflété (w = distance + 20 000 × rayon : la passe des reflets n'a plus de place dans sa
  signature racine) ; lu par live_reflresolve sur un disque de huit lectures (spirale de Vogel tournée par pixel). Approximation nommée : un rayon miroir
  puis une lecture floue, pas de rayons tirés dans le lobe (pas de bruit ; le flou ignore ce qui est caché à l'écran).
- Mesures (Red Dead, Saint-Denis, rafales) : 20,7 % de l'écran à reflet ; image changée de plus de 5 % sur 6,5 % des pixels (0,2 % sans ; 3,6 % au seuil
  0,3) ; scintillement ajouté 0,12 % à l'arrêt, 0,07 % en tournant ; reflets 0,69 ms, composition +0,5 ms ; 63 images par seconde. Horizon : coupé.
- Verdict du joueur : « beaucoup trop », des reflets à trop d'endroits et trop loin. Remis comme avant : `gloss_min` retiré des réglages de Red Dead, case
  « Reflets sur les surfaces brillantes » décochée par défaut (le code reste, coupé). Demande suivante : mieux décider quelles surfaces méritent un reflet.

## v100 : reflets brillants triés (sols proches seulement)

- Tri des surfaces brillantes (lissé entre `gloss_min` et `smooth_min`) avec les données du jeu, au lieu d'un seuil seul : **un sol** (normale à moins de
  32° du haut du monde, donné par l'orientation de la caméra dans Red Dead), plan et étendu (règles des miroirs), **pas du feuillage ni de la peau**
  (type de matériau de Red Dead, canal rouge de sa cible de matériau : 0,2 et 0,31), **proche** (effacé de 10 à 18 m). Red Dead : `gloss_min=0.3`,
  `gloss=0.6`. Un réseau de neurones a été écarté : pas de données d'entraînement, plusieurs ms par image, et il devinerait ce que le G-buffer donne exactement.
- Mesures (balcon de Saint-Denis, rue en contrebas) : 1,2 % de l'écran à reflet (20,7 % en v99) ; image changée de plus de 5 % sur 0,4 % des pixels ;
  scintillement ajouté 0,01 % ; reflets 0,22 ms. À juger dans un intérieur ou sous les arcades (pierre polie 0,36).

## v101 : flaques de nouveau nettes, sols brillants vraiment actifs, règle des miroirs remise

- **Régression de v99-v100** (signalée par le joueur : « on distingue mal ce qui est reflété, avant les flaques avaient de plus gros reflets ») : le flou
  selon la rugosité s'appliquait à toute surface sous un lissé de ~0,86, flaques comprises (0,60). Il ne vaut plus que pour la bande brillante (sous
  `smooth_min`) : les miroirs, l'eau et les flaques sont de nouveau nets.
- **Sols brillants jamais actifs en v100** : le tri prenait le haut du monde dans l'orientation de la caméra, résolue seulement quand l'occultation tourne
  (coupée depuis v98). Le sol se reconnaît maintenant à la normale du monde du G-buffer de Red Dead (z > 0,85), liée aux reflets dès qu'elle existe.
- **Règle des miroirs (v90) affaiblie** quand les reflets brillants étaient actifs : un miroir comptait des voisins seulement « brillants » ; des poutres
  du plafond des arcades recevaient un reflet. Un miroir exige de nouveau des voisins miroirs.
- Essayé et retiré : sauter la passe d'ombres sans occultation (0,3 ms) : le masque restait à zéro, toute l'image assombrie (95 % des pixels changés).
- Mesures sous les arcades de Saint-Denis : sol de pierre polie reflétant le couloir éclairé, plafond inchangé ; scintillement ajouté 0,06 % à l'arrêt,
  0,00 % en tournant ; reflets 0,25 ms, 71 images par seconde. Scintillement des fenêtres et câbles au loin : dans l'image du jeu seule (11,5 % des pixels
  varient de plus de 3 niveaux d'une image à l'autre au balcon), pas ajouté par URTI (0,05 %).

## v102 : une seule case dans le menu, occultation propre (sans personnages ni feuillage, débruiteur réactif)

- Demande du joueur : plus de réglages affichés, tout actif par défaut avec les meilleurs réglages ; l'occultation peut être belle si elle ne traîne et
  ne scintille pas. Menu de ReShade : seule la case « URTI » visible ; reflets, sols brillants proches, occultation et ombre d'ambiance actifs ; rebond de
  lumière coupé (invisible, ~0,9 ms). Valeurs enregistrées dans les préréglages effacées (copies `ReShadePreset.ini.avant_v102`).
- **Mesure des traînées** (`trainee.py`) : rafales en tournant la caméra ; décalage de la carte d'assombrissement d'URTI comparé à celui de l'image du jeu
  (retard 0 : accrochée) et écart qui reste une fois la carte précédente recalée sur le mouvement de l'image.
- Causes trouvées : occultation sur les personnages, chevaux (animés) et la canopée (au vent) ; rayons longs sur les branches ; bornage de l'historique
  trop large en mouvement.
- Corrigé : **pas d'occultation sur les personnages** (alpha des normales du G-buffer de Red Dead) **ni sur le feuillage et la peau** (type de matériau,
  cible de matériau liée maintenant à la passe d'occultation, t10) ; rayons longs réservés aux grandes surfaces continues (continuité à 6 pixels) ;
  `ao_sky` 1 -> 0,7 ; bornage de l'historique resserré en mouvement (moitié à 4 pixels par image et plus).
- Mesures (Red Dead, rue arborée de Saint-Denis) : écart après recalage en tournant 4,0-4,7 % -> 1,5-1,7 % ; scintillement ajouté à l'arrêt 1,49 % ->
  0,22 % des pixels ; assombrissement moyen 4-6 % de l'image ; retard 0,00-0,14. Horizon : occultation 0,24 % (0,15 % sans), 67 images par seconde.

## v103 : le terrain de Red Dead dans la scène RT (dessins tessellés)

- Étape 1 du « transport de lumière » demandé par le joueur (rebond du soleil en nature) : le terrain, dessiné par tessellation, manquait à la scène RT.
- Capture : les pipelines à hull + domain shader (sans géométrie ni mesh shader, qui écrivent la profondeur) gardent leur code ; leur pipeline de capture
  rejoue VS + HS + DS (topologie PATCH) et la sortie de flux prend la position après le domain shader. Taille de sortie inconnue d'avance : 32 sommets par
  point de contrôle d'abord, puis la taille apprise du maillage (comme les ExecuteIndirect), 196 608 sommets au plus. Réglage `tess=1` (Red Dead seulement ;
  Horizon garde ses dessins tessellés à l'écart).
- Mesures (rue de Saint-Denis) : dessins tessellés ignorés 11 -> 6 par présentation (les autres n'écrivent pas la profondeur de l'écran) ; structure de
  l'image 1,19 -> 1,71 M sommets, reconstruction 1,72 -> 2,14 ms ; pas de plantage ; scintillement ajouté 0,25 -> 0,35 % ; traînées : écart en tournant
  1,44 %. Effet visible faible en ville ; à juger en nature.

## v104 : l'occultation suit le mouvement (pixels qui bougent calculés à chaque image)

- Retour du joueur : en bougeant, l'occultation « pétille » sur les objets, suit mal le mouvement et laisse des traits d'ombre sur eux.
- Cause : l'occultation au quart (un pixel par bloc 2 × 2 à chaque image, à tour de rôle) ; en mouvement, un pixel n'avait une valeur neuve qu'une image
  sur quatre, le reste était de l'ancien recollé par les vecteurs de mouvement. Les vecteurs du jeu n'étaient même pas lus par la passe (lus seulement
  avec `ao_boost=1`).
- Corrigé : la passe lit le mouvement de chaque pixel ; **un pixel qui bouge de plus d'un pixel par image est calculé à chaque image** (deux rayons de
  contact, un rayon long), les autres restent au quart. Débruiteur : en mouvement, la moitié de neuf (0,3 avant). `ao_motion=0` : ancien calcul.
- Mesures (Red Dead, Saint-Denis, même lancement, réglage changé à chaud) : en marchant, scintillement ajouté 0,10-0,21 % des pixels contre 0,88-1,85 %
  (une rafale à 1,79 %, la première marche, la plus agitée) ; en tournant, part de l'occultation qui ne suit pas 0,36-0,52 × l'assombrissement moyen
  contre 0,75. Passe d'occultation ~1,2 ms en mouvement (0,8 à l'arrêt).
- Horizon : le calcul à chaque image en mouvement y est coupé (`ao_motion=0`) ; mesure à l'arrêt troublée par la météo (pluie puis soleil d'un lancement
  à l'autre). Au soleil, l'occultation y scintillait sur le sol clair (0,57-2,6 %) : le soleil n'y est pas connu, elle retire toute la lumière, même au
  soleil. `ao=0.5` dans Horizon : 0,16 % (0,14 % sans occultation). `ao_rough=0` : sans effet.

## v105 : la projection de Red Dead enfin juste (38° et pas 50°), occultation stable et posée, double historique, rayons à faible discrépance

Demande : une occultation RT viable dans Red Dead, sans décalage, mesurée avec des outils précis ; vérifier si Red Dead était mieux avant Horizon.

**Outils**
- `decalage.py` (rafales avant / après) : mouvement de l'image et de la carte d'assombrissement par corrélation de phase au dixième de pixel ;
  **suivi** (1 : accrochée, 0 : figée), **décalage fixe** de la carte par rapport aux contours de l'image, **instabilité** (écart de la carte à la
  précédente recalée sur le mouvement, rapporté à l'assombrissement moyen), part assombrie. `rdr2_regress.sh` : même séance de mesure pour n'importe
  quelle version de l'add-on.

**Régressions trouvées**
1. **Champ de vision** : la v70 le mesurait (37,9°) et corrigeait la projection ; depuis, URTI travaillait à **50°** dans Red Dead. La mesure (s², t²
   des cascades) donnait toujours 37,7-37,8°, mais la correction exigeait une rotation cumulée (4e-4) jamais atteinte, et chaque étalonnage (recherche du
   soleil, v95) remettait 50°. Espace caméra étiré de 31 % : normale du sol, direction des reflets, forme de l'occultation faussées. Corrigé : seuil
   1e-4, étirements horizontal et vertical concordants à 15 %, **même mesure à 5 % près 30 images plus tard**, **caméra immobile depuis 220 images**
   (en marchant la mesure partait : 37,9° -> 60,8° -> 91,2°), pas plus de ~25 % par correction, 25° à 75° ; l'étalonnage ne remet plus 50° ;
   champ mesuré enregistré (`fov_v=` dans urti_calib.ini) et relu au lancement.
2. **Herbe** : « feuillage » (type de matériau de Red Dead) couvre aussi l'herbe : exclue en v102, parcs et nature sans occultation (0,5-1,3 % de
   l'image contre 18-27 % en v70 / v83). Seule l'herbe tournée vers le haut (normale du monde, z > 0,6) la garde ; les feuilles des arbres restent exclues.
3. **Réglages du débruiteur décalés en v104** (commentaire en milieu de ligne : bornage 0,9 au lieu de 2, flou 3×3, sans longueur d'historique).
   Corrigé, et **C4838 (conversion qui rétrécit dans une liste d'initialisation) est maintenant une erreur de compilation**.
4. **Soleil et occultation** : la part du soleil venait de la carte d'ombres du jeu, qui ne concorde pas dans Red Dead (v44), et du rapport soleil / ciel
   mesuré avec elle (0,04 en plein soleil : soleil « trop faible », coupé). Murs au soleil assombris comme à l'ombre. Corrigé : **un rayon RT vers le soleil**
   (trous des feuilles traversés) décide si le point est éclairé (`ao_sunray=1`) ; rapport soleil / ciel peu crédible (< 0,3) -> 2,2 ;
   `sun_measure=0` dans Red Dead.

**Techniques ajoutées (recherche : NRD / ReBLUR, brevets NVIDIA sur l'historique borné, bruit bleu spatio-temporel)**
- **Double historique** (`denoise_dual=1`) : historique rapide (2-3 images, au moins la moitié de neuf) rangé avec la longueur d'historique ;
  l'historique lent est borné autour de lui (fenêtre 0,5 σ + 0,04, resserrée en mouvement), plus autour du voisinage courant bruité.
- **Rayons à faible discrépance** (`ao_ign=1`) : bruit à gradient entrelacé déplacé à chaque image, rotation commune et strates en azimut, nombre d'or
  entre les rayons d'un pixel (contact et rayons longs).

**Mesures (Red Dead, rue arborée et parc de Saint-Denis, au soleil)**
- v70 / v83 / v105 : instabilité à l'arrêt 0,19 / 0,20 / **0,11** ; en tournant 0,22-0,50 / 0,43 / **0,20-0,23** ; suivi 0,90-0,91 / 0,91-0,95 /
  **0,94-0,95** ; décalage fixe 0,02 / 0,02 / **0,00-0,02 px** ; part assombrie de plus de 5 % 18-21 % / 23-27 % / 24-27 % ; scintillement ajouté à
  l'arrêt 0,39 / 0,66 / 0,40 %, en marchant 0,09-1,04 / 0,26-1,15 / 0,30-0,67 %.
- Murs au soleil : seule la part du ciel retirée ; taches de soleil au sol intactes, ombre des arbres assombrie.
- Horizon : scintillement ajouté par l'occultation 0,06 % à l'arrêt, 0,08 % en marchant ; 99 images par seconde.

## v106 : les personnages ne font plus d'ombre d'ambiance, occultation en mouvement au damier (performances)

- Retour du joueur (captures dans une ruelle de Saint-Denis) : des « dessins d'ombre » sombres et flous restent collés aux murs et suivent l'écran ; les
  ombres ajoutées coûtent trop d'images par seconde.
- Cause probable des taches : le personnage du joueur est dans la scène RT ; collé à un mur, il cache le « ciel » des rayons longs (16 m) à une grande zone
  du mur, et cette tache le suit, donc suit l'écran. Corrigé : un point touché qui, projeté à l'écran, tombe sur un personnage (alpha des normales du
  G-buffer de Red Dead) à peu près à la même profondeur est traversé, comme un trou de feuille (occultation, rayons longs, rayon vers le soleil).
- Performances : en v104, un pixel en mouvement était calculé à chaque image ; quand la caméra bouge, presque tout l'écran l'est, coût de la passe
  multiplié jusqu'à quatre. Maintenant une image sur deux, en damier (même coût par bloc 2 × 2 qu'à l'arrêt). Cible du régulateur d'occultation
  `ao_ms` 4 -> 2,5 ms.
- Installé dans Red Dead à la demande du joueur, qui teste lui-même ; pas encore mesuré par URTI. Champ de vision : pendant sa partie, mesuré 55-69° et
  corrigé à 56,1° (garde-fous respectés : caméra immobile, mesure stable) ; Red Dead change probablement de champ selon la caméra (ruelle, mode).

## v107 : reflets seuls, documentation complète, dépôt publié

- Décision du joueur : l'occultation (« le système d'ombre ») est trop instable en jeu, on garde uniquement les reflets, « déjà exceptionnels ». Case
  « Occultation » décochée par défaut dans urti.fx (le code reste, `UrtiOcclusion` caché). Le suivi du soleil reste actif dans un jeu sans caméra lue :
  ses cascades mesurent le champ de vision, dont les reflets ont besoin (Red Dead). Champ de vision stocké remis à 37,92°.
- Documentation : `README.md` réécrit (état actuel : reflets, sols brillants, une seule case, réglages par jeu, outils de mesure) et
  `docs/HOW_IT_WORKS.md` (toute la chaîne : capture, scène RT, mesures, matériaux, rayons, stabilisation, composition, occultation optionnelle, méthode
  de mesure, leçons, limites).

## v108 : Red Dead revient aux reflets nets (sans sols brillants ni terrain)

- Retour du joueur : les reflets semblent de moins bonne qualité qu'avant. Comparaison en jeu (`fov_force=` ajouté pour imposer le champ de vision) :
  le personnage changeait de lieu pendant la séance, pas de comparaison propre ; mais les rafales montrent les sols brillants (v99-v101) qui donnent à
  tout le pavage sec un reflet flou et brillant, l'air mouillé et brouillé, là où il n'y avait avant que des miroirs nets. Champ de vision 38° : plus de
  surfaces passent les tests de planéité (21,5 / 11,5 % de l'écran à reflet contre 9,8 / 7,7 % à 50°, même moment).
- Réglages de Red Dead remis à ceux des reflets d'avant : plus de `gloss_min`, `gloss`, `tess`, `gnormal_*` (copie `urti.ini.avant_v108`). Champ de vision
  mesuré gardé (angles justes). Si l'ancien rendu à 50° plaisait davantage : `fov_force=50`.
- Images du README remplacées par trois captures du joueur en v108 (salle au parquet, flaque devant la maison, ruelle aux dalles mouillées) ;
  les anciennes images avant / après (Horizon, Red Dead) sont retirées.
