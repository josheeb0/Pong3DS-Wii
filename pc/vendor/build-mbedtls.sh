#!/usr/bin/env bash
#
# Builds mbedTLS as a static library for the HOST and installs it into
# pc/vendor/mbedtls/.
#
# The desktop client needs TLS for the same reason the 3DS does: the public
# server is reachable only on 443 through a Cloudflare tunnel, so raw TCP gets
# you nowhere outside the LAN. Vendored for the same reasons as the 3DS build --
# no root needed, an exact pinned version, and the same library on every
# platform rather than three different system packages.
#
# Unlike the 3DS build this uses mbedTLS's DEFAULT configuration. The 3DS has a
# trimmed config because a .3dsx has to fit in a console's memory; a desktop
# does not care, and the default is a superset of what the trimmed one enables,
# so anything that negotiates on the console negotiates here.
#
# Same plain `make -C library static` the 3DS build uses. mbedTLS ships a
# makefile, so this needs no cmake -- which matters because the machines that
# build this do not all have one.
#
# Idempotent: skips if the libraries are already present.
#   ./pc/vendor/build-mbedtls.sh [--force]

set -euo pipefail

MBEDTLS_VERSION="3.6.2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$HERE/mbedtls"
SRCDIR="$HERE/.src/mbedtls-$MBEDTLS_VERSION"
TARBALL="$HERE/.src/mbedtls-$MBEDTLS_VERSION.tar.bz2"
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"

if [ "${1:-}" != "--force" ] && [ -f "$PREFIX/lib/libmbedtls.a" ]; then
  echo "mbedTLS already built at $PREFIX -- pass --force to rebuild"
  exit 0
fi

mkdir -p "$HERE/.src"

if [ ! -f "$TARBALL" ]; then
  echo "==> fetching mbedTLS $MBEDTLS_VERSION"
  curl -sSL -o "$TARBALL" "$URL"
fi

if [ ! -d "$SRCDIR" ]; then
  echo "==> extracting"
  tar -xjf "$TARBALL" -C "$HERE/.src"
fi

echo "==> building for the host (this takes a few minutes)"
make -C "$SRCDIR" clean >/dev/null 2>&1 || true

make -C "$SRCDIR/library" \
  CFLAGS="-O2 -fPIC -I$SRCDIR/include -I$SRCDIR/library" \
  -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
  static

echo "==> installing to $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp "$SRCDIR"/library/*.a "$PREFIX/lib/"
cp -R "$SRCDIR"/include/mbedtls "$PREFIX/include/"
cp -R "$SRCDIR"/include/psa "$PREFIX/include/" 2>/dev/null || true

echo "==> done: $PREFIX"
