#!/usr/bin/env bash
#
# Rebuilds 3ds/romfs/cacert.pem.
#
# Roots are selected BY NAME from Mozilla's curated bundle (as published by the
# curl project) rather than fetched individually from per-CA URLs. An earlier
# version did the latter and one of the hand-written URLs silently returned an
# unrelated leaf certificate, which ended up in the bundle looking like a trust
# root. Selecting from a known-good bundle by subject makes that impossible:
# either the name is present or the script fails.
#
# Only the roots actually needed are included. A full 140-root bundle would be
# ~200KB of romfs and, more importantly, would mean trusting every CA on earth
# for a game that talks to exactly two hosts.
#
#   ./3ds/vendor/fetch-cacerts.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../romfs/cacert.pem"
SOURCE="https://curl.se/ca/cacert.pem"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "==> fetching Mozilla root bundle"
curl -sSL -m 60 "$SOURCE" -o "$tmp/all.pem"

python3 - "$tmp/all.pem" "$OUT" <<'PYEOF'
import re, subprocess, sys

src, out = sys.argv[1], sys.argv[2]

# Exactly the roots our two endpoints chain to. Keep this list short and
# justified -- every entry is a CA the console will trust.
WANTED = {
    # pong.wardcrew.com, behind Cloudflare.
    "GTS Root R4":                            "Cloudflare / Google Trust Services (current chain)",
    "GTS Root R1":                            "Cloudflare / Google Trust Services (alternate)",
    "DigiCert Global Root G2":                "Cloudflare (historical, still issued)",
    # GitHub, for updating straight from Releases.
    "USERTrust ECC Certification Authority":  "github.com and api.github.com",
    "USERTrust RSA Certification Authority":  "github.com (RSA chain)",
    "ISRG Root X1":                           "release-assets.githubusercontent.com, Let's Encrypt",
}

blocks = re.findall(
    r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
    open(src).read(), re.S)

found, skipped_sha1 = {}, []
for pem in blocks:
    try:
        text = subprocess.run(["openssl", "x509", "-noout", "-subject", "-text"],
                              input=pem, capture_output=True, text=True,
                              check=True).stdout
    except subprocess.CalledProcessError:
        continue

    cn = None
    m = re.search(r"CN\s*=\s*([^,/\n]+)", text)
    if m:
        cn = m.group(1).strip()
    if cn not in WANTED or cn in found:
        continue

    sig = ""
    ms = re.search(r"Signature Algorithm:\s*(\S+)", text)
    if ms:
        sig = ms.group(1)

    # Our mbedTLS build has SHA-1 disabled and cannot parse such a certificate.
    # Including one would advertise coverage that does not exist.
    if "sha1" in sig.lower():
        skipped_sha1.append(cn)
        continue

    found[cn] = (pem, sig)

missing = [cn for cn in WANTED if cn not in found and cn not in skipped_sha1]
if missing:
    print("ERROR: these roots were not found in the Mozilla bundle:", file=sys.stderr)
    for cn in missing:
        print("  " + cn, file=sys.stderr)
    sys.exit(1)

with open(out, "w") as f:
    f.write("# Trust roots for Pong3DS. Regenerate with 3ds/vendor/fetch-cacerts.sh.\n")
    f.write("#\n")
    f.write("# Selected by name from Mozilla's bundle; only the CAs our two\n")
    f.write("# endpoints actually chain to. SHA-1 roots are excluded because this\n")
    f.write("# mbedTLS build has SHA-1 disabled and cannot parse them.\n#\n")
    for cn, (pem, sig) in found.items():
        f.write(f"# {cn}  [{sig}]\n#   {WANTED[cn]}\n")
    f.write("\n")
    for cn, (pem, sig) in found.items():
        f.write(f"# {cn}\n{pem}\n")

print(f"wrote {out}: {len(found)} roots")
for cn in found:
    print(f"  + {cn}")
for cn in skipped_sha1:
    print(f"  - {cn} (SHA-1, unparseable by our build)")
PYEOF
