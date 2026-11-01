#!/bin/sh
# Bac à sable URTI : les ombres ajoutées par URTI ont-elles la noirceur et la teinte de celles du jeu ?
# La carte d'ombres du « jeu » oublie des objets (poteaux, piquets, mur, charrettes, passants) ; URTI les
# ombre ; on compare au lancement où le jeu les ombre lui-même (urti_sandbox_ombres.py).
#
#   tools/reshade/urti_sandbox_ombres.sh [DOSSIER] [scène ...]
#
# Scènes : rue (passants, caméra fixe), occlusion (mur et passants), marche (caméra qui avance).
# URTI_OMBRES_INI="cle=valeur;..." : réglages d'URTI (ex. sun_measure=0 : ancienne règle).
# Sol sec et reflets coupés : seules les ombres diffèrent.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/urti/build/sandbox/ombres}
[ $# -gt 0 ] && shift
SCENES=${*:-"rue occlusion marche"}
DIR="$ROOT/urti/build/sandbox"
INI="reflect=0;${URTI_OMBRES_INI:-}"
mkdir -p "$OUT"
TABLE="$OUT/tableau.txt"
printf '%-10s %10s %22s %10s %12s\n' "scène" "rattrapé" "noirceur (r v b)" "ailleurs" "scintillement" > "$TABLE"

for scene in $SCENES; do
    echo "=== $scene ==="
    mkdir -p "$OUT/$scene"
    export URTI_SANDBOX_WET=0 URTI_SANDBOX_GAMESHADOW=partiel
    sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera $scene --images 835 --photos 800-830 --ref-ombres complet --ini "$INI" \
        --sortie "$OUT/$scene/global" > "$OUT/$scene.log" 2>&1 || { echo "échec, voir $OUT/$scene.log"; continue; }
    rm -rf "$OUT/$scene/rt" "$OUT/$scene/complet" "$OUT/$scene/partiel"
    mv "$DIR/photos_rt" "$OUT/$scene/rt"
    mv "$DIR/photos_natif" "$OUT/$scene/complet"
    export URTI_SANDBOX_GAMESHADOW=partiel
    sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera $scene --images 835 --photos 800-830 --natif-seul --ini "$INI" >> "$OUT/$scene.log" 2>&1 ||
        { echo "échec, voir $OUT/$scene.log"; continue; }
    mv "$DIR/photos_natif" "$OUT/$scene/partiel"
    PYTHONIOENCODING=utf-8 python "$ROOT/tools/reshade/urti_sandbox_ombres.py" "$OUT/$scene/rt" "$OUT/$scene/complet" "$OUT/$scene/partiel" "$OUT/$scene" > "$OUT/$scene/mesures_ombres.log"
    m="$OUT/$scene/mesures.txt"
    rattrape=$(sed -n 's/^rattrapé[^:]*: \([-0-9.]*\) %.*/\1/p' "$m")
    noirceur=$(sed -n 's/^noirceur[^:]*: \(.*\)$/\1/p' "$m")
    ailleurs=$(sed -n 's/^ailleurs[^:]*: \([0-9.]*\) %.*/\1/p' "$m")
    scint=$(sed -n 's/^scintillement[^:]*: \([0-9.]*\) %.*/\1/p' "$m")
    printf '%-10s %9s%% %22s %9s%% %11s%%\n' "$scene" "$rattrape" "$noirceur" "$ailleurs" "$scint" | tee -a "$TABLE"
    grep "lumière du jeu" "$OUT/$scene.log" | tail -1 || true
done
echo
cat "$TABLE"
