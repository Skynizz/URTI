#!/bin/sh
# URTI sandbox: runs the urti_sandbox "small game" with ReShade and the URTI add-on, in its own
# folder (urti/build/sandbox) so that its calibration does not mix with the other tests'.
#
#   tools/reshade/urti_sandbox.sh [--fresh] [--no-urti] [--frames N] [--camera walk|fixed|orbit]
#                                 [--fg] [--sdr] [--start S] [--wind F] [--show] [--ini "key=value;..."]
#                                 [--photos A-B [--out FOLDER] [--truth] [--ref-shadows MODE] [--native-only]]
#
# --fresh: deletes the stored calibration (urti_calib.ini): tests a first launch.
# --no-urti: without ReShade or the add-on (speed of the sandbox alone).
# --ini: settings added to urti.ini for this launch (e.g. --ini "debug_view=1;shadow_rays=1").
# --photos A-B: two identical launches, with RT then native, snapshots of presents A to B, then
#   comparison (RT effect, added flicker, map of jumping points) in --out
#   (by default urti/build/sandbox/comparison).
# --truth: the second launch is not native but the ground truth (floor reflecting the exact
#   scene, same blending rule as URTI): measures how far RT reflections are from the ideal image and what
#   jumps relative to it (the normal motion of a reflection no longer counts as a jump).
# --ref-shadows full|partial: the "game's" shadows in the second launch (URTI_SANDBOX_GAMESHADOW).
# --native-only: with --photos, a single launch, native (snapshots in photos_native, no comparison).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/urti/build"
DIR="$BUILD/sandbox"
FRESH=0; NOURTI=0; TRUTH=0; INI=""; PHOTOS=""; OUT="$DIR/comparison"; REFSHADOWS=""; NATIVEONLY=0
export URTI_FRAMES=${URTI_FRAMES:-300}
while [ $# -gt 0 ]; do
    case "$1" in
        --fresh) FRESH=1 ;;
        --no-urti) NOURTI=1 ;;
        --frames) shift; export URTI_FRAMES=$1 ;;
        --camera) shift; export URTI_SANDBOX_CAMERA=$1 ;;
        --fg) export URTI_SANDBOX_FG=1 ;;
        --sdr) export URTI_SANDBOX_SDR=1 ;;
        --start) shift; export URTI_SANDBOX_START=$1 ;;
        --wind) shift; export URTI_SANDBOX_WIND=$1 ;;
        --show) export URTI_SHOW=1 ;;
        --ini) shift; INI=$1 ;;
        --photos) shift; PHOTOS=$1 ;;
        --out) shift; OUT=$1 ;;
        --truth) TRUTH=1 ;;
        --ref-shadows) shift; REFSHADOWS=$1 ;;
        --native-only) NATIVEONLY=1 ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
    shift
done

cmake --build "$BUILD" --config Release --target urti_sandbox urti_addon >/dev/null
mkdir -p "$DIR"
cp "$BUILD/Release/urti_sandbox.exe" "$DIR/"
if [ $NOURTI = 1 ]; then
    rm -f "$DIR/dxgi.dll"
else
    cp "$BUILD/Release/dxgi.dll" "$BUILD/Release/urti.addon64" "$BUILD/Release/ReShade.ini" "$DIR/"
    [ -f "$DIR/ReShadePreset.ini" ] || cp "$BUILD/Release/ReShadePreset.ini" "$DIR/"
fi
[ $FRESH = 1 ] && rm -f "$DIR/urti_calib.ini"

# one launch: $1 = 1 with RT, 0 native
run() {
    printf 'live=%s\n' "$1" > "$DIR/urti.ini"
    [ -n "$PHOTOS" ] && printf 'photo_frames=%s\n' "$PHOTOS" >> "$DIR/urti.ini"
    [ -n "$INI" ] && printf '%s\n' "$INI" | tr ';' '\n' >> "$DIR/urti.ini"
    rm -f "$DIR/urti_log.txt" "$DIR"/urti_photo_*.ppm
    START=$(date +%s)
    (cd "$DIR" && ./urti_sandbox.exe)
    echo "duration: $(( $(date +%s) - START )) s"
    [ -f "$DIR/urti_log.txt" ] && grep -v '^$' "$DIR/urti_log.txt" || true
}

if [ -z "$PHOTOS" ]; then
    run 1
    exit 0
fi
rm -rf "$DIR/photos_rt" "$DIR/photos_native"
mkdir -p "$DIR/photos_rt" "$DIR/photos_native"
if [ $NATIVEONLY = 1 ]; then
    echo "=== native ==="
    run 0
    mv "$DIR"/urti_photo_*.ppm "$DIR/photos_native/" 2>/dev/null || true
    exit 0
fi
echo "=== with RT ==="
run 1
mv "$DIR"/urti_photo_*.ppm "$DIR/photos_rt/" 2>/dev/null || true
[ -n "$REFSHADOWS" ] && export URTI_SANDBOX_GAMESHADOW=$REFSHADOWS
if [ $TRUTH = 1 ]; then
    echo "=== ground truth ==="
    URTI_SANDBOX_TRUTH=1 run 0
else
    echo "=== native ==="
    run 0
fi
mv "$DIR"/urti_photo_*.ppm "$DIR/photos_native/" 2>/dev/null || true
echo "=== comparison ==="
PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_compare.py" "$DIR/photos_rt" "$DIR/photos_native" "$OUT"
