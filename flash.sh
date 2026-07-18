#!/usr/bin/env bash
# One-command build + flash helper for the HML board.
#   ./flash.sh          build web UI + firmware, then flash if a BOOTSEL drive is mounted
#   ./flash.sh fw       firmware only (skip the slow web build)
#   ./flash.sh www      web UI + firmware relink only
# For pure web-UI iteration you usually don't need this at all — run
#   cd www && npm run dev-board
# which hot-reloads the UI in the browser against the live board (no flashing).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
UF2="$BUILD/GP2040-CE_0.0.0_HML.uf2"

mode="${1:-all}"
if [ "$mode" != "fw" ]; then
  echo "==> building web UI"
  (cd "$ROOT/www" && npm run build)
fi
echo "==> building firmware"
cmake --build "$BUILD" -j8

# Find a BOOTSEL (RPI-RP2) mass-storage drive mounted under /mnt (WSL/drvfs).
target=""
for d in /mnt/*; do
  [ -f "$d/INFO_UF2.TXT" ] 2>/dev/null && target="$d" && break
done

if [ -n "$target" ]; then
  echo "==> flashing $UF2 -> $target"
  cp "$UF2" "$target/"
  echo "done — the board reboots automatically."
else
  echo
  echo "UF2 built: $UF2"
  echo "BOOTSEL drive not found under /mnt. To flash:"
  echo "  1) enter BOOTSEL: hold Share+Option+Up while plugging in USB (or the BOOTSEL button)"
  echo "  2) on Windows it mounts as RPI-RP2 (say E:). Mount it in WSL once:"
  echo "       sudo mkdir -p /mnt/e && sudo mount -t drvfs E: /mnt/e"
  echo "  3) re-run ./flash.sh fw   (or: cp \"$UF2\" /mnt/e/ )"
fi
