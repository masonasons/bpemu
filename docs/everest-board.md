# The "Everest" board, as recovered from firmware

Everything here was derived by static analysis of the stock `2.2.53` firmware
bundle. No hardware, schematic or vendor documentation was consulted, so each
claim below is followed by how it was established — if you have a real unit and
find a discrepancy, the derivation is the thing to re-check.

## Identity

| Property | Value | How it was found |
|---|---|---|
| Marketing names | LevelStar Icon, APH Braille+ Mobile Manager | `Machine:` banner |
| Board name | `LevelStar Icon (Everest Board)` | `machine_desc.name` |
| ARM machine type | **1150** (`0x47e`) | `machine_desc.nr` |
| SoC | Marvell/Intel PXA270 (XScale, ARMv5TE) | `CPU: XScale-PXA270` |
| Kernel | Linux 2.6.31, gcc 3.4.4, PREEMPT | version banner |
| Build system | OpenEmbedded / Poky ("OpenedHand Linux (Poky) 3.1") | `/etc/issue` |
| Userland | Python 2.6 application stack + Eloquence ECI TTS | `/etc/StartShell` |

The kernel is a plain ARM `zImage`: gzip payload at file offset `0x3404`,
decompressing to a 4,153,632-byte image that loads at `0xc0008000`.

The `machine_desc` record sits at file offset `0x15a8c` in the decompressed
image:

```
nr           0x0000047e   (1150)
phys_io      0x40000000
io_pg_offst  0x00003c80   -> virtual 0xf2000000
name         0xc037d250   -> "LevelStar Icon (Everest Board)"
boot_params  0xa0000100
map_io       0xc000ba8c
init_irq     0xc000c0f4
init_machine 0xc000c244   (everest_init)
```

## Memory map

`everest_map_io()` installs the stock five-entry PXA table via `iotable_init()`
(array at `0xc0020180`):

| Virtual | Physical | Size | Meaning |
|---|---|---|---|
| `0xf2000000` | `0x40000000` | 32 MiB | PXA peripherals |
| `0xf6000000` | `0x48000000` | 2 MiB | PXA memory controller |
| `0xfa000000` | `0x50000000` | 1 MiB | |
| `0xfe000000` | `0x58000000` | 1 MiB | |
| `0xff000000` | `0x00000000` | 1 MiB | nCS0 boot flash window |

SDRAM is at `0xa0000000` with ATAGs at `0xa0000100`. RAM size is *not* recorded
anywhere in the firmware — the bootloader passes it in ATAGs, and we do not have
a readable bootloader. The emulator defaults to 64 MiB, which is the common
PXA270 single-bank population and boots the stock image fine; treat it as an
assumption, not a measurement.

## Boot flash: OneNAND on nCS0

The kernel registers a platform device named `onenand` (struct at `0xc03d6b74`)
with three resources at `0xc03d6c30`:

| # | Type | Range |
|---|---|---|
| 0 | `IORESOURCE_MEM` | `0x00000000`–`0x03ffffff` (nCS0, 64 MiB window) |
| 1 | `IORESOURCE_MEM` | `0x04000000`–`0x07ffffff` (nCS1, 64 MiB window) |
| 2 | `IORESOURCE_IRQ` | 84 — i.e. GPIO 20, since Linux 2.6.31 maps `IRQ_GPIO(x) = 64 + x` |

Three independent lines of evidence agree on OneNAND rather than raw NAND:

1. The PXA270 has **no NAND controller**. A NOR-like OneNAND part is the only
   thing that drops straight onto the static memory bus, and the static mapping
   of nCS0 above is exactly the window it needs.
2. The shipped `root.bin` JFFS2 image has a **128 KiB erase block** (measured
   by walking the node chain and finding the resync boundaries: 412 gaps at
   `0x20000`) and carries **no inline cleanmarkers** — i.e. they live in OOB.
   That is OneNAND geometry: 2 KiB page, 64 B OOB, 128 KiB block.
3. The kernel carries the full OneNAND stack including `flexonenand`.

The emulated part is a 128 MiB Samsung-style device (`device_id = 0x30`, since
QEMU derives size as `1 << (24 + ((id >> 4) & 7))`). The real capacity is not
recorded in the firmware; 128 MiB comfortably holds the 55 MiB root image, and
64 MiB would only just do so.

### Partition map

