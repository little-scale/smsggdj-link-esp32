#!/usr/bin/env bash
# make-release.sh — build the shippable, credential-free binaries for every
# supported board and stage them for distribution (merged images + the GitHub
# Pages web flasher under docs/). esp-web-tools auto-selects the build that
# matches whichever board the user plugs in.
#
#   esp32c3  — Ableton Link over WiFi
#   esp32s3  — Ableton Link + USB-MIDI clock (composite USB)
#
# Usage:
#   . ~/esp/esp-idf/export.sh      # activate ESP-IDF first
#   tools/make-release.sh          # both targets
#   tools/make-release.sh esp32s3  # one target
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v idf.py >/dev/null 2>&1; then
  echo "error: idf.py not found. Run '. ~/esp/esp-idf/export.sh' first." >&2
  exit 1
fi

TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=(esp32c3 esp32s3)

VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"
mkdir -p dist docs/firmware

for CHIP in "${TARGETS[@]}"; do
  OUT_NAME="smsggdj-bridge-${CHIP}.bin"
  echo ">> building ${VERSION} for ${CHIP} (SHIP=1: no compile-time creds)"
  rm -rf build                        # clean slate per target
  idf.py set-target "${CHIP}" >/dev/null
  idf.py -DSHIP=1 build
  # Single raw image flashable at offset 0x0 (bootloader + table + app merged).
  # Absolute output path: idf.py runs esptool from the build directory.
  idf.py merge-bin -f raw -o "${ROOT}/dist/${OUT_NAME}" >/dev/null
  cp "${ROOT}/dist/${OUT_NAME}" "${ROOT}/docs/firmware/${OUT_NAME}"
  echo "   -> dist/${OUT_NAME} ($(wc -c < "dist/${OUT_NAME}" | tr -d ' ') bytes)"
done

# Stamp the web-flasher manifest with this version (best-effort; needs no jq).
if [ -f docs/manifest.json ]; then
  tmp="$(mktemp)"
  sed "s/\"version\": *\"[^\"]*\"/\"version\": \"${VERSION}\"/" docs/manifest.json > "$tmp" && mv "$tmp" docs/manifest.json
fi

cat <<EOF

>> done. release ${VERSION} for: ${TARGETS[*]}
   dist/ + docs/firmware/ updated.

Flash with esptool (no ESP-IDF needed), e.g.:
   pip install esptool
   esptool.py --chip esp32s3 -p PORT write_flash 0x0 dist/smsggdj-bridge-esp32s3.bin

Or publish the web flasher: commit docs/ and enable GitHub Pages
(Settings -> Pages -> Source: main /docs), then open the page in Chrome/Edge.
EOF
