#!/usr/bin/env bash
#
# Unpack a .lsi firmware bundle and assemble the OneNAND flash image.
#
#   scripts/build-image.sh /path/to/2.2.53.lsi
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LSI="${1:-}"
BUILD="$REPO_ROOT/build"
PARTS="$BUILD/parts"
FLASH="$BUILD/flash.img"
SIZE_MB="${SIZE_MB:-128}"

if [ -z "$LSI" ]; then
    echo "usage: $0 <firmware.lsi>" >&2
    exit 1
fi

mkdir -p "$BUILD"
python3 "$REPO_ROOT/tools/bpimage.py" unpack "$LSI" "$PARTS"
python3 "$REPO_ROOT/tools/bpimage.py" mkflash "$PARTS" "$FLASH" --size "$SIZE_MB"

echo
echo "Ready. Boot it with: scripts/run.sh"
