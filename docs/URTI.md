# URTI : plan et avis de faisabilité (octobre 2026)

Idée de Skynizz : une couche intermédiaire qui observe un jeu DirectX 12 sans ray tracing, fabrique
une représentation géométrique parallèle et persistante (cache géométrique temporel, classes
statique / rigide / déformé / inconnu, niveaux de confiance), et ajoute ombres, reflets puis
éclairage indirect en RT, composés avec l'image d'origine.

Avis : faisable en prototype sur un jeu, par étapes avec critères d'arrêt ; « universel et
robuste » n'est pas réaliste sans un petit profil par jeu (comme RTX Remix). Risques, du plus
grave au moins grave :
1. La géométrie en espace monde : ne pas décoder les buffers mais capter la sortie des vertex
   shaders (sortie de flux) ; reste à retrouver la caméra. Les mesh shaders y échappent.
2. La composition : on ne peut pas retirer de l'image finale les ombres et reflets du jeu ; sans
   intervenir dans sa passe d'éclairage, le RT ne peut qu'ajouter.
3. Les matériaux hors champ : plutôt que les décoder, garder la lumière des surfaces vues dans un
   cache en espace monde (KRC) et la relire aux points touchés par les rayons.
4. Les lampes : le soleil à partir des passes de cartes d'ombres ; les autres plus tard.
5. Anti-triche : solo uniquement.

Étapes : 0 banc à vérité terrain ; 1 capture et couverture (> 95 % de la profondeur du jeu
expliquée, sinon arrêt) ; 2 ombres RT du soleil ; 3 cache de lumière, reflets, indirect.
Suivi : urti/README.md.
