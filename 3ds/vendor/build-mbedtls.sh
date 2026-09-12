#!/usr/bin/env bash
#
# Builds mbedTLS as a static library for the 3DS and installs it into
# 3ds/vendor/mbedtls/.
#
# Vendored rather than installed system-wide (devkitPro ships a 3ds-mbedtls
# package) for three reasons: it needs no root, it pins an exact version, and
# the cipher set is ours -- see mbedtls_config_3ds.h for why that matters
# against Cloudflare's ECDSA-only certificate.
#
# Idempotent: skips the build if the libraries are already present.
#   ./3ds/vendor/build-mbedtls.sh [--force]

set -euo pipefail

MBEDTLS_VERSION="3.6.2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$HERE/mbedtls"
SRCDIR="$HERE/.src/mbedtls-$MBEDTLS_VERSION"
TARBALL="$HERE/.src/mbedtls-$MBEDTLS_VERSION.tar.bz2"
URL="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"

if [ ! -d "$DEVKITARM" ]; then
  echo "error: devkitARM not found at $DEVKITARM" >&2
  exit 1
fi

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

# The 3DS ABI. These must match the flags in 3ds/Makefile exactly, or the
# linker will reject the objects for mismatched float ABI.
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"

echo "==> building (this takes a few minutes)"
make -C "$SRCDIR" clean >/dev/null 2>&1 || true

make -C "$SRCDIR/library" \
  CC="$DEVKITARM/bin/arm-none-eabi-gcc" \
  AR="$DEVKITARM/bin/arm-none-eabi-ar" \
  CFLAGS="-O2 -ffunction-sections -fdata-sections -mword-relocations $ARCH -D__3DS__ -DMBEDTLS_CONFIG_FILE='\"$HERE/mbedtls_config_3ds.h\"' -I$SRCDIR/include -I$SRCDIR/library" \
  -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
  static

echo "==> installing to $PREFIX"
rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp "$SRCDIR"/library/*.a "$PREFIX/lib/"
cp -R "$SRCDIR"/include/mbedtls "$PREFIX/include/"
cp -R "$SRCDIR"/include/psa "$PREFIX/include/" 2>/dev/null || true

# The config header must travel with the headers: anything including
# <mbedtls/ssl.h> has to see the same feature set the .a was compiled with, or
# struct layouts silently disagree and the result is memory corruption rather
# than a compile error.
cp "$HERE/mbedtls_config_3ds.h" "$PREFIX/include/"

echo ""
echo "mbedTLS $MBEDTLS_VERSION installed:"
ls -la "$PREFIX/lib/"
echo ""
echo "Build against it with:"
echo "  LIBDIRS += 3ds/vendor/mbedtls"
echo "  LIBS    += -lmbedtls -lmbedx509 -lmbedcrypto"
echo "  CFLAGS  += -DMBEDTLS_CONFIG_FILE='\"mbedtls_config_3ds.h\"'"
