#!/usr/bin/env bash
#
# Boot the Braille+ / Icon firmware under the emulated Everest board.
#
# Expects build/flash.img and build/parts/kernel.bin, both produced by
# scripts/build-image.sh.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HOME/qemu}"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE=.exe; DEFAULT_AUDIO=dsound ;;
    *)                    EXE=;     DEFAULT_AUDIO=pa     ;;
esac
QEMU="${QEMU:-$QEMU_DIR/build/qemu-system-arm$EXE}"

BUILD="$REPO_ROOT/build"
KERNEL="${KERNEL:-$BUILD/parts/kernel.bin}"
FLASH="${FLASH:-$BUILD/flash.img}"

RAM="${RAM:-64}"
# The board creates the AC97 device itself, so audio needs a *default* backend:
# "-audio <driver>" (no model=) is what establishes one. Override with
# AUDIO_DRV=alsa, or AUDIO_DRV=none to boot silently.
AUDIO_DRV="${AUDIO_DRV:-$DEFAULT_AUDIO}"
BOARD_ID="${BOARD_ID:-2}"
KEYPAD_ID="${KEYPAD_ID:-0}"
# The stock kernel's built-in cmdline is
#   root=/dev/mtdblock2 rw rootfstype=jffs2 console=ttyS0,115200
# but the shipped /etc/fstab mounts mtdblock3, i.e. the real bootloader passes
# a four-partition map. We supply that map explicitly.
MTDPARTS="$(python3 "$REPO_ROOT/tools/bpimage.py" mtdparts)"
APPEND="${APPEND:-root=/dev/mtdblock3 rw rootfstype=jffs2 console=ttyS0,115200 $MTDPARTS}"

for f in "$QEMU" "$KERNEL" "$FLASH"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

exec "$QEMU" \
    -M "everest,board-id=$BOARD_ID,keypad-id=$KEYPAD_ID" \
    -m "$RAM" \
    -kernel "$KERNEL" \
    -append "$APPEND" \
    -drive if=mtd,format=raw,file="$FLASH" \
    -audio "$AUDIO_DRV" \
    -serial mon:stdio \
    -display none \
    "$@"
