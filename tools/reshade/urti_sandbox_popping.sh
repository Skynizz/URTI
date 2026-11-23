#!/bin/sh
# URTI sandbox: "popping" of RT reflections. One launch with diagnostic view 9 (share of the
# RT reflection actually applied, after stabilisation), snapshots of 31 consecutive frames, then a count
# of the toggles (urti_sandbox_popping.py).
#
#   tools/reshade/urti_sandbox_popping.sh [--camera C] [--wind W] [--ini "key=value;..."]
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/urti/build/sandbox"
CAM=walk; WIND=1; INI=""
while [ $# -gt 0 ]; do
    case "$1" in
        --camera) shift; CAM=$1 ;;
        --wind) shift; WIND=$1 ;;
        --ini) shift; INI=$1 ;;
    esac
    shift
done
OUT="$DIR/popping_$CAM"
URTI_SANDBOX_WET=1 sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera "$CAM" --wind "$WIND" --frames 835 --ini "debug_view=9;photo_frames=800-830;$INI" > "$OUT.log" 2>&1
rm -rf "$OUT" && mkdir -p "$OUT" && mv "$DIR"/urti_photo_*.ppm "$OUT/"
PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_popping.py" "$OUT"
