#!/usr/bin/env bash
#
# Fetches the CIA packaging tools. They are not part of devkitPro, and they are
# vendored per-repo rather than installed system-wide so the build needs no root
# and pins exact versions.
#
#   ./3ds/vendor/fetch-tools.sh

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/tools"
mkdir -p "$TOOLS"

MAKEROM_VER="v0.19.0"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  MAKEROM_ASSET="makerom-$MAKEROM_VER-macos_arm64.zip";  BT_DIR="mac-x86_64" ;;
  Darwin-x86_64) MAKEROM_ASSET="makerom-$MAKEROM_VER-macos_x86_64.zip"; BT_DIR="mac-x86_64" ;;
  Linux-x86_64)  MAKEROM_ASSET="makerom-$MAKEROM_VER-ubuntu_x86_64.zip"; BT_DIR="linux-x86_64" ;;
  *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ ! -x "$TOOLS/makerom" ]; then
  echo "==> makerom $MAKEROM_VER"
  curl -sSL -o "$tmp/makerom.zip" \
    "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-$MAKEROM_VER/$MAKEROM_ASSET"
  unzip -oq "$tmp/makerom.zip" -d "$tmp/mr"
  cp "$(find "$tmp/mr" -name makerom -type f | head -1)" "$TOOLS/makerom"
  chmod +x "$TOOLS/makerom"
  xattr -d com.apple.quarantine "$TOOLS/makerom" 2>/dev/null || true
fi

if [ ! -x "$TOOLS/bannertool" ]; then
  echo "==> bannertool"
  curl -sSL -o "$tmp/bt.zip" \
    "https://github.com/Epicpkmn11/bannertool/releases/download/v1.2.2/bannertool.zip"
  unzip -oq "$tmp/bt.zip" -d "$tmp/bt"
  cp "$tmp/bt/$BT_DIR/bannertool" "$TOOLS/bannertool"
  chmod +x "$TOOLS/bannertool"
  xattr -d com.apple.quarantine "$TOOLS/bannertool" 2>/dev/null || true
fi

echo "tools ready:"
ls -la "$TOOLS"
