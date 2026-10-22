# KRC — Knowledge Radiance Cache

Prototype de recherche : **rendu incrémental piloté par l'incertitude**. Au lieu de
recalculer chaque image, on entretient un cache spatial de radiance où chaque
cellule connaît sa propre incertitude. Les rayons vont là où l'on doute. Quand la scène
change, un graphe de dépendances spatiales désigne les cellules suspectes, et des
**chemins corrélés** (même graine, scène d'avant et d'après) *mesurent* leur
changement pour corriger l'estimation au lieu de la jeter.

Résultat principal (voir [docs/RESULTATS.md](docs/RESULTATS.md)) : à budget égal,
l'erreur est 3× plus faible qu'un cache avec reset et 21× plus faible que le path
tracing accumulé sur une séquence dynamique. Une scène statique converge vers ~0,01
chemin/pixel/frame, et le coût d'un objet qui bouge ne croît pas avec la taille de la
scène. Sur de vrais triangles (Cornell box, Sponza) avec ray tracing matériel, comparé au vrai
SHaRC de NVIDIA (portage WGSL validé) : KRC v2 a 2 à 4× moins d'erreur à temps GPU égal
sur les séquences dynamiques de Sponza (caméra fixe ou mobile) et 20× sur la Cornell box ;
SHaRC reste meilleur en statique avec caméra mobile en haute résolution, et son coût
minimal est plus bas. Voir RESULTATS §12–13.

- Hypothèse, critique de l'idée initiale et protocole : [docs/HYPOTHESE.md](docs/HYPOTHESE.md)
- Journal de bord : [docs/JOURNAL.md](docs/JOURNAL.md)
- Résultats : [docs/RESULTATS.md](docs/RESULTATS.md)
- Vers les jeux : [docs/FEUILLE_DE_ROUTE.md](docs/FEUILLE_DE_ROUTE.md)

## Lancer

```sh
cargo run --release -- all      # références + toutes les méthodes
cargo run --release -- refs     # références seules (mises en cache dans results/ref)
cargo run --release -- run krc pt_reset   # méthodes choisies
cargo run --release -- scale    # passage à l'échelle : 2, 4, 8 pièces (KRC_SEEDS, KRC_WARMUP)
cargo run --release -- bench    # validation GPU vs CPU et débit
KRC_GPU=1 cargo run --release -- all   # même chose, chemins tracés sur GPU (results/gpu)
KRC_GPU=1 cargo run --release -- run gpu_krc gpu_sharc   # pipeline entièrement GPU (boîtes)
cargo run --release -- cornell 256      # Cornell box en triangles, ray query (KRC_SEEDS)
sh tools/fetch_sponza.sh && cargo run --release -- sponza 480   # Sponza
cargo run --release -- pareto [1024 | sponza 1280]   # erreur contre temps GPU
cargo run --release -- bias [cornell]   # luminance image / référence (biais)
cargo run --release -- sponzacam 480    # caméra mobile (aussi cornellcam), KRC et SHaRC officiel
cargo run --release -- sharccheck       # validation du portage SHaRC
cargo run --release -- passes sponzacam # temps GPU par passe
cargo run --release -- duel sponzacam   # test court KRC contre SHaRC (KRC_DUEL filtre)
cargo run --release -- statique sponza  # convergence en scène statique
python tools/plots.py           # graphiques SVG dans results/
```

Path tracer en Rust : CPU multi-thread ou GPU (compute WGSL via wgpu/Vulkan).
