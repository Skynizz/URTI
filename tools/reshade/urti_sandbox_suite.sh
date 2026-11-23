#!/bin/sh
# URTI sandbox test suite: seven scenes, each launched with RT then as a reference, measured
# and summed up in a table.
#
#   tools/reshade/urti_sandbox_suite.sh [FOLDER] [scene ...]
#
# FOLDER: where to store the results (default urti/build/sandbox/suite); scenes: a subset.
#   building          still building, fixed camera                spatial stability
#   still_tree        tree without wind, fixed camera             thin geometry
#   tree_wind         tree in the wind, fixed camera              deformations
#   character         passers-by crossing, fixed camera           moving objects
#   fixed_camera      the whole street (wind, passers-by), fixed  flicker
#   moving_camera     walking down the street, U-turns            reprojection
#   occlusion         passers-by walking behind a wall            occlusions
#
# Reference: the ground truth (floor reflecting the exact scene); URTI_SUITE_NATIVE=1: the native launch.
# URTI_SUITE_INI="key=value;...": URTI settings for the whole suite.
# Columns: mean difference to the reference, added flicker, jumping pixels, blinking pixels
# (the difference to the reference changes sign from one frame to the next).
set -e
REF=--truth
[ "${URTI_SUITE_NATIVE:-0}" = 1 ] && REF=
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/urti/build/sandbox/suite}
[ $# -gt 0 ] && shift
SCENES=${*:-"building still_tree tree_wind character fixed_camera moving_camera occlusion"}
mkdir -p "$OUT"
TABLE="$OUT/table.txt"
printf '%-18s %8s %10s %10s %10s\n' "scene" "deviation" "added" "jumping" "blinking" > "$TABLE"

for scene in $SCENES; do
    case $scene in
        building) cam=building; wind=0; mob=0 ;;
        still_tree) cam=tree; wind=0; mob=0 ;;
        tree_wind) cam=tree; wind=1; mob=0 ;;
        character) cam=street; wind=0; mob=1 ;;
        fixed_camera) cam=fixed; wind=1; mob=1 ;;
        moving_camera) cam=walk; wind=1; mob=1 ;;
        occlusion) cam=occlusion; wind=0; mob=1 ;;
        *) echo "unknown scene: $scene"; exit 2 ;;
    esac
    echo "=== $scene ==="
    URTI_SANDBOX_WET=1 URTI_SANDBOX_MOVERS=$mob sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera $cam --wind $wind --frames 835 --photos 800-830 $REF \
        --ini "${URTI_SUITE_INI:-}" --out "$OUT/$scene" > "$OUT/$scene.log" 2>&1 || { echo "failed, see $OUT/$scene.log"; continue; }
    m="$OUT/$scene/measurements.txt"
    effect=$(sed -n 's/.*(mean difference RT \/ reference): \([0-9.]*\) %.*/\1/p' "$m")
    added=$(sed -n 's/.*added \([-+0-9.]*\) %.*/\1/p' "$m")
    jumping=$(sed -n 's/.*more than 8 %): \([0-9.]*\) %.*/\1/p' "$m")
    blinking=$(sed -n 's/.*more than 4 %): \([0-9.]*\) %.*/\1/p' "$m")
    printf '%-18s %7s%% %9s%% %9s%% %9s%%\n' "$scene" "$effect" "$added" "$jumping" "$blinking" | tee -a "$TABLE"
done
echo
cat "$TABLE"