The real map lives in the bootloader, which is not readable, so the emulator
supplies its own via `mtdparts=` and both ends therefore agree. The *ordering*
is pinned by the firmware itself:

- `levelstar/update/update.py` writes `bootld.bin` to `/dev/mtdblock0` and
  the kernel to partition 2 when there are more than three partitions.
- `levelstar/update/app.py`, `installer/reg.py` and `sysmon/wireless.py` all
  read the serial number and MAC out of `/dev/mtdblock1`.
- `/etc/fstab` mounts `/dev/mtdblock3` as the root filesystem.

So the shipped device has four partitions in the order
bootloader, params, kernel, root:

```
mtdparts=onenand:1024k(bootloader),1024k(params),4096k(kernel),-(root)
```

Note the kernel's *compiled-in* command line says `root=/dev/mtdblock2`, which
corresponds to the older three-partition Icon layout. `/etc/fstab` in this
2.2.53 image says `mtdblock3`, so the shipped bootloader passes the
four-partition map. We follow `/etc/fstab`.

### The params partition

`mtd1` is a small parameter block, read directly as bytes:

| Offset | Length | Contents |
|---|---|---|
| 0 | 12 | ASCII serial number. A leading `1IBP1` makes the firmware report the manufacturer as "The American Printing House for the Blind"; anything else reads as "LevelStar". |
| 16 | 6 | Wireless MAC. `sysmon/wireless.py` expects OUI `00:11:d6` or `00:50:c2:a4`, otherwise it invents one and caches it in `/etc/MAC.conf`. |

## Board straps (GPIO)

`everest_init()` reads **GPLR3** (`0x40e00100`, GPIO 96–127) once and splits it
into two straps:

```
board_id  = (GPLR3 >> 7) & 0xf                        -> GPIO 103,104,105,106
keypad_id = ((GPLR3 >> 11) & 3) | ((GPLR3 & 1) << 3)  -> GPIO 107,108 and 96
```

Note that bit 2 of `keypad_id` has no GPIO behind it and can never be set.
Both values are exported at `/proc/everest/board_id` and
`/proc/everest/keypad_id`, and `sysmon/wireless.py` reads `board_id` at import
time. `everest_init()` branches on `board_id > 1`, so the value is not
cosmetic; the emulator defaults to 2 and exposes both as machine properties
(`-M everest,board-id=N,keypad-id=N`) because the correct values for a specific
unit are not recoverable from firmware alone.

`everest_init()` also requests GPIOs 103–108 and 96 by name, and creates
`/proc/everest` entries named `board_id`, `keypad_id`, `sd_card_present`,
`bt_power`, `wifi_power`, `uart_enable` and `usb_wake`.

### SD card detect

The `/proc/everest/sd_card_present` handler at `0xc002e888` reads GPLR0 and
returns `(GPLR0 >> 4) & 1`, so **card detect is GPIO 4**. Left at QEMU's default
of 0 the MMC core believes a card is present and retries forever, flooding the
console with `everest_power_mci` / `everest_power_off_mci`. The emulator drives
it high for an empty slot. (GPIOs 95 and 19 are the MMC power-rail controls,
toggled by the `everest_power_mci` helper at `0xc002e8bc`.)

## Custom peripherals

`everest_init()` registers platform devices named `ac97`, `battery`, `motor`
and `mmc_spi`, plus the `onenand` device above. Their drivers announce
themselves on boot:

```
Everest Keypad driver loadee.        <- sic, typo is in the firmware
Everest battery driver loaded.
Everest motor driver loaded.
Everest IDE interface at 0xc48e0000  irq:155
```

