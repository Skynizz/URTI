#!/bin/sh
# Essai automatique d'URTI dans Horizon Forbidden West pendant une pause de l'utilisateur : attend que le PC ne soit
# plus touché depuis 3 minutes, lance le jeu (sauvegarde du feu de camp du marais), tourne la caméra vers l'eau, prend
# des rafales (image normale, poids des reflets, occultation brute, occultation renforcée), ferme le jeu. Dès que
# l'utilisateur revient (clavier ou souris touchés), le jeu est fermé et l'essai s'arrête.
#
#   tools/reshade/urti_hfw_test.sh <dossier de sortie> [attente maximale en minutes, 240 par défaut]
set -u
OUT=$1
WAIT_MIN=${2:-240}
HERE=$(cd "$(dirname "$0")" && pwd)
H="/f/SteamLibrary/steamapps/common/Horizon Forbidden West Complete Edition"
mkdir -p "$OUT"
LOG="$OUT/essai.txt"
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; }
idle() { powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/inactif.ps1" 2>/dev/null | tr -d '\r'; }
pause_s() { n=0; while [ $n -lt "$1" ]; do n=$((n + 1)); timeout 1 cat /dev/zero >/dev/null 2>&1; done; }
close_game() {
    taskkill //IM HorizonForbiddenWest.exe >/dev/null 2>&1
    n=0; while tasklist //NH | grep -qi horizonforbidden && [ $n -lt 20 ]; do n=$((n + 1)); pause_s 1; done
    tasklist //NH | grep -qi horizonforbidden && taskkill //F //IM HorizonForbiddenWest.exe >/dev/null 2>&1
    [ -f "$OUT/urti.ini.base" ] && cp "$OUT/urti.ini.base" "$H/urti.ini"
}
# l'utilisateur est revenu : on rend la main tout de suite
back() { [ "$(idle)" -lt 20 ]; }
abort_if_back() { if back; then say "utilisateur revenu : jeu fermé, essai arrêté"; close_game; exit 2; fi; }

say "attente d'une pause (3 min sans clavier ni souris, $WAIT_MIN min au plus)"
t=0
while [ "$(idle)" -lt 180 ]; do
    t=$((t + 15)); pause_s 15
    [ $t -ge $((WAIT_MIN * 60)) ] && { say "pas de pause"; exit 1; }
done
say "pause : lancement"
cp "$H/urti.ini" "$OUT/urti.ini.base"
rm -rf -- "${H:?}"/urti_rafale_*
powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/urti_hfw_rapide.ps1" -TimeoutMin 6 >> "$LOG" 2>&1
abort_if_back
pause_s 20
abort_if_back
L=$(grep -n "appareil qui présente choisi" "$H/urti_log.txt" | tail -1 | cut -d: -f1)
# vers le marais (+800 : un tiers de tour depuis le feu de camp)
powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/mousemove.ps1" -Dx 20 -Dy 0 -Steps 40 -Delay 15 >/dev/null
pause_s 10
abort_if_back

# rafale avec un réglage ajouté le temps de la prise : nom, lignes urti.ini
burst() {
    name=$1; extra=$2
    { cat "$OUT/urti.ini.base"; printf '%b' "$extra"; } > "$H/urti.ini"
    pause_s 8
    abort_if_back
    c0=$(grep -c "rafale .* terminée" "$H/urti_log.txt")
    touch "$H/urti_rafale"
    n=0; while [ "$(grep -c "rafale .* terminée" "$H/urti_log.txt")" -le "$c0" ] && [ $n -lt 60 ]; do n=$((n + 1)); pause_s 1; done
    D=$(ls -td "$H"/urti_rafale_* | head -1)
    rm -rf -- "${OUT:?}/$name"; cp -r "$D" "$OUT/$name"
    say "rafale $name prise"
}
burst normal ""
burst poids "debug_view=5\n"
burst occultation "debug_view=1\n"
burst occultation_forte "debug_view=1\nao=1\nao_radius=2.5\n"
burst normal_forte "ao=1\nao_radius=2.5\n"
burst occultation_herbe "debug_view=1\nao_rough=1\n"
burst normal_herbe "ao_rough=1\n"
cp "$OUT/urti.ini.base" "$H/urti.ini"
pause_s 3
tail -n +"$L" "$H/urti_log.txt" > "$OUT/journal.txt"
close_game
rm -rf -- "${H:?}"/urti_rafale_*
say "essai terminé"
