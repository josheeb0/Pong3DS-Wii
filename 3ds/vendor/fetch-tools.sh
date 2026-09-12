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

# Newest first. makerom v0.19.0 is built against glibc 2.38, which is newer
# than the devkitPro container (Debian bookworm, 2.36) provides -- so rather
# than pinning a version and hoping, we try each and keep the first that
# actually RUNS on this machine.
MAKEROM_VERSIONS=("v0.19.0" "v0.18.4")

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)  MAKEROM_PLAT="macos_arm64";   BT_DIR="mac-x86_64" ;;
  Darwin-x86_64) MAKEROM_PLAT="macos_x86_64";  BT_DIR="mac-x86_64" ;;
  Linux-x86_64)  MAKEROM_PLAT="ubuntu_x86_64"; BT_DIR="linux-x86_64" ;;
  *) echo "unsupported platform $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Does this makerom actually run on this machine?
#
# Two traps here, both of which cost an hour to find:
#   - `makerom -help` exits non-zero even on success, so the exit code is
#     meaningless; match its banner instead.
#   - piping into `grep -q` makes grep exit at the first match, which sends
#     SIGPIPE to makerom, and `set -o pipefail` then reports the whole pipeline
#     as failed. So capture the output first and match it in the shell.
runs_ok() {
  local out
  out="$("$1" -help 2>&1 || true)"
  case "$out" in
    *akerom*|*AKEROM*) return 0 ;;
    *) return 1 ;;
  esac
}

if [ ! -x "$TOOLS/makerom" ] || ! runs_ok "$TOOLS/makerom"; then
  got=""
  for ver in "${MAKEROM_VERSIONS[@]}"; do
    echo "==> makerom $ver"
    rm -rf "$tmp/mr"; mkdir -p "$tmp/mr"
    if ! curl -sSL -o "$tmp/makerom.zip" \
      "https://github.com/3DSGuy/Project_CTR/releases/download/makerom-$ver/makerom-$ver-$MAKEROM_PLAT.zip"; then
      echo "    download failed, trying older"
      continue
    fi
    unzip -oq "$tmp/makerom.zip" -d "$tmp/mr" || continue
    bin="$(find "$tmp/mr" -name makerom -type f | head -1)"
    [ -n "$bin" ] || continue
    cp "$bin" "$TOOLS/makerom"
    chmod +x "$TOOLS/makerom"
    xattr -d com.apple.quarantine "$TOOLS/makerom" 2>/dev/null || true

    # Verify it runs HERE. A binary that downloads fine but cannot start --
    # usually a glibc newer than this system's -- is worse than useless,
    # because the failure only surfaces at packaging time.
    if runs_ok "$TOOLS/makerom"; then
      echo "    ok ($ver runs on this system)"
      got="$ver"
      break
    fi
    echo "    $ver will not run here ($("$TOOLS/makerom" -help 2>&1 | grep -oE 'GLIBC_[0-9.]+ not found' | head -1))"
    rm -f "$TOOLS/makerom"
  done
  if [ -z "$got" ]; then
    echo "error: no usable makerom build for this system" >&2
    exit 1
  fi
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
