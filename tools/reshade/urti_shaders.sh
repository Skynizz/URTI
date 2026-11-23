#!/bin/sh
# Recompiles URTI's live-mode shaders into <game folder>/urti_shaders: the add-on hot-reloads them
# (in under a second), without restarting the game. Usage: urti_shaders.sh "F:/Red Dead Redemption 2"
set -e
DEST="${1:?game folder}"
DXC="${URTI_DXC:-F:/rtxgi/sdk/build/_deps/dxc-src/bin/x64/dxc.exe}"
SRC="$(dirname "$0")/../../urti/addon"
mkdir -p "$DEST/urti_shaders"
"$DXC" -nologo -T cs_6_5 -E main -Fo "$DEST/urti_shaders/live_shadow.cso" "$SRC/live_shadow.hlsl"
"$DXC" -nologo -T cs_6_0 -E main -Fo "$DEST/urti_shaders/live_composite.cso" "$SRC/live_composite.hlsl"
"$DXC" -nologo -T cs_6_5 -E main -Fo "$DEST/urti_shaders/live_reflect.cso" "$SRC/live_reflect.hlsl"
echo "shaders compiled into $DEST/urti_shaders"
