#!/bin/sh
# Bac à sable URTI : lance le « mini-jeu » urti_sandbox avec ReShade et l'add-on URTI, dans son propre
# dossier (urti/build/sandbox) pour ne pas mélanger son étalonnage avec celui des autres tests.
#
#   tools/reshade/urti_sandbox.sh [--frais] [--sans-urti] [--images N] [--camera marche|fixe|orbite]
#                                 [--fg] [--sdr] [--debut S] [--vent F] [--montrer] [--ini "cle=valeur;..."]
#                                 [--photos A-B [--sortie DOSSIER] [--verite]]
#
# --frais : efface l'étalonnage stocké (urti_calib.ini) : on teste un premier lancement.
# --sans-urti : sans ReShade ni add-on (vitesse du bac à sable seul).
# --ini : réglages ajoutés à urti.ini pour ce lancement (ex. --ini "debug_view=1;shadow_rays=1").
# --photos A-B : deux lancements identiques, avec RT puis natif, photos des présentations A à B, puis
#   comparaison (effet du RT, scintillement ajouté, carte des points qui sautent) dans --sortie
#   (par défaut urti/build/sandbox/comparaison).
# --verite : le second lancement n'est pas le natif mais la vérité terrain (sol qui reflète la scène
#   exacte, même règle de mélange qu'URTI) : on mesure l'écart des reflets RT à l'image idéale et ce qui
#   saute par rapport à elle (le mouvement normal d'un reflet ne compte plus comme un saut).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/urti/build"
DIR="$BUILD/sandbox"
FRAIS=0; SANS=0; VERITE=0; INI=""; PHOTOS=""; SORTIE="$DIR/comparaison"
export URTI_FRAMES=${URTI_FRAMES:-300}
while [ $# -gt 0 ]; do
    case "$1" in
        --frais) FRAIS=1 ;;
        --sans-urti) SANS=1 ;;
        --images) shift; export URTI_FRAMES=$1 ;;
        --camera) shift; export URTI_SANDBOX_CAMERA=$1 ;;
        --fg) export URTI_SANDBOX_FG=1 ;;
        --sdr) export URTI_SANDBOX_SDR=1 ;;
        --debut) shift; export URTI_SANDBOX_START=$1 ;;
        --vent) shift; export URTI_SANDBOX_WIND=$1 ;;
        --montrer) export URTI_SHOW=1 ;;
        --ini) shift; INI=$1 ;;
        --photos) shift; PHOTOS=$1 ;;
        --sortie) shift; SORTIE=$1 ;;
        --verite) VERITE=1 ;;
        *) echo "option inconnue : $1"; exit 2 ;;
    esac
    shift
done

cmake --build "$BUILD" --config Release --target urti_sandbox urti_addon >/dev/null
mkdir -p "$DIR"
cp "$BUILD/Release/urti_sandbox.exe" "$DIR/"
if [ $SANS = 1 ]; then
    rm -f "$DIR/dxgi.dll"
else
    cp "$BUILD/Release/dxgi.dll" "$BUILD/Release/urti.addon64" "$BUILD/Release/ReShade.ini" "$DIR/"
    [ -f "$DIR/ReShadePreset.ini" ] || cp "$BUILD/Release/ReShadePreset.ini" "$DIR/"
fi
[ $FRAIS = 1 ] && rm -f "$DIR/urti_calib.ini"

# un lancement : $1 = 1 avec RT, 0 natif
run() {
    printf 'live=%s\n' "$1" > "$DIR/urti.ini"
    [ -n "$PHOTOS" ] && printf 'photo_frames=%s\n' "$PHOTOS" >> "$DIR/urti.ini"
    [ -n "$INI" ] && printf '%s\n' "$INI" | tr ';' '\n' >> "$DIR/urti.ini"
    rm -f "$DIR/urti_log.txt" "$DIR"/urti_photo_*.ppm
    START=$(date +%s)
    (cd "$DIR" && ./urti_sandbox.exe)
    echo "durée : $(( $(date +%s) - START )) s"
    [ -f "$DIR/urti_log.txt" ] && grep -v '^$' "$DIR/urti_log.txt" || true
}

if [ -z "$PHOTOS" ]; then
    run 1
    exit 0
fi
echo "=== avec RT ==="
run 1
rm -rf "$DIR/photos_rt" "$DIR/photos_natif"
mkdir -p "$DIR/photos_rt" "$DIR/photos_natif"
mv "$DIR"/urti_photo_*.ppm "$DIR/photos_rt/" 2>/dev/null || true
if [ $VERITE = 1 ]; then
    echo "=== vérité terrain ==="
    URTI_SANDBOX_TRUTH=1 run 0
else
    echo "=== natif ==="
    run 0
fi
mv "$DIR"/urti_photo_*.ppm "$DIR/photos_natif/" 2>/dev/null || true
echo "=== comparaison ==="
PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_compare.py" "$DIR/photos_rt" "$DIR/photos_natif" "$SORTIE"
