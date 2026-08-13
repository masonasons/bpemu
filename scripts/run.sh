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
#
# Prefer SDL, then GTK. A QEMU built without libsdl2-dev has no sdl display at
# all and refuses to start, and for a window that is black by design either one
# serves equally well -- so pick whichever this binary actually has rather than
# making libsdl2-dev a requirement. "none" is always available, so this always
# resolves.
if [ -z "${DISPLAY_BACKEND:-}" ]; then
    HAVE_DISPLAYS="$("$QEMU" -display help 2>/dev/null || true)"
    for d in sdl gtk none; do
        if printf '%s\n' "$HAVE_DISPLAYS" | grep -qx "$d"; then
            DISPLAY_BACKEND="$d"
            break
        fi
    done
    DISPLAY_BACKEND="${DISPLAY_BACKEND:-none}"
fi
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
# The firmware mounts /dev/hda1 on /media/hdd, and without a drive its sysmon
# applet dies in setup() and is restarted forever, so the interface never comes
# up and never speaks. Create the drive on demand; qcow2 keeps a 60GB disk down
# to a couple of hundred KB until the guest writes to it.
if [ ! -e "$HDD" ]; then
    echo "Creating the internal 60GB drive at $HDD"
    python3 "$REPO_ROOT/tools/bpimage.py" mkdisk "$HDD"
    echo "It is blank, so the device will offer to format it on first boot."
fi

# The real hardware's WiFi and Bluetooth are a proprietary combo chip that
# cannot be emulated. NET=1 attaches an RTL8150 USB ethernet adapter instead,
# which the firmware already has a driver for. The guest does not configure it
# on its own -- from the device's shell run:
#   ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up
#   route add default gw 10.0.2.2
NET_ARGS=""
if [ -n "${NET:-}" ]; then
    NET_ARGS="-netdev user,id=bpnet -device usb-rtl8150,netdev=bpnet"
fi

exec "$QEMU" \
    -M "everest,board-id=$BOARD_ID,keypad-id=$KEYPAD_ID" \
    -m "$RAM" \
    -kernel "$KERNEL" \
    -append "$APPEND" \
    -drive if=mtd,format=raw,file="$FLASH" \
    -drive if=ide,index=0,format=qcow2,file="$HDD" \
    $NET_ARGS \
    -audio "$AUDIO_DRV" \
    -serial mon:stdio \
    -display "$DISPLAY_BACKEND" \
    "$@"
