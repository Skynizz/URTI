#!/bin/sh
# Suite de tests du bac à sable URTI : sept scènes, chacune lancée avec RT puis en référence, mesurées
# et résumées dans un tableau.
#
#   tools/reshade/urti_sandbox_suite.sh [DOSSIER] [scène ...]
#
# DOSSIER : où ranger les résultats (défaut urti/build/sandbox/suite) ; scènes : un sous-ensemble.
#   batiment          bâtiment immobile, caméra fixe              stabilité spatiale
#   arbre_immobile    arbre sans vent, caméra fixe                géométrie fine
#   arbre_vent        arbre au vent, caméra fixe                  déformations
#   personnage        passants qui traversent, caméra fixe        objets mobiles
#   camera_fixe       toute la rue (vent, passants), caméra fixe  scintillement
#   camera_mouvement  marche dans la rue, demi-tours              reprojection
#   occlusion         passants qui passent derrière un mur        occultations
#
# Référence : la vérité terrain (sol qui reflète la scène exacte) ; URTI_SUITE_NATIF=1 : le natif.
# URTI_SUITE_INI="cle=valeur;..." : réglages d'URTI pour toute la suite.
# Colonnes : écart moyen à la référence, scintillement ajouté, pixels qui sautent, pixels qui clignotent
# (l'écart à la référence change de sens d'une image à la suivante).
set -e
REF=--verite
[ "${URTI_SUITE_NATIF:-0}" = 1 ] && REF=
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/urti/build/sandbox/suite}
[ $# -gt 0 ] && shift
SCENES=${*:-"batiment arbre_immobile arbre_vent personnage camera_fixe camera_mouvement occlusion"}
mkdir -p "$OUT"
TABLE="$OUT/tableau.txt"
printf '%-18s %8s %10s %10s %10s\n' "scène" "écart" "ajouté" "sautent" "clignotent" > "$TABLE"

for scene in $SCENES; do
    case $scene in
        batiment) cam=batiment; vent=0; mob=0 ;;
        arbre_immobile) cam=arbre; vent=0; mob=0 ;;
        arbre_vent) cam=arbre; vent=1; mob=0 ;;
        personnage) cam=rue; vent=0; mob=1 ;;
        camera_fixe) cam=fixe; vent=1; mob=1 ;;
        camera_mouvement) cam=marche; vent=1; mob=1 ;;
        occlusion) cam=occlusion; vent=0; mob=1 ;;
        *) echo "scène inconnue : $scene"; exit 2 ;;
    esac
    echo "=== $scene ==="
    URTI_SANDBOX_WET=1 URTI_SANDBOX_MOVERS=$mob sh "$ROOT/tools/reshade/urti_sandbox.sh" --camera $cam --vent $vent --images 835 --photos 800-830 $REF \
        --ini "${URTI_SUITE_INI:-}" --sortie "$OUT/$scene" > "$OUT/$scene.log" 2>&1 || { echo "échec, voir $OUT/$scene.log"; continue; }
    m="$OUT/$scene/mesures.txt"
    effet=$(sed -n 's/.*(écart moyen RT \/ référence) : \([0-9.]*\) %.*/\1/p' "$m")
    ajoute=$(sed -n 's/.*ajouté \([-+0-9.]*\) %.*/\1/p' "$m")
    sautent=$(sed -n 's/.*plus de 8 %) : \([0-9.]*\) %.*/\1/p' "$m")
    clignotent=$(sed -n 's/.*plus de 4 %) : \([0-9.]*\) %.*/\1/p' "$m")
    printf '%-18s %7s%% %9s%% %9s%% %9s%%\n' "$scene" "$effet" "$ajoute" "$sautent" "$clignotent" | tee -a "$TABLE"
done
echo
cat "$TABLE"
