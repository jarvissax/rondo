#!/usr/bin/env bash
# The roots the Y1 trusts for podcast hosts (direct/rss.cpp): Apple's own list, exported from this Mac's
# system keychain, expired ones left out, into native/assets/certs/podcast-roots.pem. Android 4.2's
# roots are from 2012, and podcast hosts use every certificate authority there is (Megaphone's chain
# ends at USERTrust ECC, Amazon's CDNs at Amazon Root CA 1, which it doesn't have). Run it again now and
# then, on an up-to-date Mac; the build embeds the file (native/CMakeLists.txt).
#   tools/native/podcast-roots.sh
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$ROOT/native/assets/certs/podcast-roots.pem
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
security find-certificate -a -p /System/Library/Keychains/SystemRootCertificates.keychain > "$TMP/all.pem"
# one file per certificate, then only those still good
awk -v dir="$TMP" '/BEGIN CERTIFICATE/ { n++ } { print > (dir "/c" n ".pem") }' "$TMP/all.pem"
: > "$TMP/good.pem"
kept=0
for c in "$TMP"/c*.pem; do
  if openssl x509 -in "$c" -noout -checkend 0 >/dev/null 2>&1; then
    subject=$(openssl x509 -in "$c" -noout -subject 2>/dev/null | sed 's/^subject= *//')
    { echo "# $subject"; cat "$c"; } >> "$TMP/good.pem"
    kept=$((kept + 1))
  fi
done
{ echo "# Apple's trusted roots from macOS's SystemRootCertificates.keychain, $(date -u +%Y-%m-%d), unexpired ($kept)"; cat "$TMP/good.pem"; } > "$OUT"
echo "$OUT: $kept roots"
