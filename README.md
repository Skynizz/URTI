# KRC — Knowledge Radiance Cache

Prototype de recherche : **rendu incrémental piloté par l'incertitude**. Au lieu de
recalculer chaque image, on entretient un cache spatial de radiance où chaque
cellule connaît sa propre incertitude. Les rayons vont là où l'on doute, et un
changement de scène n'invalide que les cellules qui en dépendent *réellement*,
ce que vérifie un test statistique.

- Hypothèse, critique de l'idée initiale et protocole : [docs/HYPOTHESE.md](docs/HYPOTHESE.md)
- Journal de bord : [docs/JOURNAL.md](docs/JOURNAL.md)
- Résultats : [docs/RESULTATS.md](docs/RESULTATS.md)

## Lancer

```sh
cargo run --release -- all      # références + toutes les méthodes
cargo run --release -- refs     # références seules (mises en cache dans results/ref)
cargo run --release -- run krc pt_reset   # méthodes choisies
python tools/plots.py           # graphiques SVG dans results/
```

Path tracer CPU en Rust, sans dépendance externe, multi-thread.