- **Keypad** — a board-specific driver that registers input device
  `everestkeypad/input0`, named "Everest keypad.". It drives the **PXA27x
  keypad controller (KPC) at `0x41500000`** rather than bit-banging GPIOs. The
  literal `0x37dff800` written to that address decodes cleanly against the KPC
  bit layout:

  | Bits | Field | Value |
  |---|---|---|
  | 29 | `KPC_ASACT` | set — automatic scan on activity |
  | 28:26 | `KPC_MKRN` | 5 → **6 matrix rows** |
  | 25:23 | `KPC_MKCN` | 7 → **8 matrix columns** |
  | 12 | `KPC_ME` | set — matrix keypad enabled |
  | 11 | `KPC_MIE` | set — matrix interrupt enabled |

  A sibling constant `0x3bdff800` (same, plus `KPC_AS`) sits in the driver's
  literal pool. So the keypad is an auto-scanned **6 × 8 matrix**, 48 positions
  — a plausible size for a Perkins keyboard plus function and navigation keys.

  The driver looks keycodes up in a table of stacked 48-entry `u32` arrays at
  `0xc03f2dac`, indexed `keycodes[variant * 48 + row * 8 + col]`. The `variant`
  is a global selected at runtime by writing `'0'` or `'1'` to a sysfs node
  (`'1'` picks array 1 or 2 depending on `keypad_id`); it is 0 at boot, and the
  probe only ever declares array 0's keycodes to the input core.

  **Array 0** — the boot default — decodes to a telephone keypad plus
  navigation and media keys:

  |      | col0 | col1 | col2 | col3 | col4 | col5 | col6 |
  |------|------|------|------|------|------|------|------|
  | row0 | `KPASTERISK` | `BTN_7` | `BTN_4` | `BTN_1` | `OK` | `INFO` | `PROG1` |
  | row1 | `BTN_0` | `BTN_8` | `BTN_5` | `BTN_2` | `MENU` | `SELECT` | `DOWN` |
  | row2 | – | – | – | `BTN_RIGHT` | `BTN_LEFT` | `UP` | `PROGRAM` |
  | row3 | `KPDOT` | `BTN_9` | `BTN_6` | `BTN_3` | `CANCEL` | `HELP` | `PROG2` |
  | row4 | `VOLUMEUP` | `VOLUMEDOWN` | `MUTE` | `RECORD` | – | – | – |
  | row5 | – | – | – | – | – | – | – |

  Column 7 and row 5 are unpopulated. `BTN_0`–`BTN_9` are the digit keys: the
  digits form clean columns (1/2/3 in col3, 4/5/6 in col2, 7/8/9 in col1,
  `*`/`0`/`#` in col0), which is what confirms the `row * 8 + col` indexing
  rather than the transpose.

  **Array 1** remaps those same physical keys to values in the `0x600` range,
  which the driver detects by testing bit `0x200` and routes separately — these
  are the braille dots, i.e. chorded braille entry on the twelve-key pad.

  **Array 2** is array 1 plus a sixth row, and is selected whenever `keypad_id`
  is non-zero. That row matters far more than its size suggests: it holds
  space, shift and control, none of which appear anywhere in arrays 0 or 1.
  Pick the wrong variant and the machine cannot type a space or a capital
  letter, which is why the board defaults `keypad-id` to 1.

  Those three carry `0x600`-range values just like the dots, so it is natural
  to read them as dots 7 and 8 plus a blank cell — the more so because the
  application defines `KEY_BRAILLE_DOT1..DOT8` followed immediately by
  `KEY_BRAILLE_SPACE`. That reading is wrong. Injecting each key and watching
  `/dev/input/event0` shows what the driver really emits:

  | Matrix | Value | Emits |
  |---|---|---|
  | row5 col3 | `0x606` | `KEY_LEFTSHIFT` |
  | row5 col4 | `0x607` | `KEY_SPACE` |
  | row5 col2 | `0x608` | `KEY_LEFTCTRL` |

  So the encoding order is not the obvious one, and this keypad is six-dot
  braille plus modifiers rather than eight-dot.

### The key lock switch gates the entire keypad (GPIO 93)

Wiring the matrix up via `pxa27x_register_keypad()` is necessary but not
sufficient, and the missing piece was not in the keypad at all.

The keypad driver's per-key routine at `0xc01e36e8` opens with:

```
ldrb lr, [r0, #52]     ; a gating byte in the driver's private state
cmp  lr, #0
bne  0xc01e39dc        ; non-zero -> return immediately, report nothing
```

