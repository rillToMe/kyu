#!/usr/bin/env python3
"""
Run fpu_test inside QEMU and read its verdict from COM1.

Boots the ISO, logs in as root, runs `fpu_test`, and requires the PASS line on
the serial console. This is the in-OS half of the Phase 3 verification: it
proves user-space FPU state survives context switches and syscalls.

Usage:
    python3 tools/ci/run_fpu_test.py --iso build/target/boot_image.iso
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

PASS = re.compile(r"fpu_test: PASS")
FAIL = re.compile(r"fpu_test: FAIL")


def qemu_cmd(iso, disk, serial, mon_port):
    return [
        "qemu-system-x86_64",
        "-cpu", "max", "-m", "1G", "-boot", "d", "-smp", "4",
        "-drive", f"file={disk},format=raw,index=0,media=disk",
        "-drive", f"file={iso},media=cdrom,index=2",
        "-nic", "user,model=e1000",
        "-vga", "std",
        "-display", "none",
        "-serial", f"file:{serial}",
        # The QEMU monitor lets us type into the guest without a display.
        # TCP, not a unix socket: Windows Python has no socket.AF_UNIX.
        "-monitor", f"tcp:127.0.0.1:{mon_port},server,nowait",
        "-no-reboot",
    ]


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def mon(port, cmd):
    """Send one command to the QEMU monitor over TCP."""
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.settimeout(3.0)
        s.connect(("127.0.0.1", port))
        try:
            s.recv(4096)                  # banner
        except socket.timeout:
            pass
        s.sendall((cmd + "\n").encode())
        time.sleep(0.15)
        try:
            s.recv(4096)
        except socket.timeout:
            pass
    finally:
        s.close()


def send_keys(port, keys, delay=0.25):
    for k in keys:
        mon(port, f"sendkey {k}")
        time.sleep(delay)


def type_text(port, text, delay=0.12):
    """Type a string. QEMU sendkey takes key names, not characters."""
    name = {
        " ": "spc", "\n": "ret", "_": "shift-minus", "-": "minus",
        "/": "slash", ".": "dot",
    }
    for ch in text:
        if ch in name:
            mon(port, f"sendkey {name[ch]}")
        elif ch.isdigit() or ch.isalpha():
            mon(port, f"sendkey {ch.lower()}")
        time.sleep(delay)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--serial", default="fpu_test_serial.log")
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--mon-port", type=int, default=45555,
                    help="TCP port for the QEMU monitor")
    args = ap.parse_args()

    if shutil.which("qemu-system-x86_64") is None:
        print("FAIL: qemu-system-x86_64 not on PATH", file=sys.stderr)
        return 1
    if not os.path.isfile(args.iso):
        print(f"FAIL: ISO not found: {args.iso}", file=sys.stderr)
        return 1

    work = tempfile.mkdtemp(prefix="kyuzen-fpu-")
    disk = os.path.join(work, "disk.img")
    serial = os.path.abspath(args.serial)
    if os.path.exists(serial):
        os.remove(serial)
    with open(disk, "wb") as f:
        f.truncate(100 * 1024 * 1024)

    proc = subprocess.Popen(qemu_cmd(args.iso, disk, serial, args.mon_port),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    # Wait for the monitor to accept connections.
    import socket
    port = args.mon_port
    for _ in range(100):
        try:
            t = socket.create_connection(("127.0.0.1", port), timeout=1)
            t.close()
            break
        except OSError:
            time.sleep(0.2)

    deadline = time.time() + args.timeout
    status = "timeout"
    log = ""

    try:
        # Wait for the first-boot password prompt.
        while time.time() < deadline:
            log = read(serial)
            if "Password Root Baru" in log:
                break
            if proc.poll() is not None:
                status = "qemu-exited"
                break
            time.sleep(1)

        if status == "timeout" and "Password Root Baru" in log:
            # First boot: the installer asks for the root password ONCE (no
            # confirmation prompt — see system/login.c). Typing it twice sent
            # the second "1" into the username field of the login screen.
            print("fpu_test: creating the root password (first boot)")

            def wait_for(marker, budget=90):
                end = time.time() + budget
                while time.time() < end:
                    if marker in read(serial):
                        return True
                    time.sleep(0.5)
                return False

            type_text(port, "1")
            send_keys(port, ["ret"], delay=0.5)

            # The installer writes users.sys, then loads the login page.
            wait_for("Username", budget=90)
            time.sleep(1.5)
            type_text(port, "root")
            send_keys(port, ["ret"], delay=0.5)
            time.sleep(1.5)
            type_text(port, "1")
            send_keys(port, ["ret"], delay=0.5)

            # Wait for the shell prompt before typing a command.
            wait_for("root@kyuzen", budget=90)
            time.sleep(1)

            print("fpu_test: running the test")
            type_text(port, "start fpu_test")
            send_keys(port, ["ret"], delay=0.5)

            while time.time() < deadline:
                log = read(serial)
                if PASS.search(log) or FAIL.search(log):
                    break
                if proc.poll() is not None:
                    break
                time.sleep(1)

            if PASS.search(log):
                status = "pass"
            elif FAIL.search(log):
                status = "fail"
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        shutil.rmtree(work, ignore_errors=True)

    log = read(serial)
    print("")
    print("===== serial (last 25) =====")
    for line in log.splitlines()[-25:]:
        print(line)
    print("============================")
    print("")

    if status == "pass":
        print("PASS: fpu_test reported PASS")
        return 0
    if status == "fail":
        print("FAIL: fpu_test reported FAIL", file=sys.stderr)
        return 1
    print(f"FAIL: fpu_test did not finish ({status})", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
