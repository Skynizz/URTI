#!/bin/sh
# Essai automatique d'URTI dans Horizon Forbidden West pendant une pause de l'utilisateur : attend que le PC ne soit
# plus touché depuis 3 minutes, lance le jeu (sauvegarde du feu de camp du marais), tourne la caméra vers l'eau, prend
# des rafales (image normale, poids des reflets, occultation brute, occultation renforcée), ferme le jeu. Dès que
# l'utilisateur revient (clavier ou souris touchés), le jeu est fermé et l'essai s'arrête.
# URTI_FORCE=1 : tout de suite, même si quelqu'un se sert du PC (accord de l'utilisateur), sans s'arrêter.
# URTI_BURSTS : liste de rafales à la place de la liste habituelle, une par ligne : nom|réglages urti.ini (\n entre deux) ;
# un nom qui commence par « tourne » : prise pendant que la caméra tourne ; par « ecran » : simple capture de la fenêtre,
# qui ne ralentit pas le jeu (une rafale le gèle : ses images ne sont pas au rythme réel).
# URTI_PASSES=1 : à la fin, relevé des passes (toutes les images de rendu d'une image du jeu) dans <dossier>/passes.
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
FORCE=${URTI_FORCE:-0}
back() { [ "$FORCE" = 0 ] && [ "$(idle)" -lt 20 ]; }
abort_if_back() { if back; then say "utilisateur revenu : jeu fermé, essai arrêté"; close_game; exit 2; fi; }

say "attente d'une pause (3 min sans clavier ni souris, $WAIT_MIN min au plus)"
t=0
while [ "$FORCE" = 0 ] && [ "$(idle)" -lt 180 ]; do
    t=$((t + 15)); pause_s 15
    [ $t -ge $((WAIT_MIN * 60)) ] && { say "pas de pause"; exit 1; }
done
say "pause : lancement"
# réglages de départ, sans ce qu'un essai interrompu aurait laissé ; toujours remis à la fin, même si l'essai est coupé
grep -avE '^(debug_view|motion_log|trace_draw|ao|ao_radius|ao_rough|reflect_f0|geo|queue_sync)=' "$H/urti.ini" > "$OUT/urti.ini.base"
trap 'cp "$OUT/urti.ini.base" "$H/urti.ini"' EXIT
rm -rf -- "${H:?}"/urti_rafale_*
powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/urti_hfw_rapide.ps1" -TimeoutMin 6 $([ "$FORCE" = 1 ] && echo -Force) >> "$LOG" 2>&1
launched=$?
if [ "$launched" != 0 ]; then
    say "pas de partie (code $launched) : essai arrêté"
    tail -1 "$LOG"
    exit "$launched"
fi
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
    case "$name" in ecran*)
        powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/capture_fenetre.ps1" -Process HorizonForbiddenWest -Out "$(cygpath -w "$OUT/$name.png")" >/dev/null
        say "capture $name prise"
        return ;;
    esac
    c0=$(grep -c "rafale .* terminée" "$H/urti_log.txt")
    case "$name" in tourne*) powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/mousemove.ps1" -Dx 4 -Dy 0 -Steps 150 -Delay 16 >/dev/null & ;; esac
    touch "$H/urti_rafale"
    n=0; while [ "$(grep -c "rafale .* terminée" "$H/urti_log.txt")" -le "$c0" ] && [ $n -lt 60 ]; do n=$((n + 1)); pause_s 1; done
    D=$(ls -td "$H"/urti_rafale_* | head -1)
    rm -rf -- "${OUT:?}/$name"; cp -r "$D" "$OUT/$name"
    say "rafale $name prise"
}
if [ -n "${URTI_BURSTS:-}" ]; then
    while IFS='|' read -r bn be; do [ -n "$bn" ] && burst "$bn" "$be"; done <<EOF
$URTI_BURSTS
EOF
else
    burst normal ""
    burst poids "debug_view=5\n"
    burst occultation "debug_view=1\n"
    burst occultation_forte "debug_view=1\nao=1\nao_radius=2.5\n"
    burst normal_forte "ao=1\nao_radius=2.5\n"
    burst occultation_herbe "debug_view=1\nao_rough=1\n"
    burst normal_herbe "ao_rough=1\n"
fi
cp "$OUT/urti.ini.base" "$H/urti.ini"
pause_s 3
if [ -n "${URTI_PASSES:-}" ]; then
    c0=$(grep -c "des passes fini" "$H/urti_log.txt")
    touch "$H/urti_passes"
    n=0; while [ "$(grep -c "des passes fini" "$H/urti_log.txt")" -le "$c0" ] && [ $n -lt 90 ]; do n=$((n + 1)); pause_s 1; done
    mkdir -p "$OUT/passes"
    mv "$H"/urti_passe_*.ppm "$H"/urti_instant_*.ppm "$OUT/passes/" 2>/dev/null
    say "relevé des passes pris : $(ls "$OUT/passes" | wc -l) images"
fi
tail -n +"$L" "$H/urti_log.txt" > "$OUT/journal.txt"
close_game
rm -rf -- "${H:?}"/urti_rafale_*
say "essai terminé"
