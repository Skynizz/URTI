# URTI effects for ReShade

- `urti.fx`: the "URTI" checkbox in the ReShade menu (same as F7). The add-on reads it every frame.
- `urti_depth.fx`: gives the add-on access to the game's depth. It draws nothing.
- `ReShade.fxh` and `ReShadeUI.fxh` come from crosire/reshade-shaders (CC0).

Reversible install: `python -I tools/reshade/install.py --game <game folder> --dll <ReShade64.dll> --addon <urti.addon64>`
(ReShade 6.8 with add-on support, reshade.me); removal: `--remove`.

Story mode only, never online.
