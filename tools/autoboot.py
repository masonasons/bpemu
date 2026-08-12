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
import re
import select
import shlex
import socket
import subprocess
import sys
import tempfile
import time

try:
    import pty
except ImportError:      # Windows: the console runs over TCP instead.
    pty = None

DEFAULT_QEMU = os.path.expanduser("~/qemu/build/qemu-system-arm")
MTDPARTS = ("mtdparts=onenand:1024k(bootloader),1024k(params),"
            "4096k(kernel),-(root)")
APPEND = ("root=/dev/mtdblock3 rw rootfstype=jffs2 console=ttyS0,115200 "
          + MTDPARTS)

LOGIN_RE = re.compile(rb"everest login:")
PROMPT_RE = re.compile(rb"(root@everest[^\r\n]*[#\$]|\r\n# )")


class Console:
    """The guest serial line, over a pty where there is one and TCP otherwise.

    Windows has no pty module, so there the console is a socket and QEMU is
    told to serve it. Everything above this class works the same either way.
    """

    def __init__(self, argv, logpath=None, port=None):
        self.log = open(logpath, "wb") if logpath else None
        self.buf = b""
        self.sock = None
        self.master = None
        if port is None:
            self.master, slave = pty.openpty()
            self.proc = subprocess.Popen(
                argv, stdin=slave, stdout=slave, stderr=subprocess.STDOUT,
                close_fds=True)
            os.close(slave)
            return
        self.proc = subprocess.Popen(argv, close_fds=True)
        deadline = time.monotonic() + 30
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited with %d" % self.proc.returncode)
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), 1.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise RuntimeError("qemu never accepted a console "
                                       "connection on port %d" % port)
                time.sleep(0.2)
        self.sock.setblocking(False)

    def _fileno(self):
        return self.master if self.sock is None else self.sock.fileno()

    def _read(self):
        if self.sock is None:
            return os.read(self.master, 4096)
        return self.sock.recv(4096)

    def _write(self, data):
        if self.sock is None:
            os.write(self.master, data)
        else:
            self.sock.sendall(data)

    def expect(self, pattern, timeout):
        """Read until pattern matches.

        Returns everything consumed up to and including the match, not just
        the match itself: for a command sentinel the interesting part is the
        output that came before it.
        """
        deadline = time.monotonic() + timeout
        while True:
            m = pattern.search(self.buf)
            if m:
                consumed = self.buf[:m.end()]
                self.buf = self.buf[m.end():]
                return consumed
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                tail = self.buf[-800:].decode("utf-8", "replace")
                raise TimeoutError(
                    "timed out waiting for %s\n--- last output ---\n%s"
                    % (pattern.pattern, tail))
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited with %d" % self.proc.returncode)
            r, _, _ = select.select([self._fileno()], [], [],
                                    min(remaining, 1.0))
            if not r:
                continue
            try:
                chunk = self._read()
            except BlockingIOError:
                continue
            except OSError:
                raise RuntimeError("console closed")
            if not chunk:
                raise RuntimeError("console closed")
            self.buf += chunk
            if self.log:
                self.log.write(chunk)
                self.log.flush()

    def send(self, line):
        self._write(line.encode() + b"\n")

    def close(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        if self.sock:
            self.sock.close()
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

    def command(self, cmd):
        """Run an arbitrary monitor command and return its reply.

        Handy for reading device registers with `xp`, which goes through the
        MMIO dispatch and so reflects what the model would hand the guest.
        """
        self.sock.sendall(cmd.encode() + b"\n")
        time.sleep(0.5)
        reply = self._drain().decode("utf-8", "replace").replace("\r", "")
        lines = [ln.strip() for ln in reply.split("\n")]
        return " | ".join(ln for ln in lines
                          if ln and cmd not in ln and ln != "(qemu)")

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
        if option_string in ("-k", "--key"):
            kind = "key"
        elif option_string in ("-m", "--mon"):
            kind = "mon"
        else:
            kind = "cmd"
        steps.append((kind, values))


def free_port():
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def command_output(raw, command):
    """Pull a command's own output out of the console capture.

    What comes back from the guest is the terminal echo of the command, then
    its output, then the echo of the sentinel and the sentinel itself. Drop
    everything that is our own scaffolding and keep the rest.
    """
    lines = raw.decode("utf-8", "replace").replace("\r", "").split("\n")
    keep = []
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if stripped == command.strip():
            continue
        if "__BPEMU_DONE_" in stripped:
            continue
        if PROMPT_RE.search(stripped.encode()) and stripped.endswith(("#", "$")):
            continue
        keep.append(line.rstrip())
    return keep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=DEFAULT_QEMU)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--hdd", default=None,
                    help="internal IDE disk image; without one the firmware's "
                         "sysmon applet crash-loops and the UI never starts")
    ap.add_argument("--ram", default="64")
    ap.add_argument("--board-id", default="2")
    ap.add_argument("--keypad-id", default="1",
                    help="1 selects the 8-dot braille keycode array, which is "
                         "the only one with a space key")
    ap.add_argument("--net", action="store_true",
                    help="attach the RTL8150 USB ethernet adapter, which the "
                         "firmware has a driver for, with QEMU user networking")
    ap.add_argument("--serial-tcp", type=int, default=0, metavar="PORT",
                    help="drive the console over TCP on this port instead of a "
                         "pty; 0 picks a free one. Implied on Windows, which "
                         "has no pty")
    ap.add_argument("--pcap", default=None,
                    help="with --net, dump both directions of the wire to this "
                         "pcap file")
    ap.add_argument("--extra", action="append", default=[], metavar="ARGS",
                    help="extra arguments passed through to qemu, split with "
                         "shell quoting; repeatable")
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
    ap.add_argument("-m", "--mon", action=OrderedStep,
                    help="QEMU monitor command to run, e.g. 'xp/1xw 0x40e00038'")
    ap.add_argument("--push", action="append", default=[], metavar="LOCAL:REMOTE",
                    help="copy a local file into the guest before running "
                         "commands, typed over the console as a heredoc. Far "
                         "easier than fighting three layers of shell quoting.")
    ap.add_argument("--key-hold-ms", type=int, default=800,
                    help="how long to hold each injected key (default 800). "
                         "QEMU's default of 100ms is often too short for a "
                         "TCG guest to debounce and sample the matrix.")
    args = ap.parse_args()
    steps = getattr(args, "steps", None) or []

    monitor_path = None
    if any(kind in ("key", "mon") for kind, _ in steps):
        monitor_path = os.path.join(tempfile.mkdtemp(prefix="bpemu-"), "mon")

    argv = [
        args.qemu,
        "-M", "everest,board-id=%s,keypad-id=%s" % (args.board_id, args.keypad_id),
        "-m", args.ram,
        "-kernel", args.kernel,
        "-append", APPEND,
        "-drive", "if=mtd,format=raw,file=%s" % args.flash,
        "-audio", args.audio,
        "-display", "none",
    ]
    port = None
    if pty is None or args.serial_tcp:
        port = args.serial_tcp or free_port()
        argv += ["-serial", "tcp:127.0.0.1:%d,server,nowait" % port]
    else:
        argv += ["-serial", "stdio"]
    if args.hdd:
        argv += ["-drive", "if=ide,index=0,format=qcow2,file=%s" % args.hdd]
    if args.net:
        argv += ["-netdev", "user,id=bpnet",
                 "-device", "usb-rtl8150,netdev=bpnet"]
        if args.pcap:
            argv += ["-object", "filter-dump,id=bpdump,netdev=bpnet,file=%s"
                     % args.pcap]
    for extra in args.extra:
        argv += shlex.split(extra)
    if monitor_path:
        argv += ["-monitor", "unix:%s,server,nowait" % monitor_path]
    else:
        argv += ["-monitor", "none"]

    con = Console(argv, args.log, port)
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

        for spec in args.push:
            local, _, remote = spec.rpartition(":")
            if not local:
                raise RuntimeError("--push wants LOCAL:REMOTE, got %r" % spec)
            with open(local, "r") as f:
                body = f.read().rstrip("\n")
            print("> pushing %s -> %s (%d bytes)" % (local, remote, len(body)),
                  flush=True)
            # A quoted heredoc delimiter stops the guest shell expanding
            # anything in the payload.
            con.send("cat > %s <<'__BPEMU_EOF__'" % remote)
            for line in body.split("\n"):
                con.send(line)
            con.send("__BPEMU_EOF__")
            con.send("echo __BPEMU_DONE_$?__")
            con.expect(re.compile(rb"__BPEMU_DONE_(\d+)__"), args.cmd_timeout)

        for kind, value in steps:
            if kind == "mon":
                print("$ %s -> %s" % (value, mon.command(value)), flush=True)
                continue
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
            # Match the sentinel itself, and take the last one: `out` now holds
            # the command's output too, which can contain digits of its own.
            status = int(re.findall(rb"__BPEMU_DONE_(\d+)__", out)[-1])
            # Echo what the command actually said. Without this only the exit
            # status comes back, which is no help at all for the many commands
            # that report their findings on stdout and still exit 0.
            for line in command_output(out, value):
                print("  | %s" % line, flush=True)
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
