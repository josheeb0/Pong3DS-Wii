#!/usr/bin/env bash
# Rebuilds 3ds/romfs/cacert.pem from upstream. Run if Cloudflare changes issuer
# and the console starts refusing to connect.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../romfs/cacert.pem"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

declare -a SRC=(
  "https://i.pki.goog/r4.pem"
  "https://i.pki.goog/r1.pem"
  "https://letsencrypt.org/certs/isrgrootx1.pem"
  "https://cacerts.digicert.com/DigiCertGlobalRootCA.crt.pem"
  "https://cacerts.digicert.com/DigiCertGlobalRootG2.crt.pem"
  "https://cacerts.digicert.com/BaltimoreCyberTrustRoot.crt.pem"
)

{
  echo "# Trust roots for Pong3DS. Regenerate with 3ds/vendor/fetch-cacerts.sh."
  for u in "${SRC[@]}"; do
    curl -sSL -m 20 "$u" -o "$tmp/c.pem"
    openssl x509 -in "$tmp/c.pem" -noout -subject >/dev/null || { echo "bad cert from $u" >&2; exit 1; }
    echo "# $(openssl x509 -in "$tmp/c.pem" -noout -subject)"
    cat "$tmp/c.pem"
  done
} > "$OUT"

echo "wrote $OUT ($(grep -c 'BEGIN CERTIFICATE' "$OUT") certs)"
