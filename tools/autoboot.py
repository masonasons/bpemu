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
import socket
import subprocess
import sys
import tempfile
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


class Monitor:
    """QEMU human monitor over a unix socket, used to inject key presses."""

    def __init__(self, path, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.sock.settimeout(5)
        self._drain()

    def _drain(self):
        out = b""
        try:
            while True:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                out += chunk
        except (socket.timeout, BlockingIOError):
            pass
        return out

    def sendkey(self, key, hold_ms=None):
        cmd = "sendkey %s" % key
        if hold_ms:
            cmd += " %d" % hold_ms
        self.sock.sendall(cmd.encode() + b"\n")
        time.sleep(0.4)
        reply = self._drain().decode("utf-8", "replace")
        # The monitor echoes the command and prints a prompt; anything else is
        # an error worth surfacing rather than silently swallowing.
        noise = [ln.strip() for ln in reply.replace("\r", "").split("\n")]
        errs = [ln for ln in noise
                if ln and cmd not in ln and not ln.startswith("(qemu)")]
        return " | ".join(errs)


class OrderedStep(argparse.Action):
    """Collect -c/-k into one list so their relative order is preserved."""

    def __call__(self, parser, namespace, values, option_string=None):
        steps = getattr(namespace, "steps", None)
        if steps is None:
            steps = []
            setattr(namespace, "steps", steps)
        kind = "key" if option_string in ("-k", "--key") else "cmd"
        steps.append((kind, values))


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
    ap.add_argument("-c", "--command", action=OrderedStep,
                    help="shell command to run after login (repeatable)")
    ap.add_argument("-k", "--key", action=OrderedStep,
                    help="QEMU sendkey name to inject, e.g. kp_1 (repeatable); "
                         "ordering with -c is preserved")
    ap.add_argument("--key-hold-ms", type=int, default=800,
                    help="how long to hold each injected key (default 800). "
                         "QEMU's default of 100ms is often too short for a "
                         "TCG guest to debounce and sample the matrix.")
    args = ap.parse_args()
    steps = getattr(args, "steps", None) or []

    monitor_path = None
    if any(kind == "key" for kind, _ in steps):
        monitor_path = os.path.join(tempfile.mkdtemp(prefix="bpemu-"), "mon")

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
    ]
    if monitor_path:
        argv += ["-monitor", "unix:%s,server,nowait" % monitor_path]
    else:
        argv += ["-monitor", "none"]

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

        mon = Monitor(monitor_path) if monitor_path else None

        for kind, value in steps:
            if kind == "key":
                err = mon.sendkey(value, args.key_hold_ms)
                print("* sendkey %s%s" % (value, "  -> %s" % err if err else ""),
                      flush=True)
                if err:
                    rc = 1
                continue
            print("+ %s" % value, flush=True)
            # A sentinel makes the end of each command unambiguous even when
            # the kernel interleaves its own printks with the shell output.
            # It goes on its own line rather than after a ';' so that commands
            # ending in '&' stay valid shell.
            con.send(value)
            con.send("echo __BPEMU_DONE_$?__")
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
