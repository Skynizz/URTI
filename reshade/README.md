# KRC GI for ReShade

`KRC_GI.fx`: screen-space indirect lighting and contact shadows (half-resolution visibility mask, sky seen on
screen in KRC mode). `ReShade.fxh` and `ReShadeUI.fxh` come from crosire/reshade-shaders (CC0).

Reversible install: `python -I tools/reshade/install.py --game <game folder> --dll <ReShade64.dll>`
(ReShade 6.8.0 without add-ons, reshade.me); removal: `--remove`. The effect is read from this folder: after a
change, use "Reload" in the ReShade menu.

In game: Home opens the menu, F8 toggles the effect, "Split screen" shows left without, right with. Story mode
only, never online.
