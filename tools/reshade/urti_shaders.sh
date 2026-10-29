#!/bin/sh
# Recompile les shaders du mode direct d'URTI dans <dossier du jeu>/urti_shaders : l'add-on les recharge
# à chaud (en moins d'une seconde), sans relancer le jeu. Usage : urti_shaders.sh "F:/Red Dead Redemption 2"
set -e
DEST="${1:?dossier du jeu}"
DXC="${URTI_DXC:-F:/rtxgi/sdk/build/_deps/dxc-src/bin/x64/dxc.exe}"
SRC="$(dirname "$0")/../../urti/addon"
mkdir -p "$DEST/urti_shaders"
"$DXC" -nologo -T cs_6_5 -E main -Fo "$DEST/urti_shaders/live_shadow.cso" "$SRC/live_shadow.hlsl"
"$DXC" -nologo -T cs_6_0 -E main -Fo "$DEST/urti_shaders/live_composite.cso" "$SRC/live_composite.hlsl"
echo "shaders compilés dans $DEST/urti_shaders"