The probe initialises that byte to **2** — neither of its two valid values —
and then samples the key lock switch (`0xc0017354` calls the same worker the
switch's interrupt handler uses), which derives it from GPLR2 bit 29:

```
tst r3, #0x20000000    ; GPIO 93
movne r4, #0           ; pin HIGH -> 0, keypad enabled
moveq r4, #1           ; pin LOW  -> 1, keys locked
strb r4, [r5, #52]
```

So **GPIO 93 must be high or the keypad is dead**. Left at QEMU's default of
low, every keypress was silently dropped: interrupts arrived (the `Keypad`
count on IRQ 4 rose by exactly two per press) and the driver read the matrix
correctly, but nothing ever reached `/dev/input`. The board now drives GPIO 93
high, with a `key-lock=on` machine property to engage the switch.

The GPIO numbering is self-consistent: `/proc/interrupts` lists IRQ 68 as
"MMC card detect", and 68 − 64 = GPIO 4, which independently matches the
`sd_card_present` handler reading `GPLR0 >> 4`. On the same base, IRQ 157
("key lock") is GPIO 93.

A rejected hypothesis, recorded so nobody repeats it: the per-key routine
consults `jiffies` and per-key state, which looks like a software debounce, so
it seemed plausible that the driver needed to see a key still pressed on a
*later* scan — real PXA27x auto-scan hardware re-interrupts continuously while
a key is held, whereas QEMU's model only fires on state changes. Patching
`hw/input/pxa2xx_keypad.c` to rescan every 10 ms drove the interrupt count to
183 for three keys and still produced no events. That patch was reverted.

### Which keycode array is live

Injecting a key at matrix position 3 reports **362** (`KEY_PROGRAM`), which is
`array1[3]`, not `array0[3]` (`BTN_1`). So the running firmware selects
**array 1**, even though array 0 is the boot default and the only array whose
keycodes the probe declares to the input core.

Array 1 is also the fuller layout, and the only one containing the six braille
dots — so it is very likely the real physical keypad:

|      | col0 | col1 | col2 | col3 | col4 | col5 | col6 |
|------|------|------|------|------|------|------|------|
| row0 | `PROG1` | `DOT3` | `INFO` | `PROGRAM` | `PROG2` | `HELP` | `CANCEL` |
| row1 | `KPDOT` | `DOT2` | `BTN_LEFT` | `UP` | `SELECT` | `DOWN` | `BTN_RIGHT` |
| row2 | `BTN_0` | `DOT1` | `BTN_1` | `BTN_2` | `BTN_3` | `OK` | `MENU` |
| row3 | `KPASTERISK` | `DOT4` | `BTN_4` | `RECORD` | `MUTE` | `VOLUMEDOWN` | `VOLUMEUP` |
| row4 | `DOT6` | `DOT5` | `BTN_5` | `BTN_6` | `BTN_7` | `BTN_8` | `BTN_9` |

The mapping was confirmed end to end by injecting keys and decoding the
resulting `input_event` structs — six positions, six exact matches:

| Matrix index | Expected `array1[i]` | Reported |
|---|---|---|
| 3 | `PROGRAM` (362) | 362 |
| 2 | `INFO` (358) | 358 |
| 1 | `DOT3` (0x602) | **40 — apostrophe** |
| 8 | `KPDOT` (83) | 83 |
| 4 | `PROG2` (149) | 149 |
| 29 | `VOLUMEDOWN` (114) | 114 |

The dot-3 result is the most telling. Dot keys carry keycodes in the `0x600`
range, which the driver detects by testing bit `0x200` and routes into the
kernel's braille chord assembler rather than reporting directly — and dot 3
alone *is* an apostrophe in braille. Chorded braille entry works.
## Audio: PXA27x AC97 + WM9713

QEMU's PXA2xx model has an I2S unit but has never had an AC97 one, and there
was no WM9713 model either, so `hw/audio/pxa2xx_ac97.c` adds both. This is not
optional decoration on this machine: the entire user interface is spoken
through Eloquence, so without audio the firmware boots mute.

- Controller at `0x40500000`, interrupt 14.
- DMA request lines, per Linux's `sound/soc/pxa/pxa2xx-ac97.c`: PCM out on
  request 12, PCM in on 11, aux out 10, aux in 9, mic in 8.
- Codec registers are reached through windows at `+0x200` (primary audio),
  `+0x300` (primary modem), `+0x400` and `+0x500` (secondary); AC97 register
  `n` appears at `window + (n >> 1) * 4`.
- Reset handshake: Linux clears GCR entirely and then writes
  `GCR_COLD_RST | GCR_WARM_RST`, and waits for `GSR_PCR` to indicate the primary
  codec is ready. A read from a codec window must set `GSR_SDONE`, a write must
  set `GSR_CDONE`; GSR is write-1-to-clear.
- Only a primary audio codec answers. Leaving `GSR_SDONE` clear for the other
  windows is how the driver learns nothing else is fitted.

Success looks like this on the console:

```
WM9713/WM9714 SoC Audio Codec 0.15
asoc: AC97 HiFi <-> pxa2xx-ac97 mapping ok
asoc: AC97 Aux <-> pxa2xx-ac97-aux mapping ok
ALSA device list:
  #0: Everest (WM9713)
```

### The device speaks through the Aux DAC, not the HiFi one

This is the single least guessable thing about the machine's audio, and it cost
a long debugging session, so it is worth stating plainly.

The WM9713 has **two** DACs, and ALSA exposes them as two PCM devices:

```
card 0: Everest [Everest], device 0: AC97 HiFi     -> PCDR, DMA request 12
card 0: Everest [Everest], device 1: AC97 Aux      -> MODR, DMA request 10
```

GStreamer sound effects (`.ogg`, `.wav`) play through the stereo **HiFi** DAC.
**Eloquence speech plays through the mono Aux DAC**, which is what that DAC is
for. Model only the HiFi path -- as this board did at first, discarding `MODR`
with a `break;` -- and the result is a machine that plays every sound effect
perfectly while never speaking a word. That symptom looks exactly like a broken
speech engine, and it is not.

Two measurements settle it quickly if it ever regresses:

- Play the same clip to `plughw:0,0` and then `plughw:0,1`. If only one
  playback's worth reaches the host, the Aux path is missing.
- Call Eloquence directly with `ctypes` (`eciNew`, `eciSetOutputFilename`,
  `eciAddText`, `eciSynthesize`). It reports version 6.1.0.0 and writes a wav
  quite happily, which rules the engine out in one step. There is no licence
  check standing in the way, despite `libeci.so` exporting `eciRequestLicense`.

Each 32-bit write to `MODR` carries **one** mono sample in its low half. It is
tempting to read it as two packed S16 samples, since Linux configures that DMA
with `DCMD_WIDTH4` over what looks like a linear mono buffer, but that doubles
the stream -- measured as 293936 samples for a clip that should be about
145440.

Rates are not the interesting part, contrary to first instinct: VRA is enabled
(`AC97_EXTENDED_STATUS` reads `0x0411`), the HiFi voice reopens itself at
11025Hz for speech-rate content and times correctly, and the Aux path's rate
register genuinely reads 48000 because ALSA resamples before handing samples
over.

Set `BPEMU_AC97_DEBUG=1` to have the model report the sample count and rate of
each playback burst, which is the fastest way to catch this class of bug.

### A QEMU DMA quirk worth knowing

Real hardware holds the AC97 DMA request at a *level* and the DMA engine
samples it. QEMU's `pxa2xx-dma` instead latches request *edges*, and
`pxa2xx_dma_request()` drops any edge that arrives before the guest has set
`DRCMR_MAPVLD` for that channel — which is exactly when the controller first
raises it, during codec bring-up. Nothing re-raises it afterwards, so the
channel never starts and guest writes fail with `-EIO`. The controller
therefore re-presents the request level on a 2 ms timer while the AC-link is
up; once samples flow, the PCDR writes and the audio callback keep the line
current and the timer merely idles.

## Networking: an RTL8150 on the OHCI bus

The real unit's wireless is a Stonestreet One BGW200 combo part on SPI with a
binary-only driver, and QEMU dropped its Bluetooth stack in 5.0, so neither can
be modelled. What the firmware *does* carry is a driver for the Realtek RTL8150
USB ethernet chip, and the PXA270's OHCI controller is already wired up, so
attaching one gives the guest a working network with no firmware changes:

```
-netdev user,id=bpnet -device usb-rtl8150,netdev=bpnet
```

The device model lives in `hw/usb/dev-rtl8150.c`. Registers sit at 0x0120-0x014f
with bulk IN on endpoint 1, bulk OUT on 2 and an interrupt IN on 3, matching the
`usb_rcvbulkpipe(1)` / `usb_sndbulkpipe(2)` / `usb_rcvintpipe(3)` calls in
`rtl8150_open()`. Three things about it are worth recording, because each one
produced a symptom that pointed somewhere else entirely.

### Self-clearing bits

`CR_SOFT_RESET` (0x10 in CR) and `PHY_GO` (0x40 in PHYCNT) are self-clearing on
real silicon. A model that stores register writes verbatim hangs the driver:
`rtl8150_reset()` spins on the reset bit until it times out and probe fails with
`-EIO`, and `read_mii_word()` spins on `PHY_GO` forever. This is a recurring
shape in this codebase -- the same trap appears in the AC97 model.

### The carrier is decided twice, by two different registers

`rtl8150_open()` calls `set_carrier()`, which reads **CSCR** and tests
`CSCR_LINK_STATUS` (1<<3). But `intr_callback()`, which runs on every poll of
the interrupt endpoint, reads byte 2 of the 8-byte payload as **MSR** and calls
`netif_carrier_off()` whenever `MSR_LINK` is clear. Both have to say "up".

`MSR_LINK` is `1<<2`, `MSR_SPEED` is `1<<3`, `MSR_DUPLEX` is `1<<4` -- low bits,
not high ones. Reporting a plausible-looking `0xe0` there sets three unrelated
bits and leaves link clear, so the carrier is switched on by `set_carrier()` and
switched straight back off by the first interrupt poll. The interface then comes
up `BROADCAST MULTICAST` without `RUNNING`, the stack never transmits, and every
ping is lost with nothing in the logs to explain it.

### A bulk transfer is not a frame

This is the one that cost the most time. A bulk transfer arrives as a run of
`wMaxPacketSize` (64 byte) packets terminated by a short one, and the device
handler is called once per *packet*, not once per transfer. Calling
`qemu_send_packet()` on each one chops every frame over 64 bytes into a
truncated head plus a stray fragment:

```
 3 len=  60 ARP           <- fits in one packet, works
 4 len=  64 IPv4          <- a 98-byte ICMP echo,
 5 len=  34 (junk)        <- in two pieces
```

ARP requests are 60 bytes, so address resolution succeeded and its reply came
back, while every ping was silently dropped by the far side. The result looks
exactly like a *receive* fault, and the receive path is where the time went.
`hw/usb/dev-network.c` shows the correct idiom: accumulate into a buffer and
send when a short packet arrives. `rtl8150_start_xmit()` pads runts to 60 and
appends one byte when the length would otherwise be a multiple of 64, which is
precisely what guarantees the terminating packet is always short.

The same segmentation applies in reverse. Copying a whole frame into one packet
trips the assert in `usb_packet_copy()` as soon as anything larger than 64 bytes
arrives, so receive has to be metered out per packet too -- with a zero-length
packet to terminate a transfer that is a whole number of packets, as the silicon
would send.

Verified with `ping` at 56, 82 and 1400 byte payloads; the 82-byte case is the
one that makes frame plus status word exactly 128 bytes and exercises the
zero-length terminator.

## Boot sequence

```
/etc/inittab  id:5:initdefault
              si::sysinit:/etc/init.d/rcS
              S:2345:respawn:/sbin/getty 115200 ttyS0
              5:5:once:/etc/StartShell
/etc/StartShell
              PYTHONOPTIMIZE=2, ECIINI=/etc/eci.ini
              loadkeys /etc/everest.keymap.gz
              modprobe ide-gd_mod pxa2xx-ide uinput
              python -m levelstar.sysmon.launcher
              dbus-launch shell
```

Note that `getty` and the application share `ttyS0`, so on the emulator the
login prompt and the application's own stdout interleave on the same console.
Root has an empty password (`root::0:0:root:/home/root:/bin/sh`).

## Known gaps

- **RAM size and OneNAND capacity are assumptions**, not measurements; both are
  supplied by a bootloader we cannot read.
- **The partition sizes are ours.** Only the ordering is attested by firmware.
- The vibration motor is not modelled. The battery is, through the WM9713
  digitiser, and reads a steady 4.0V unless `battery-mv` says otherwise.
- **WiFi and Bluetooth cannot be emulated.** The BGW200 is a proprietary
  Stonestreet One combo part on SPI with a binary-only driver, and QEMU removed
  its Bluetooth stack in 5.0. An RTL8150 USB ethernet adapter stands in; see
  the networking section.
- `dbus-launch` fails because it tries to autolaunch via X11, so the launcher
  logs `GConf Error: No D-BUS daemon running` in a loop. The firmware ships
  `Xfbdev`/`Xfake`; a headless device presumably never needed a working session
  bus, but this has not been traced.
- Capture (microphone / line in) reads silence.
- The `board_id` and `keypad_id` values a real unit straps are unknown.
