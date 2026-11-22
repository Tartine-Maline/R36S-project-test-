#!/bin/bash
# Camera thermique UNI-T UTi120Mobile - R36S / dArkOS (Version 3)
# Place ce script dans le dossier "ports" avec uti120.c (même dossier).
# Réglages, captures et journaux : dossier uti120_out/ (à côté de ce script).
# Rendu logiciel (si l'affichage montre des défauts) : créer un fichier vide
#   uti120_out/rendu_logiciel

set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/uti120_out"
BIN="/tmp/uti120"
mkdir -p "$OUT"

[ -f "$OUT/log_affichage.txt" ] && mv -f "$OUT/log_affichage.txt" "$OUT/log_precedent.txt"
rm -f "$OUT/build.txt" "$OUT/dmesg_usb.txt"

SDLF="$(pkg-config --cflags --libs sdl2 2>/dev/null || sdl2-config --cflags --libs 2>/dev/null)"
[ -z "$SDLF" ] && SDLF="-I/usr/include/SDL2 -lSDL2"
USBLIB="$(pkg-config --libs libusb-1.0 2>/dev/null || echo "-l:libusb-1.0.so.0")"

{
  echo "== compilation $(date) =="
  gcc -Ofast -march=armv8-a+simd -flto -pthread \
      -o "$BIN" "$DIR/uti120.c" \
      $SDLF $USBLIB -lm \
      -Wno-unused-parameter -Wno-unused-variable \
      2>&1
  echo "gcc exit: $?"
} > "$OUT/build.txt" 2>&1

[ -x "$BIN" ] || { cat "$OUT/build.txt"; exit 1; }

sudo chmod a+rw /dev/bus/usb/*/* 2>/dev/null

OPT=""
[ -f "$OUT/rendu_logiciel" ] && OPT="-s"

"$BIN" -r 1 $OPT -d "$OUT" -l "$OUT/log_affichage.txt"
RC=$?

echo "code de retour: $RC" >> "$OUT/log_affichage.txt"
if [ "$RC" -ne 0 ]; then
  { echo "== messages noyau USB =="; sudo dmesg -T 2>&1 | grep -i -E "usb|dwc2|otg|vbus|over-current" | tail -n 30; } > "$OUT/dmesg_usb.txt" 2>&1
fi
sync