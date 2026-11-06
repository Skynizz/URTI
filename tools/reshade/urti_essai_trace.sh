#!/bin/sh
# Essai à vitesse réelle : trace par présentation (immobile, tourne, marche), pause (rafale), vue d'alignement pendant que la caméra tourne.
# Rien n'est envoyé si le jeu n'est pas au premier plan.

H="/f/SteamLibrary/steamapps/common/Horizon Forbidden West Complete Edition"
T=/f/NewrAYTRACING/tools/reshade
O="$1"; EXTRA="${2:-}" # dossier de sortie, réglages urti.ini en plus
mkdir -p "$O"
# jeu déjà ouvert : essai annulé, il n'est pas fermé
if tasklist //NH | grep -qi horizonforbidden; then
    echo "JEU DEJA OUVERT : essai annule, rien n'est touche"; exit 1
fi
[ -f "$H/urti.addon64.v76" ] || cp "$H/urti.addon64" "$H/urti.addon64.v76"
cp /f/NewrAYTRACING/urti/build/Release/urti.addon64 "$H/urti.addon64"
grep -avE '^(motion_log|camera|frame_trace|debug_view)=' "$H/urti.ini" > "$O/ini_base"
trap 'cp "$O/ini_base" "$H/urti.ini"; rm -f "$H/urti_rafale"' EXIT
{ cat "$O/ini_base"; printf 'frame_trace=1\n%b' "$EXTRA"; } > "$H/urti.ini"
tail -c 1000000 "$H/urti_log.txt" > "$O/x"; cp "$O/x" "$H/urti_log.txt"; rm "$O/x"
L0=$(wc -l < "$H/urti_log.txt")
powershell -NoProfile -ExecutionPolicy Bypass -File "$T/urti_hfw_rapide.ps1" -TimeoutMin 4 -Force > "$O/lancement.txt" 2>&1; rc=$?
tail -1 "$O/lancement.txt" | cut -c1-160
[ "$rc" = 0 ] || { echo "PAS DE PARTIE (code $rc) : essai arrete, rien n'est envoye"; tail -n +$L0 "$H/urti_log.txt" > "$O/journal.txt"; exit 1; }
# le jeu doit tourner et le journal avancer, sinon l'essai s'arrete tout de suite
alive() {
    tasklist //NH | grep -qi horizonforbidden || { echo "JEU FERME OU PLANTE : essai arrete"; tail -n +$L0 "$H/urti_log.txt" > "$O/journal.txt"; exit 1; }
    a=$(wc -l < "$H/urti_log.txt"); sleep 2; b=$(wc -l < "$H/urti_log.txt")
    [ "$b" -gt "$a" ] || { echo "JEU FIGE (journal immobile) : essai arrete"; tail -n +$L0 "$H/urti_log.txt" > "$O/journal.txt"; exit 1; }
}
mark() { alive; echo "PHASE $1" >> "$H/urti_log.txt"; echo "$(date +%H:%M:%S) $1"; }
mouse() { powershell -NoProfile -ExecutionPolicy Bypass -File "$T/mousemove.ps1" -Process HorizonForbiddenWest "$@" | tail -1; }
key() { powershell -NoProfile -ExecutionPolicy Bypass -File "$T/hold_key.ps1" -Process HorizonForbiddenWest "$@" | tail -1; }
shot() { powershell -NoProfile -ExecutionPolicy Bypass -File "$T/capture_fenetre.ps1" -Process HorizonForbiddenWest -Out "$(cygpath -w "$O/$1.png")" | tail -1; }
sleep 15
mark immobile; sleep 8; shot immobile
mark tourne; mouse -Dx 2 -Dy 0 -Steps 400 -Delay 16
mark marche; key -Scan 0x11 -Seconds 8; shot marche
mark course; key -Scan 0x11 -Scan2 0x2A -Seconds 8; shot course
mark tourne_vite; mouse -Dx 6 -Dy 0 -Steps 300 -Delay 16
mark calme; sleep 4
# rafale caméra immobile, jeu non en pause
c0=$(grep -c "rafale .* terminée" "$H/urti_log.txt"); touch "$H/urti_rafale"
n=0; while [ "$(grep -c 'rafale .* terminée' "$H/urti_log.txt")" -le "$c0" ] && [ $n -lt 40 ]; do n=$((n+1)); sleep 1; done
d=$(ls -td "$H"/urti_rafale_* 2>/dev/null | head -1)
case "$d" in */urti_rafale_[0-9]*) mkdir -p "$O/immob"; cp -r "$d"/* "$O/immob/"; rm -rf -- "${d:?}" ;; *) echo "pas de rafale immobile" ;; esac
rm -f "$H/urti_rafale"
sleep 4
# pause : la touche Echap tenue un instant
mark pause; key -Scan 0x01 -Seconds 0.35; sleep 6; shot pause
c0=$(grep -c "rafale .* terminée" "$H/urti_log.txt"); touch "$H/urti_rafale"
n=0; while [ "$(grep -c 'rafale .* terminée' "$H/urti_log.txt")" -le "$c0" ] && [ $n -lt 40 ]; do n=$((n+1)); sleep 1; done
d=$(ls -td "$H"/urti_rafale_* 2>/dev/null | head -1)
case "$d" in */urti_rafale_[0-9]*) mkdir -p "$O/pause"; cp -r "$d"/* "$O/pause/"; rm -rf -- "${d:?}" ;; *) echo "pas de rafale" ;; esac
rm -f "$H/urti_rafale"
sleep 4; shot pause2
mark reprise; key -Scan 0x01 -Seconds 0.35; sleep 5; shot reprise
# vue d'alignement : bords de la profondeur en rouge sur l'image, pendant que la caméra tourne à vitesse constante
{ cat "$O/ini_base"; printf 'frame_trace=1\ndebug_view=11\n%b' "$EXTRA"; } > "$H/urti.ini"
sleep 6
mark align_immobile; for k in 0 1 2 3; do shot align_immobile$k; done
mark align_tourne
mouse -Dx 3 -Dy 0 -Steps 500 -Delay 16 &
sleep 1; for k in 0 1 2 3 4 5 6 7 8 9; do shot align_tourne$k; done
wait
mark align_marche
key -Scan 0x11 -Seconds 6 &
sleep 1; for k in 0 1 2 3 4 5; do shot align_marche$k; done
wait
{ cat "$O/ini_base"; printf 'frame_trace=1\n%b' "$EXTRA"; } > "$H/urti.ini"
mark fin; sleep 3
tail -n +$L0 "$H/urti_log.txt" > "$O/journal.txt"
taskkill //IM HorizonForbiddenWest.exe >/dev/null 2>&1; n=0; while tasklist //NH | grep -qi horizonforbidden && [ $n -lt 25 ]; do n=$((n+1)); sleep 1; done
tasklist //NH | grep -qi horizonforbidden && taskkill //F //IM HorizonForbiddenWest.exe >/dev/null 2>&1
echo FINI
