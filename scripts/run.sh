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
# The firmware mounts /dev/hda1 on /media/hdd, and its sysmon applet crashes in
# a loop without a drive present, which stops the UI coming up at all.
HDD="${HDD:-$BUILD/hdd.qcow2}"

RAM="${RAM:-64}"
# The board creates the AC97 device itself, so audio needs a *default* backend:
# "-audio <driver>" (no model=) is what establishes one. Override with
# AUDIO_DRV=alsa, or AUDIO_DRV=none to boot silently.
AUDIO_DRV="${AUDIO_DRV:-$DEFAULT_AUDIO}"
# The window this opens stays black -- the device has no screen -- but it is
# what captures keystrokes for the emulated keypad. DISPLAY_BACKEND=none for a
# headless run driven only from the serial console.
DISPLAY_BACKEND="${DISPLAY_BACKEND:-sdl}"
BOARD_ID="${BOARD_ID:-2}"
# 1 selects the keycode array that has space, shift and control.
KEYPAD_ID="${KEYPAD_ID:-1}"
# The stock kernel's built-in cmdline is
#   root=/dev/mtdblock2 rw rootfstype=jffs2 console=ttyS0,115200
# but the shipped /etc/fstab mounts mtdblock3, i.e. the real bootloader passes
# a four-partition map. We supply that map explicitly.
MTDPARTS="$(python3 "$REPO_ROOT/tools/bpimage.py" mtdparts)"
APPEND="${APPEND:-root=/dev/mtdblock3 rw rootfstype=jffs2 console=ttyS0,115200 $MTDPARTS}"

for f in "$QEMU" "$KERNEL" "$FLASH"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
# The drive is attached only if you have created one. It is NOT created
# automatically yet: the IDE interrupt is not being delivered, so the guest logs
# "hda: lost interrupt" and `insmod pxa2xx-ide` wedges inside /etc/StartShell,
# which stops the launcher and shell starting at all. Until that is fixed,
# booting without a drive is the better of two broken options.
HDD_ARGS=""
if [ -e "$HDD" ]; then
    HDD_ARGS="-drive if=ide,index=0,format=qcow2,file=$HDD"
fi

exec "$QEMU" \
    -M "everest,board-id=$BOARD_ID,keypad-id=$KEYPAD_ID" \
    -m "$RAM" \
    -kernel "$KERNEL" \
    -append "$APPEND" \
    -drive if=mtd,format=raw,file="$FLASH" \
    $HDD_ARGS \
    -audio "$AUDIO_DRV" \
    -serial mon:stdio \
    -display "$DISPLAY_BACKEND" \
    "$@"
