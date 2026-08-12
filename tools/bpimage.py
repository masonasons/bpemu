#!/usr/bin/env python3
"""
Flash-image tooling for the Braille+ / Icon "Everest" board emulator.

The stock firmware ships as a ``.lsi`` file, which is a plain zip containing
the bootloader, kernel and a JFFS2 root filesystem. This script unpacks one and
assembles a OneNAND image that QEMU's ``onenand`` device can serve.

QEMU's OneNAND backing image is *main data followed by spare data*: the OOB
region starts at ``main_size`` and holds 16 bytes per 512-byte sector, i.e.
64 bytes per 2KiB page, so the file is ``main_size * 33/32`` bytes total.

Usage:
    bpimage.py unpack <firmware.lsi> <outdir>
    bpimage.py mkflash <partsdir> <flash.img> [--size 128] [--serial 1IBP10000001]
    bpimage.py mtdparts
"""

import argparse
import os
import sys
import zipfile

MiB = 1024 * 1024

PAGE_SIZE = 2048
OOB_PER_PAGE = 64
ERASE_BLOCK = 128 * 1024

# Partition map. The real device's map lives in the bootloader, which we cannot
# read, but the layout is pinned at both ends by the firmware itself:
# levelstar/update/update.py treats mtd0 as the bootloader and mtd1 as the
# serial/MAC parameter block, and /etc/fstab mounts mtdblock3 as the root.
# The sizes below are ours; they are passed to the kernel via mtdparts= so the
# emulated device and the kernel always agree.
PARTITIONS = [
    # (name, size bytes or None for "rest", source file)
    ("bootloader", 1 * MiB, "bootld.bin"),
    ("params",     1 * MiB, None),
    ("kernel",     4 * MiB, "kernel.bin"),
    ("root",       None,    "root.bin"),
]


def mtdparts():
    """Kernel mtdparts= argument matching PARTITIONS."""
    out = []
    for name, size, _ in PARTITIONS:
        spec = "-" if size is None else "%dk" % (size // 1024)
        out.append("%s(%s)" % (spec, name))
    return "mtdparts=onenand:" + ",".join(out)


def unpack(lsi_path, outdir):
    os.makedirs(outdir, exist_ok=True)
    with zipfile.ZipFile(lsi_path) as z:
        names = z.namelist()
        z.extractall(outdir)
    print("Unpacked %s -> %s" % (lsi_path, outdir))
    for n in sorted(names):
        p = os.path.join(outdir, n)
        if os.path.isfile(p):
            print("  %-24s %10d bytes" % (n, os.path.getsize(p)))
    vfile = os.path.join(outdir, "version")
    if os.path.exists(vfile):
        with open(vfile) as f:
            print("Firmware version: %s" % f.read().strip())
    return 0


def make_params(serial, mac):
    """Build the mtd1 parameter block.

    levelstar/update/app.py reads 12 bytes of serial number at offset 0 and
    treats a leading '1IBP1' as an APH-branded unit; levelstar/sysmon/wireless.py
    reads a 6-byte MAC at offset 16 and expects a 00:11:d6 (LevelStar) or
    00:50:c2:a4 OUI, otherwise it invents one and caches it in /etc/MAC.conf.
    """
    blk = bytearray(b"\xff" * ERASE_BLOCK)
    ser = serial.encode("ascii")
    if len(ser) != 12:
        raise SystemExit("serial must be exactly 12 characters, got %d" % len(ser))
    blk[0:12] = ser
    octets = [int(x, 16) for x in mac.split(":")]
    if len(octets) != 6:
        raise SystemExit("mac must be six colon-separated hex octets")
    blk[16:22] = bytes(octets)
    return bytes(blk)


def mkflash(partsdir, out_path, size_mb, serial, mac):
    main_size = size_mb * MiB
    if main_size % ERASE_BLOCK:
        raise SystemExit("flash size must be a multiple of 128KiB")
    oob_size = main_size // 32

    main = bytearray(b"\xff" * main_size)

    offset = 0
    print("Building %d MiB OneNAND image" % size_mb)
    for name, size, src in PARTITIONS:
        part_size = main_size - offset if size is None else size
        if offset + part_size > main_size:
            raise SystemExit("partition %r does not fit in %d MiB" % (name, size_mb))

        if name == "params":
            data = make_params(serial, mac)
        elif src is None:
            data = b""
        else:
            path = os.path.join(partsdir, src)
            if not os.path.exists(path):
                raise SystemExit("missing %s (run 'bpimage.py unpack' first)" % path)
            with open(path, "rb") as f:
                data = f.read()

        if len(data) > part_size:
            raise SystemExit(
                "%s: %d bytes does not fit in %d byte partition"
                % (name, len(data), part_size))

        main[offset:offset + len(data)] = data
        print("  %-11s @ 0x%08x  %7d KiB  %8d bytes used  (%s)"
              % (name, offset, part_size // 1024, len(data), src or "generated"))
        offset += part_size

    with open(out_path, "wb") as f:
        f.write(main)
        # Spare/OOB area: all 0xff. JFFS2 keeps its cleanmarkers here, so the
        # kernel will simply erase and re-mark blocks it finds unmarked.
        f.write(b"\xff" * oob_size)

    print("Wrote %s (%d MiB main + %d MiB OOB = %d bytes)"
          % (out_path, size_mb, oob_size // MiB, main_size + oob_size))
    print(mtdparts())
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("unpack", help="extract a .lsi firmware bundle")
    p.add_argument("lsi")
    p.add_argument("outdir")

    p = sub.add_parser("mkflash", help="build a OneNAND image from unpacked parts")
    p.add_argument("partsdir")
    p.add_argument("out")
    p.add_argument("--size", type=int, default=128, help="flash size in MiB (default 128)")
    p.add_argument("--serial", default="1IBP10000001",
                   help="12-character serial number (default 1IBP10000001)")
    p.add_argument("--mac", default="00:11:d6:04:00:01",
                   help="wireless MAC address (default 00:11:d6:04:00:01)")

    sub.add_parser("mtdparts", help="print the kernel mtdparts= argument")

    args = ap.parse_args(argv)
    if args.cmd == "unpack":
        return unpack(args.lsi, args.outdir)
    if args.cmd == "mkflash":
        return mkflash(args.partsdir, args.out, args.size, args.serial, args.mac)
    if args.cmd == "mtdparts":
        print(mtdparts())
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
