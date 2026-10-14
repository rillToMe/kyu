#!/usr/bin/env python3
"""
boot_check.py — boot a KyuzenOS ISO in QEMU and fail unless the kernel reports
that it reached userspace.

    python3 tools/ci/boot_check.py --iso build/target/boot_image.iso

WHY THIS EXISTS
    A successful link is not a working system. The kernel can build cleanly and
    still panic before it ever reaches init: a bad Limine module, a corrupted
    page table, or an SMP race all produce an ISO that builds and dies at boot.
    Nothing else in CI would notice.

    So this runs the image and requires the kernel's own success marker on COM1:

        KyuzenOS ready.

    That line is printed by boot_handoff_to_init() (kernel/kernel.c) only after
    the display, memory, paging, heap, interrupts, filesystem, VFS, SMP, network
    and all 88 Limine modules are up, and init.elf has been created as a ring-3
    task. It is the narrowest single signal that the whole boot path worked.

WHAT IT BOOTS WITH
    The hardware configuration mirrors the project's own run targets
    (cmake/KyuzenQemu.cmake): 1 GB RAM, an e1000 NIC, and a disk. The disk is a
    fresh, empty image on purpose — the kernel formats an unformatted disk
    itself ([KZFS4] Disk belum terformat V4 - memformat...), so no prebuilt
    disk.img (which is gitignored and 100 MB) is needed here.

    -vga std is deliberate. The project's default is -device virtio-vga, but
    that device is not present in every QEMU package (Debian/Ubuntu ship it in
    qemu-system-x86; Arch splits it into qemu-hw-display-virtio-gpu-pci, and
    even then only virtio-gpu-pci exists). The boot marker is printed to COM1
    before any GPU-dependent path can matter, and the software GHAL backend is
    exercised, so the display device does not need to match the dev default.

EXIT CODES
    0  the boot marker was seen
    1  QEMU failed to start, panicked, or never reached the marker in time
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

# The kernel's own "everything came up" line. See boot_handoff_to_init().
BOOT_MARKER = "KyuzenOS ready."

# A panic is a definite failure, so it fails fast instead of waiting out the
# timeout. Both spellings are checked: the BSOD banner and the serial dump.
PANIC_PATTERNS = [
    re.compile(r"KERNEL PANIC", re.IGNORECASE),
    re.compile(r"PANIC BERSARANG", re.IGNORECASE),
    re.compile(r"EXCEPTION: "),
]

# An empty disk. The kernel formats it on first boot, so no disk image needs to
# be committed or downloaded.
DISK_SIZE_MB = 100


def build_qemu_args(iso_path, disk_path, serial_path, cpus, memory):
    return [
        "qemu-system-x86_64",
        "-cpu", "max",
        "-m", memory,
        "-boot", "d",
        "-smp", str(cpus),
        "-drive", f"file={disk_path},format=raw,index=0,media=disk",
        "-drive", f"file={iso_path},media=cdrom,index=2",
        "-nic", "user,model=e1000",
        # See the module docstring: the marker is on COM1 and predates any GPU
        # path, so the display device is not the one from the dev run targets.
        "-vga", "std",
        "-display", "none",
        "-serial", f"file:{serial_path}",
        "-no-reboot",
    ]


def read_serial(path):
    """Read the serial log, tolerating a partially written file."""
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", required=True, help="path to boot_image.iso")
    parser.add_argument("--serial", default="boot_serial.log",
                        help="where to write the COM1 log")
    parser.add_argument("--timeout", type=int, default=120,
                        help="seconds to wait for the boot marker")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory", default="1G")
    args = parser.parse_args()

    if shutil.which("qemu-system-x86_64") is None:
        print("FAIL: qemu-system-x86_64 is not on PATH", file=sys.stderr)
        return 1

    if not os.path.isfile(args.iso):
        print(f"FAIL: ISO not found: {args.iso}", file=sys.stderr)
        return 1

    iso_size = os.path.getsize(args.iso)
    print(f"boot_check: ISO {args.iso} ({iso_size} bytes)")
    if iso_size == 0:
        print("FAIL: the ISO is empty", file=sys.stderr)
        return 1

    workdir = tempfile.mkdtemp(prefix="kyuzen-boot-")
    disk_path = os.path.join(workdir, "disk.img")
    serial_path = os.path.abspath(args.serial)

    # Remove any stale log so a previous run's marker cannot pass this one.
    if os.path.exists(serial_path):
        os.remove(serial_path)

    with open(disk_path, "wb") as f:
        f.truncate(DISK_SIZE_MB * 1024 * 1024)

    cmd = build_qemu_args(args.iso, disk_path, serial_path,
                          args.cpus, args.memory)
    print("boot_check: " + " ".join(cmd))

    qemu = subprocess.Popen(
        cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    deadline = time.time() + args.timeout
    status = None
    serial = ""

    try:
        while time.time() < deadline:
            # QEMU exiting on its own means the guest reset or crashed; either
            # way this run cannot pass.
            if qemu.poll() is not None:
                status = "qemu-exited"
                break

            serial = read_serial(serial_path)

            if BOOT_MARKER in serial:
                status = "ok"
                break

            for pattern in PANIC_PATTERNS:
                if pattern.search(serial):
                    status = "panic"
                    break
            if status == "panic":
                break

            time.sleep(0.5)
    finally:
        if qemu.poll() is None:
            qemu.terminate()
            try:
                qemu.wait(timeout=10)
            except subprocess.TimeoutExpired:
                qemu.kill()
                qemu.wait()
        qemu.stderr.close()

    serial = read_serial(serial_path)
    shutil.rmtree(workdir, ignore_errors=True)

    print("")
    print("===== serial log (tail) =====")
    tail = serial.splitlines()[-30:]
    for line in tail:
        print(line)
    if not tail:
        print("(no serial output)")
    print("=============================")
    print("")

    if status == "ok":
        print("PASS: boot marker seen on COM1")
        return 0

    if status == "panic":
        print("FAIL: the kernel panicked during boot", file=sys.stderr)
        return 1

    if status == "qemu-exited":
        print("FAIL: QEMU exited before the boot marker appeared",
              file=sys.stderr)
        return 1

    print(f"FAIL: no boot marker within {args.timeout}s", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
