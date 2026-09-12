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

# Our mbedTLS build omits SHA-1 (MBEDTLS_SHA1_C is off), so it cannot parse a
# root signed with sha1WithRSAEncryption -- mbedtls_x509_crt_parse rejects it
# with "Signature algorithm (oid) is unsupported". Shipping such a root is
# worse than omitting it: the bundle looks like it covers an issuer it cannot
# actually load. So they are filtered out here, loudly.
skipped=0
kept=0
{
  echo "# Trust roots for Pong3DS. Regenerate with 3ds/vendor/fetch-cacerts.sh."
  echo "#"
  echo "# SHA-1 signed roots are omitted: our mbedTLS build has SHA-1 disabled and"
  echo "# cannot parse them. See the filter in fetch-cacerts.sh."
  for u in "${SRC[@]}"; do
    curl -sSL -m 20 "$u" -o "$tmp/c.pem"
    subject="$(openssl x509 -in "$tmp/c.pem" -noout -subject 2>/dev/null)" \
      || { echo "bad cert from $u" >&2; exit 1; }
    sigalg="$(openssl x509 -in "$tmp/c.pem" -noout -text 2>/dev/null \
              | awk "/Signature Algorithm/ {print \$3; exit}")"
    case "$sigalg" in
      *sha1*|*SHA1*)
        echo "  skipping (SHA-1, unparseable by our build): $subject" >&2
        skipped=$((skipped + 1))
        continue
        ;;
    esac
    echo "# $subject  [$sigalg]"
    cat "$tmp/c.pem"
    kept=$((kept + 1))
  done
} > "$OUT"

echo "wrote $OUT: $kept certs kept, $skipped skipped"
