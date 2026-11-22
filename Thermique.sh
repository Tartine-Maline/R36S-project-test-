#!/bin/bash
# Launcher optimisé pour R36S (Sans réseau)
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$DIR/uti120_out"
BIN=/tmp/uti120
mkdir -p "$OUT"

# Vérification espace SD (pour éviter les crashs de capture)
FREE_KB=$(df "$DIR" | tail -n 1 | awk '{print $4}')
if [ "$FREE_KB" -lt 10240 ]; then
  echo "ERREUR: Espace SD insuffisant (<10MB)" > "$OUT/error_sd.txt"
  exit 1
fi

# Compilation (auto-détecte les headers locaux si présents)
SDLF="$(sdl2-config --cflags --libs 2>/dev/null)"
[ -z "$SDLF" ] && SDLF="-I/usr/include/SDL2 -lSDL2"

gcc -O3 -pthread -o "$BIN" "$DIR/uti120.c" $SDLF -l:libusb-1.0.so.0 -lm 2>&1 > "$OUT/build_log.txt"

if [ -x "$BIN" ]; then
  # Donne l'accès USB et lance l'app
  sudo chmod a+rw /dev/bus/usb/*/* 2>/dev/null
  "$BIN" -r 1 -d "$OUT" -l "$OUT/log_affichage.txt"
else
  echo "Erreur de compilation. Verifiez uti120.c" > "$OUT/error.txt"
fi
sync