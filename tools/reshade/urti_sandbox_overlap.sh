#!/bin/sh
# Bac à sable URTI : part des sauts due à la superposition des ombres et des reflets RT.
# Cinq lancements identiques (marche dans la rue mouillée) : ombres + reflets, reflets seuls, ombres
# seules, vérité terrain, natif ; puis analyse par pixel et par image (urti_sandbox_overlap.py).
#
#   tools/reshade/urti_sandbox_overlap.sh [DOSSIER] [--camera C]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DIR="$ROOT/urti/build/sandbox"
OUT=${1:-$DIR/superposition}
CAM=${URTI_OVERLAP_CAMERA:-marche}
mkdir -p "$OUT"
PRESET="$DIR/ReShadePreset.ini"
[ -f "$PRESET" ] && cp "$PRESET" "$OUT/preset.bak"
preset() { # $1 : force des ombres RT
    printf 'Techniques=URTI@urti.fx\nTechniqueSorting=URTI@urti.fx\n\n[urti.fx]\nUrtiSplit=0\nUrtiStrength=%s\n' "$1" > "$PRESET"
}
one() { # $1 : nom, $2 : force des ombres, $3 : réglages urti.ini
    preset "$2"
    echo "=== $1 ==="
    URTI_SANDBOX_WET=1 sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera "$CAM" --images 835 --ini "photo_frames=800-830;$3" > "$OUT/$1.log" 2>&1
    rm -rf "$OUT/$1" && mkdir -p "$OUT/$1" && mv "$DIR"/urti_photo_*.ppm "$OUT/$1/"
}
one les_deux 0.55 ""
one reflets 0 ""
one ombres 0.55 "reflect=0"
URTI_SANDBOX_TRUTH=1 one verite 0.55 "live=0"
one natif 0.55 "live=0"
[ -f "$OUT/preset.bak" ] && mv "$OUT/preset.bak" "$PRESET"
PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_overlap.py" "$OUT"
