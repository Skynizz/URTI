#!/bin/sh
# URTI sandbox: do the shadows added by URTI have the darkness and the tint of the game's?
# The "game's" shadow map forgets objects (poles, posts, wall, carts, passers-by); URTI
# shadows them; the result is compared with the launch where the game shadows them itself (urti_sandbox_shadows.py).
#
#   tools/reshade/urti_sandbox_shadows.sh [FOLDER] [scene ...]
#
# Scenes: street (passers-by, fixed camera), occlusion (wall and passers-by), walk (camera moving forward).
# URTI_SHADOWS_INI="key=value;...": URTI settings (e.g. sun_measure=0: old rule).
# Dry ground and reflections off: only the shadows differ.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/urti/build/sandbox/shadows}
[ $# -gt 0 ] && shift
SCENES=${*:-"street occlusion walk"}
DIR="$ROOT/urti/build/sandbox"
INI="reflect=0;${URTI_SHADOWS_INI:-}"
mkdir -p "$OUT"
TABLE="$OUT/table.txt"
printf '%-10s %10s %22s %10s %12s\n' "scene" "recovered" "darkness (r g b)" "elsewhere" "flicker" > "$TABLE"

for scene in $SCENES; do
    echo "=== $scene ==="
    mkdir -p "$OUT/$scene"
    export URTI_SANDBOX_WET=0 URTI_SANDBOX_GAMESHADOW=partial
    sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera $scene --frames 835 --photos 800-830 --ref-shadows full --ini "$INI" \
        --out "$OUT/$scene/global" > "$OUT/$scene.log" 2>&1 || { echo "failed, see $OUT/$scene.log"; continue; }
    rm -rf "$OUT/$scene/rt" "$OUT/$scene/full" "$OUT/$scene/partial"
    mv "$DIR/photos_rt" "$OUT/$scene/rt"
    mv "$DIR/photos_native" "$OUT/$scene/full"
    export URTI_SANDBOX_GAMESHADOW=partial
    sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera $scene --frames 835 --photos 800-830 --native-only --ini "$INI" >> "$OUT/$scene.log" 2>&1 ||
        { echo "failed, see $OUT/$scene.log"; continue; }
    mv "$DIR/photos_native" "$OUT/$scene/partial"
    PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_shadows.py" "$OUT/$scene/rt" "$OUT/$scene/full" "$OUT/$scene/partial" "$OUT/$scene" > "$OUT/$scene/shadow_measurements.log"
    m="$OUT/$scene/measurements.txt"
    recovered=$(sed -n 's/^recovered[^:]*: \([-0-9.]*\) %.*/\1/p' "$m")
    darkness=$(sed -n 's/^darkness[^:]*: \(.*\)$/\1/p' "$m")
    elsewhere=$(sed -n 's/^elsewhere[^:]*: \([0-9.]*\) %.*/\1/p' "$m")
    flicker=$(sed -n 's/^flicker[^:]*: \([0-9.]*\) %.*/\1/p' "$m")
    printf '%-10s %9s%% %22s %9s%% %11s%%\n' "$scene" "$recovered" "$darkness" "$elsewhere" "$flicker" | tee -a "$TABLE"
    grep "the game's light" "$OUT/$scene.log" | tail -1 || true
done
echo
cat "$TABLE"
