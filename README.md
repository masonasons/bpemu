# bpemu — an emulator for the APH Braille+ Mobile Manager

Boots the **real, unmodified** Braille+ / LevelStar Icon firmware in QEMU, with
working audio.

The Braille+ Mobile Manager (sold by the American Printing House for the Blind,
built by LevelStar as the "Icon") is a PXA270 handheld whose entire user
interface is spoken. This adds an `everest` machine type to QEMU that models the
board well enough for the stock `.lsi` firmware image to boot from its own
OneNAND flash, all the way to the Python application launcher, with speech and
sound effects coming out of your speakers.

```
Machine: LevelStar Icon (Everest Board)
everest_init...
Everest battery driver loaded.
Everest motor driver loaded.
Muxed OneNAND 128MB 1.8V 16-bit (0x30)
4 cmdlinepart partitions found on MTD device <NULL>
Everest Keypad driver loadee.
ALSA device list:
  #0: Everest (WM9713)
VFS: Mounted root (jffs2 filesystem) on device 31:3.
INIT: version 2.86 booting
...
OpenedHand Linux (Poky) 3.1 everest ttyS0
everest login:
```

Nothing in the guest is patched: the shipped `kernel.bin` and JFFS2 `root.bin`
run as they were flashed.

## What works

| | Status |
|---|---|
| Boot to userspace and a login shell | ✅ ~30 s under TCG once configured |
| OneNAND boot flash, 4 MTD partitions, JFFS2 root read-write | ✅ |
| Audio output (AC97 + WM9713) | ✅ new device models |
| Python application launcher starts | ✅ |
| Serial console, networking, SSH, Samba, Bluetooth stack | ✅ as far as the firmware takes them |
| Keypad input into the application | ❌ see [Limitations](#limitations) |
| Battery, vibration motor | ❌ drivers load, hardware unmodelled |
| Audio capture | ❌ reads silence |

QEMU has no PXA2xx AC97 controller and no WM9713 codec, so this repo adds both
(`qemu/hw/audio/pxa2xx_ac97.c`). On a device where every interaction is spoken,
that is the difference between an emulator and a curiosity.

## Requirements

Linux, or Windows with WSL2. You need a C toolchain and QEMU's build
dependencies:

```bash
sudo apt install git build-essential ninja-build pkg-config \
    libglib2.0-dev libpixman-1-dev python3-venv flex bison \
    libslirp-dev libasound2-dev libpulse-dev zlib1g-dev
```

You also need a firmware bundle — an `.lsi` file such as `2.2.53.lsi`. This
repo does not redistribute one.

## Quick start

```bash
./scripts/setup.sh                      # clone QEMU, graft the board on, build
./scripts/build-image.sh /path/to/2.2.53.lsi
./scripts/run.sh                        # boot it
```

`setup.sh` is safe to re-run; it re-applies the board sources and rebuilds.
Log in as `root` (no password).

**The first boot of a freshly built image is slow** — several minutes, because
the firmware runs its one-time `ipkg-cl configure` pass, rebuilds the MIME
database and generates SSH host keys. That state is written back to the JFFS2
root, so later boots of the same `flash.img` reach a login prompt in well under
a minute. Keep the image around rather than rebuilding it each time, and use
QEMU snapshots (`savevm`/`loadvm` from the monitor) to skip the boot entirely.

To boot without audio, or with a different backend:

```bash
AUDIO_DRV=alsa ./scripts/run.sh
AUDIO_DRV=none ./scripts/run.sh
```

To capture what the device says to a file instead of your speakers:

```bash
AUDIO_DRV='driver=wav,path=/tmp/braille.wav' ./scripts/run.sh
```

## Driving it non-interactively

`tools/autoboot.py` boots the machine, waits for the login prompt, and runs
shell commands — useful as a smoke test or for poking at the firmware without
sitting through a boot:

```bash
python3 tools/autoboot.py \
    --kernel build/parts/kernel.bin --flash build/flash.img \
    --audio 'driver=wav,path=/tmp/out.wav' \
    -c 'aplay -l' \
    -c 'cat /proc/mtd' \
    -c 'dd if=/dev/urandom bs=1024 count=200 | aplay -f cd -'
```

That last command is the end-to-end audio test: it should exit 0 and leave
about 200 KiB of 44.1 kHz stereo PCM in `/tmp/out.wav`. Expect the capture to
come up ~10 KiB short of what you sent — that is the audio still in flight in
the FIFO when QEMU is killed, not lost samples. Add a trailing `-c 'sleep 2'`
if you want it flushed.

Note the guest is BusyBox 1.2.1 from 2010; its `head` has no `-N` form and many
other options you would reach for are absent.

## Machine options

```
-M everest,board-id=N,keypad-id=N
```

Both are hardware straps the firmware reads from GPIO and exposes under
`/proc/everest/`. `board_id` defaults to 2 and is not cosmetic — the board
setup code branches on `board_id > 1`. The values a real unit straps are not
recoverable from firmware, so they are exposed as knobs.

RAM defaults to 64 MiB (`-m`). See [docs/everest-board.md](docs/everest-board.md)
for why that, and the 128 MiB flash size, are assumptions rather than
measurements.

## Layout

```
qemu/hw/arm/everest.c              the board
qemu/hw/audio/pxa2xx_ac97.c        PXA27x AC97 controller + WM9713 codec
qemu/include/hw/audio/pxa2xx_ac97.h
scripts/setup.sh                   clone + patch + build QEMU
scripts/build-image.sh             .lsi -> OneNAND flash image
scripts/run.sh                     boot it
tools/bpimage.py                   unpack firmware, assemble flash images
tools/autoboot.py                  scripted console driver
docs/everest-board.md              what the hardware is, and how we know
```

The board sources live here rather than in a QEMU fork; `setup.sh` copies them
into a QEMU checkout and appends the Kconfig/meson entries, idempotently.

QEMU is pinned to **v9.1.0**, the last release that still carries PXA2xx
support — it is deprecated upstream and removed in later versions. That pin is
deliberate and is the main long-term maintenance risk for this project.

## Limitations

The honest ones, in rough order of how much they matter:

- **You cannot yet drive the UI.** The application takes input from the Linux
  console keyboard, fed by the Everest keypad driver. That driver appears to use
  the PXA27x keypad controller, which QEMU *already* models — so the missing
  piece is just the matrix keymap, not a new device. Until then the serial
  console gets you a shell, not the app.
- **RAM size, flash capacity and partition sizes are informed assumptions.**
  They come from a bootloader we cannot read. Partition *ordering* is attested
  by the firmware itself. See the docs for the derivations.
- `dbus-launch` fails (it tries to autolaunch through X11), so the launcher
  repeats `GConf Error: No D-BUS daemon running`. Not yet traced.
- Audio capture returns silence; battery reads absent; the vibration motor goes
  nowhere.
- The `.so` extension modules in the firmware are ARM binaries and stay that
  way — this is full-system emulation, so that is fine, but it is why a native
  port would be a very different project.

## Provenance

Every hardware constant was recovered by static analysis of the shipped
firmware — no schematics or vendor documentation. `docs/everest-board.md`
records each finding together with how it was established, so anything wrong is
traceable rather than folklore.

Useful companion material, if you have it: the `levelstar/` Python tree from a
device or SDK bundle is `uncompyle6` output of the same application, and is far
easier to read than the `.pyo` files in the firmware.

## Licence

The QEMU board and device sources under `qemu/` are GPL v2, matching QEMU.
