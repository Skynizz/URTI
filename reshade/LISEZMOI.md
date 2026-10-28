# KRC GI pour ReShade

`KRC_GI.fx` : éclairage indirect et ombres de contact calculés à l'écran (masque de visibilité en
demi-résolution, ciel vu à l'écran en mode KRC). `ReShade.fxh` et `ReShadeUI.fxh` viennent de
crosire/reshade-shaders (CC0).

Installation réversible : `python -I tools/reshade/install.py --game <dossier du jeu> --dll <ReShade64.dll>`
(ReShade 6.8.0 sans add-ons, reshade.me) ; retrait : `--remove`. L'effet est lu dans ce dossier :
après une modification, « Recharger » dans le menu de ReShade.

En jeu : Origine ouvre le menu, F8 coupe / active l'effet, « Écran partagé » montre gauche sans,
droite avec. Mode histoire uniquement, jamais en ligne.
