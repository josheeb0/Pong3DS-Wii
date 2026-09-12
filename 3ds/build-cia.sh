#!/usr/bin/env bash
#
# Packages a built .elf into an installable .cia.
#
# A CIA installs to the HOME menu, gets its own title and 64MB of application
# memory, and survives without the Homebrew Launcher. The .3dsx remains the
# development artifact because 3dslink can push it over wifi in a second (see
# `make send`); the CIA is what you install once you are happy with it.
#
#   ./3ds/build-cia.sh <path/to/app.elf> [output.cia]

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/vendor/tools"
ASSETS="$HERE/assets"
BUILD="$HERE/.ciabuild"

ELF="${1:-$HERE/pong3ds.elf}"
OUT="${2:-$HERE/pong3ds.cia}"

TITLE="${APP_TITLE:-Pong3DS}"
DESC="${APP_DESCRIPTION:-Cross-play Pong vs a browser}"
AUTHOR="${APP_AUTHOR:-Pong3DS}"

for t in makerom bannertool; do
  if [ ! -x "$TOOLS/$t" ]; then
    echo "error: $TOOLS/$t missing. Run ./3ds/vendor/fetch-tools.sh" >&2
    exit 1
  fi
done

if [ ! -f "$ELF" ]; then
  echo "error: $ELF not found -- build the .3dsx first (make -C 3ds)" >&2
  exit 1
fi

mkdir -p "$BUILD"

# The banner format requires a tune. Ours is silence, which is the right choice
# for a game launched from a menu you are already looking at -- and 260KB of
# zeros does not belong in git, so it is generated here.
if [ ! -f "$ASSETS/banner.wav" ]; then
  echo "==> banner audio (silence)"
  python3 - "$ASSETS/banner.wav" <<'PYEOF'
import sys, wave
with wave.open(sys.argv[1], 'w') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(22050)
    w.writeframes(b'\x00\x00\x00\x00' * int(22050 * 3))
PYEOF
fi

echo "==> banner"
"$TOOLS/bannertool" makebanner \
  -i "$ASSETS/banner.png" \
  -a "$ASSETS/banner.wav" \
  -o "$BUILD/banner.bnr" >/dev/null

echo "==> icon"
"$TOOLS/bannertool" makesmdh \
  -s "$TITLE" -l "$DESC" -p "$AUTHOR" \
  -i "$ASSETS/icon.png" \
  -o "$BUILD/icon.icn" >/dev/null

echo "==> cia"
# romfs is optional; only pass it if the directory has content, since makerom
# rejects an empty one.
# makerom resolves the RSF's RootPath relative to its own cwd, so pass an
# absolute path through a variable instead.
ROMFS_ARG=(-DROMFS_ROOT="$HERE/romfs")

"$TOOLS/makerom" -f cia -o "$OUT" \
  -elf "$ELF" \
  -rsf "$HERE/pong3ds.rsf" \
  -icon "$BUILD/icon.icn" \
  -banner "$BUILD/banner.bnr" \
  -exefslogo -target t \
  ${ROMFS_ARG[@]+"${ROMFS_ARG[@]}"}

echo ""
echo "built $(basename "$OUT") -- $(ls -lh "$OUT" | awk '{print $5}')"
echo "Install it with FBI (copy to your SD card), or over the network:"
echo "  FBI -> Remote Install -> Receive URLs over the network"
