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

# Works both on Linux/WSL and in an MSYS2 MINGW64 shell, where the host audio
# backends and the binary suffix differ.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        AUDIO_DRVS="${AUDIO_DRVS:-dsound,sdl}"
        EXE=.exe
        # Build against the MinGW-w64 toolchain, not MSYS2's Cygwin-like one,
        # so the result is a native Windows binary with no runtime dependency
        # on MSYS2. This also works when the shell was started without
        # MSYSTEM=MINGW64.
        if [ -d /mingw64/bin ]; then
            export PATH="/mingw64/bin:$PATH"
        fi
        ;;
    *)
        AUDIO_DRVS="${AUDIO_DRVS:-pa,alsa}"
        EXE=
        ;;
esac

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

if [ -n "$EXE" ]; then
    # QEMU's configure builds a "qemu-bundle" view of the install tree out of
    # symlinks. Windows refuses those without Developer Mode or Administrator
    # rights, and the failure aborts meson setup entirely. Enabling Developer
    # Mode is the upstream answer; since that needs admin and is a system-wide
    # setting, fall back to copying instead -- the tree is only a convenience
    # view, so a copy serves just as well.
    SIT="$QEMU_DIR/scripts/symlink-install-tree.py"
    if [ -f "$SIT" ] && ! grep -q 'bpemu copy fallback' "$SIT"; then
        info "Teaching symlink-install-tree.py to copy when it may not symlink"
        python3 - "$SIT" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path, encoding='utf-8').read()

helper = '''import shutil

def _link_or_copy(source, dest):
    """bpemu copy fallback for hosts that disallow symlinks.

    This tree is built at configure time, so many entries point at build
    outputs that do not exist yet. A symlink is happy to dangle; a copy is
    not, so those are skipped -- they are produced by the build itself and
    nothing needs them via this convenience view. Data files that already
    exist in the source tree are copied normally.
    """
    try:
        os.symlink(source, dest)
    except OSError as e:
        if e.errno == errno.EEXIST:
            raise
        if not os.path.exists(source):
            return
        if os.path.isdir(source):
            shutil.copytree(source, dest, dirs_exist_ok=True, symlinks=True)
        else:
            shutil.copy2(source, dest)


'''

src = src.replace("introspect = os.environ.get",
                  helper + "introspect = os.environ.get", 1)
src = src.replace("        os.symlink(source, bundle_dest)",
                  "        _link_or_copy(source, bundle_dest)", 1)
open(path, 'w', encoding='utf-8', newline='\n').write(src)
PYEOF
    fi
fi

# The Everest keypad driver debounces in software: a key is only accepted once
# it has been seen still pressed on a *later* scan, timed against jiffies. Real
# PXA27x hardware in automatic-scan mode keeps re-interrupting for as long as
# any key is down, so that second observation always arrives. QEMU's model only
# raises the interrupt when the matrix state *changes*, so the state machine
# never advances and no key ever reaches userspace -- the interrupt count climbs
# but /dev/input/event0 stays silent. Give the model the periodic rescan that
# auto-scan mode implies.
KEYPAD="$QEMU_DIR/hw/input/pxa2xx_keypad.c"
if [ -f "$KEYPAD" ] && ! grep -q 'bpemu auto-scan' "$KEYPAD"; then
    info "Adding auto-scan rescan interrupts to hw/input/pxa2xx_keypad.c"
    python3 - "$KEYPAD" <<'PYEOF'
import sys

path = sys.argv[1]
src = open(path, encoding='utf-8').read()

src = src.replace('#include "ui/console.h"',
                  '#include "ui/console.h"\n#include "qemu/timer.h"', 1)

src = src.replace("""    uint32_t    kpkdi;
};""",
                  """    uint32_t    kpkdi;

    /* bpemu auto-scan: periodic rescan while keys are held. */
    QEMUTimer   *scan_timer;
};

/*
 * Interval between simulated automatic scans. The real controller scans
 * continuously; anything comfortably shorter than a human keypress will do,
 * and this keeps guest-visible debounce timers advancing.
 */
#define PXA2XX_KEYPAD_SCAN_PERIOD_NS (10 * SCALE_MS)

static void pxa27x_keypad_scan(void *opaque)
{
    PXA2xxKeyPadState *s = opaque;

    if (!s->pressed_cnt || !(s->kpc & KPC_ME) ||
        !(s->kpc & (KPC_AS | KPC_ASACT))) {
        return;
    }

    if (s->kpc & KPC_MIE) {
        s->kpc |= KPC_MI;
        qemu_irq_raise(s->irq);
    }
    timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              PXA2XX_KEYPAD_SCAN_PERIOD_NS);
}

static void pxa27x_keypad_scan_update(PXA2xxKeyPadState *s)
{
    if (s->pressed_cnt && (s->kpc & KPC_ME) &&
        (s->kpc & (KPC_AS | KPC_ASACT))) {
        timer_mod(s->scan_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  PXA2XX_KEYPAD_SCAN_PERIOD_NS);
    } else {
        timer_del(s->scan_timer);
    }
}""", 1)

# Keep the rescan timer in step with every matrix state change.
src = src.replace("""    if (assert_irq && (kp->kpc & KPC_MIE)) {
        kp->kpc |= KPC_MI;
        qemu_irq_raise(kp->irq);
    }""",
                  """    if (assert_irq && (kp->kpc & KPC_MIE)) {
        kp->kpc |= KPC_MI;
        qemu_irq_raise(kp->irq);
    }

    pxa27x_keypad_scan_update(kp);""", 1)

src = src.replace("""    s = g_new0(PXA2xxKeyPadState, 1);
    s->irq = irq;""",
                  """    s = g_new0(PXA2xxKeyPadState, 1);
    s->irq = irq;
    s->scan_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pxa27x_keypad_scan, s);""",
                  1)

open(path, 'w', encoding='utf-8', newline='\n').write(src)
PYEOF
fi

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
        --audio-drv-list="$AUDIO_DRVS" \
        --disable-docs \
        --disable-werror)
fi

info "Building qemu-system-arm$EXE"
if [ -n "$EXE" ]; then
    # Build only the emulator. QEMU 9.1's test binaries do not link against
    # current mingw-w64 runtimes (undefined reference to qemu_ftruncate64),
    # and a full build would fail on them for no benefit here.
    (cd "$QEMU_DIR/build" && ninja -j"$(nproc)" "qemu-system-arm$EXE")
else
    (cd "$QEMU_DIR/build" && ninja -j"$(nproc)")
fi

info "Done: $QEMU_DIR/build/qemu-system-arm$EXE"
"$QEMU_DIR/build/qemu-system-arm$EXE" -M help | grep -i everest || {
    echo "ERROR: the everest machine did not register" >&2
    exit 1
}
