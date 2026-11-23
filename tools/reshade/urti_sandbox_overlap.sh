#!/bin/sh
# URTI sandbox: share of the jumps due to RT shadows and reflections overlapping.
# Five identical launches (walking down the wet street): shadows + reflections, reflections alone, shadows
# alone, ground truth, native; then analysis per pixel and per frame (urti_sandbox_overlap.py).
#
#   tools/reshade/urti_sandbox_overlap.sh [FOLDER] [--camera C]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/urti/build/sandbox"
OUT=${1:-$DIR/overlap}
CAM=${URTI_OVERLAP_CAMERA:-walk}
mkdir -p "$OUT"
PRESET="$DIR/ReShadePreset.ini"
[ -f "$PRESET" ] && cp "$PRESET" "$OUT/preset.bak"
preset() { # $1: RT shadow strength
    printf 'Techniques=URTI@urti.fx\nTechniqueSorting=URTI@urti.fx\n\n[urti.fx]\nUrtiSplit=0\nUrtiStrength=%s\n' "$1" > "$PRESET"
}
one() { # $1: name, $2: shadow strength, $3: urti.ini settings
    preset "$2"
    echo "=== $1 ==="
    URTI_SANDBOX_WET=1 sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera "$CAM" --frames 835 --ini "photo_frames=800-830;$3" > "$OUT/$1.log" 2>&1
    rm -rf "$OUT/$1" && mkdir -p "$OUT/$1" && mv "$DIR"/urti_photo_*.ppm "$OUT/$1/"
}
one both 0.55 ""
one reflections 0 ""
one shadows 0.55 "reflect=0"
URTI_SANDBOX_TRUTH=1 one truth 0.55 "live=0"
one native 0.55 "live=0"
[ -f "$OUT/preset.bak" ] && mv "$OUT/preset.bak" "$PRESET"
PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_overlap.py" "$OUT"
