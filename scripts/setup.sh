#!/usr/bin/env bash
#
# Clone QEMU, graft the Everest board onto it, and build qemu-system-arm.
# Safe to re-run: the QEMU tree is only patched if it hasn't been already.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${QEMU_DIR:-$HOME/qemu}"
# PXA2xx support is deprecated upstream and disappears after this tag, so the
# board is pinned to the last release that still carries it.
QEMU_TAG="${QEMU_TAG:-v9.1.0}"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

if [ ! -d "$QEMU_DIR/.git" ]; then
    info "Cloning QEMU $QEMU_TAG into $QEMU_DIR"
    git clone --depth 1 --branch "$QEMU_TAG" \
        https://gitlab.com/qemu-project/qemu.git "$QEMU_DIR"
else
    info "Reusing existing QEMU tree at $QEMU_DIR"
fi

info "Installing board and device sources"
mkdir -p "$QEMU_DIR/include/hw/audio"
cp "$REPO_ROOT/qemu/hw/arm/everest.c"                    "$QEMU_DIR/hw/arm/everest.c"
cp "$REPO_ROOT/qemu/hw/audio/pxa2xx_ac97.c"              "$QEMU_DIR/hw/audio/pxa2xx_ac97.c"
cp "$REPO_ROOT/qemu/include/hw/audio/pxa2xx_ac97.h"      "$QEMU_DIR/include/hw/audio/pxa2xx_ac97.h"

MARKER='# >>> bpemu everest board >>>'

# Drop any block we appended on a previous run so re-running always lands the
# current version rather than stacking duplicates.
unpatch() {
    local f="$1"
    if grep -qF "$MARKER" "$f"; then
        sed -i "/$(printf '%s' "$MARKER" | sed 's/[][\.*^$\/]/\\&/g')/,\$d" "$f"
    fi
}

KCONFIG="$QEMU_DIR/hw/arm/Kconfig"
info "Adding CONFIG_EVEREST to hw/arm/Kconfig"
unpatch "$KCONFIG"
cat >> "$KCONFIG" <<EOF
$MARKER
config EVEREST
    bool
    default y
    depends on TCG && ARM
    select PXA2XX
    select ONENAND
    select PXA2XX_AC97
EOF

AUDIO_KCONFIG="$QEMU_DIR/hw/audio/Kconfig"
info "Adding CONFIG_PXA2XX_AC97 to hw/audio/Kconfig"
unpatch "$AUDIO_KCONFIG"
cat >> "$AUDIO_KCONFIG" <<EOF
$MARKER
config PXA2XX_AC97
    bool
EOF

MESON="$QEMU_DIR/hw/arm/meson.build"
info "Adding everest.c to hw/arm/meson.build"
unpatch "$MESON"
cat >> "$MESON" <<EOF
$MARKER
arm_ss.add(when: 'CONFIG_EVEREST', if_true: files('everest.c'))
EOF

AUDIO_MESON="$QEMU_DIR/hw/audio/meson.build"
info "Adding pxa2xx_ac97.c to hw/audio/meson.build"
unpatch "$AUDIO_MESON"
cat >> "$AUDIO_MESON" <<EOF
$MARKER
system_ss.add(when: 'CONFIG_PXA2XX_AC97', if_true: files('pxa2xx_ac97.c'))
EOF

if [ ! -f "$QEMU_DIR/build/build.ninja" ]; then
    info "Configuring QEMU"
    mkdir -p "$QEMU_DIR/build"
    (cd "$QEMU_DIR/build" && ../configure \
        --target-list=arm-softmmu \
        --enable-slirp \
        --audio-drv-list=pa,alsa \
        --disable-docs \
        --disable-werror)
fi

info "Building qemu-system-arm"
(cd "$QEMU_DIR/build" && ninja -j"$(nproc)")

info "Done: $QEMU_DIR/build/qemu-system-arm"
"$QEMU_DIR/build/qemu-system-arm" -M help | grep -i everest || {
    echo "ERROR: the everest machine did not register" >&2
    exit 1
}
