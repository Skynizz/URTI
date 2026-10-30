#!/bin/sh
# Bac à sable URTI : « pop / dépop » des reflets RT. Un lancement avec la vue de diagnostic 9 (part du
# reflet RT réellement appliquée, après stabilisation), photos de 31 images consécutives, puis comptage
# des bascules (urti_sandbox_popping.py).
#
#   tools/reshade/urti_sandbox_popping.sh [--camera C] [--vent V] [--ini "cle=valeur;..."]
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/urti/build/sandbox"
CAM=marche; VENT=1; INI=""
while [ $# -gt 0 ]; do
    case "$1" in
        --camera) shift; CAM=$1 ;;
        --vent) shift; VENT=$1 ;;
        --ini) shift; INI=$1 ;;
    esac
    shift
done
OUT="$DIR/popping_$CAM"
URTI_SANDBOX_WET=1 sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera "$CAM" --vent "$VENT" --images 835 --ini "debug_view=9;photo_frames=800-830;$INI" > "$OUT.log" 2>&1
rm -rf "$OUT" && mkdir -p "$OUT" && mv "$DIR"/urti_photo_*.ppm "$OUT/"
PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_popping.py" "$OUT"
