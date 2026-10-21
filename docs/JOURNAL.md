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
