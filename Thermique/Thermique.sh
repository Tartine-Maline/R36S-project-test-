#!/bin/bash
# Camera thermique UNI-T UTi120Mobile - R36S / dArkOS (version 2)
# A placer dans le dossier "ports" avec uti120.c (meme dossier).
# Reglages, captures et journaux : dossier uti120_out/ (a cote de ce script).
# Rendu logiciel (si l'affichage montre des defauts) : creer un fichier vide
#   uti120_out/rendu_logiciel

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/uti120_out"
BIN=/tmp/uti120
mkdir -p "$OUT"
[ -f "$OUT/log_affichage.txt" ] && mv -f "$OUT/log_affichage.txt" "$OUT/log_precedent.txt"
rm -f "$OUT/build.txt" "$OUT/dmesg_usb.txt"

# 1. Compilation (quelques secondes)
SDLF="$(sdl2-config --cflags --libs 2>/dev/null)"
[ -z "$SDLF" ] && SDLF="-I/usr/include/SDL2 -lSDL2"
{
  echo "== compilation $(date) =="
  gcc -O2 -pthread -o "$BIN" "$DIR/uti120.c" $SDLF -l:libusb-1.0.so.0 -lm 2>&1
  echo "gcc exit: $?"
} > "$OUT/build.txt" 2>&1
[ -x "$BIN" ] || exit 1

# 2. Droits d'acces a la camera USB
sudo chmod a+rw /dev/bus/usb/*/* 2>/dev/null

# 3. Application
OPT=""
[ -f "$OUT/rendu_logiciel" ] && OPT="-s"
"$BIN" -r 1 $OPT -d "$OUT" -l "$OUT/log_affichage.txt"
RC=$?
echo "code de retour: $RC" >> "$OUT/log_affichage.txt"
if [ "$RC" -ne 0 ]; then
  { echo "== messages noyau USB =="; sudo dmesg 2>&1 | grep -i -E "usb|dwc2|otg|vbus|over-current" | tail -n 30; } > "$OUT/dmesg_usb.txt" 2>&1
fi
sync
