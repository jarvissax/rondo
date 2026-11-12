#!/usr/bin/env bash
# Builds and signs a Rondo 2.0 update bundle (native/core/bundle.h): native/build/bundle/rondo-<version>.rondo.
#
#   tools/native/bundle.sh [key.pem]     the key: RONDO_BUNDLE_KEY, else ~/Desktop/rondo-signing/bundle-ed25519.pem
#
# What it holds, as /system/rondo and /data/rondo/<version> lay it out:
#   rondo              the binary, stripped
#   lib/               FFmpeg's shared libraries (LGPL: native/third_party/y1/lib)
#   fonts/             Roboto and Noto Color Emoji (native/assets/fonts, native/third_party)
#   advances.bin       Android's glyph advances, kerning.bin its kerning (native/assets)
#   dashboard/         the dashboard's page (native/assets/dashboard)
#   start              how init starts it (firmware/rondo/start), kept with the version
#   VERSION            the version (native/core/version.h)
# The signature is Ed25519 over the SHA-256 of the .tar.gz, by Node (no other tool needed); the key's
# private half never enters the repo.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KEY=${1:-${RONDO_BUNDLE_KEY:-$HOME/Desktop/rondo-signing/bundle-ed25519.pem}}
VERSION=$(sed -n 's/.*kRondoVersion = "\(.*\)".*/\1/p' "$ROOT/native/core/version.h")
STRIP=${STRIP:-$HOME/Library/Android/sdk/ndk/23.2.8568313/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip}
OUT=$ROOT/native/build/bundle
STAGE=$OUT/rondo-$VERSION
[[ -f $KEY ]] || { echo "No bundle key at $KEY" >&2; exit 1; }
"$ROOT/tools/native/build.sh" rondo
rm -rf "$STAGE" && mkdir -p "$STAGE/lib" "$STAGE/fonts" "$STAGE/dashboard"
"$STRIP" -o "$STAGE/rondo" "$ROOT/native/build/y1/rondo"
cp "$ROOT"/native/third_party/y1/lib/libav{codec,format,util}.so "$ROOT/native/third_party/y1/lib/libswresample.so" "$STAGE/lib/"
cp "$ROOT/native/assets/fonts/Roboto-Regular.ttf" "$ROOT/native/third_party/y1/fonts/NotoColorEmoji.ttf" "$STAGE/fonts/"
cp "$ROOT/native/assets/advances.bin" "$ROOT/native/assets/kerning.bin" "$STAGE/"
cp "$ROOT"/native/assets/dashboard/{index.html,app.js,app.css} "$STAGE/dashboard/"
cp "$ROOT/firmware/rondo/start" "$STAGE/start"
echo "$VERSION" > "$STAGE/VERSION"
chmod 755 "$STAGE/rondo" "$STAGE/start"
# ustar, no macOS extras (resource forks, extended attributes)
(cd "$STAGE" && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs -czf "$OUT/rondo-$VERSION.tar.gz" .)
node - "$KEY" "$OUT/rondo-$VERSION.tar.gz" "$OUT/rondo-$VERSION.rondo" <<'JS'
const fs = require("fs"), crypto = require("crypto");
const [key, gz, out] = process.argv.slice(2);
const body = fs.readFileSync(gz);
const digest = crypto.createHash("sha256").update(body).digest();
const sig = crypto.sign(null, digest, crypto.createPrivateKey(fs.readFileSync(key)));
if (sig.length !== 64) throw new Error("not an Ed25519 key");
fs.writeFileSync(out, Buffer.concat([Buffer.from("RONDOB1\n"), sig, body]));
console.log(`${out}: ${(fs.statSync(out).size / 1048576).toFixed(1)} MB, SHA-256 of the archive ${digest.toString("hex")}`);
JS
rm -f "$OUT/rondo-$VERSION.tar.gz"
