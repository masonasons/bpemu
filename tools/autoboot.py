#!/usr/bin/env python3
"""
Drive the emulated Braille+ console non-interactively.

Boots the machine with its serial port on a pty, waits for the login prompt,
then runs a scripted list of shell commands. Used both as a smoke test and as a
way to poke at the firmware without sitting through a TCG boot by hand.

Usage:
    autoboot.py --flash build/flash.img --kernel build/parts/kernel.bin \\
        -c 'aplay -l' -c 'cat /proc/mtd'
"""

import argparse
import os
import pty
import re
import select
import shutil
import subprocess
import sys
import time

DEFAULT_QEMU = os.path.expanduser("~/qemu/build/qemu-system-arm")
MTDPARTS = ("mtdparts=onenand:1024k(bootloader),1024k(params),"
            "4096k(kernel),-(root)")
APPEND = ("root=/dev/mtdblock3 rw rootfstype=jffs2 console=ttyS0,115200 "
          + MTDPARTS)

LOGIN_RE = re.compile(rb"everest login:")
PROMPT_RE = re.compile(rb"(root@everest[^\r\n]*[#\$]|\r\n# )")


class Console:
    def __init__(self, argv, logpath=None):
        self.master, slave = pty.openpty()
        self.log = open(logpath, "wb") if logpath else None
        self.proc = subprocess.Popen(
            argv, stdin=slave, stdout=slave, stderr=subprocess.STDOUT,
            close_fds=True)
        os.close(slave)
        self.buf = b""

    def expect(self, pattern, timeout):
        """Read until pattern matches. Returns the matched text."""
        deadline = time.monotonic() + timeout
        while True:
            m = pattern.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m.group(0)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                tail = self.buf[-800:].decode("utf-8", "replace")
                raise TimeoutError(
                    "timed out waiting for %s\n--- last output ---\n%s"
                    % (pattern.pattern, tail))
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited with %d" % self.proc.returncode)
            r, _, _ = select.select([self.master], [], [], min(remaining, 1.0))
            if not r:
                continue
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                raise RuntimeError("console closed")
            if not chunk:
                raise RuntimeError("console closed")
            self.buf += chunk
            if self.log:
                self.log.write(chunk)
                self.log.flush()

    def send(self, line):
        os.write(self.master, line.encode() + b"\n")

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        if self.log:
            self.log.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=DEFAULT_QEMU)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--ram", default="64")
    ap.add_argument("--board-id", default="2")
    ap.add_argument("--keypad-id", default="0")
    ap.add_argument("--audio", default="none",
                    help="value for -audio, e.g. 'driver=wav,path=/tmp/o.wav'")
    ap.add_argument("--log", default=None, help="write raw console output here")
    ap.add_argument("--boot-timeout", type=float, default=1800)
    ap.add_argument("--cmd-timeout", type=float, default=300)
    ap.add_argument("-c", "--command", action="append", default=[],
                    help="shell command to run after login (repeatable)")
    args = ap.parse_args()

    argv = [
        args.qemu,
        "-M", "everest,board-id=%s,keypad-id=%s" % (args.board_id, args.keypad_id),
        "-m", args.ram,
        "-kernel", args.kernel,
        "-append", APPEND,
        "-drive", "if=mtd,format=raw,file=%s" % args.flash,
        "-audio", args.audio,
        "-serial", "stdio",
        "-display", "none",
        "-monitor", "none",
    ]

    con = Console(argv, args.log)
    started = time.monotonic()
    rc = 0
    try:
        print("waiting for login prompt (this takes several minutes under TCG)...",
              flush=True)
        con.expect(LOGIN_RE, args.boot_timeout)
        print("login prompt after %.0fs" % (time.monotonic() - started), flush=True)
        con.send("root")
        con.expect(PROMPT_RE, args.cmd_timeout)
        print("logged in", flush=True)

        for cmd in args.command:
            print("+ %s" % cmd, flush=True)
            # A sentinel makes the end of each command unambiguous even when
            # the kernel interleaves its own printks with the shell output.
            con.send("%s ; echo __BPEMU_DONE_$?__" % cmd)
            out = con.expect(re.compile(rb"__BPEMU_DONE_(\d+)__"),
                             args.cmd_timeout)
            status = int(re.search(rb"(\d+)", out).group(1))
            print("  exit status %d" % status, flush=True)
            if status != 0:
                rc = status
    except (TimeoutError, RuntimeError) as e:
        print("FAILED: %s" % e, file=sys.stderr)
        rc = 1
    finally:
        con.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
