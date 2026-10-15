#!/usr/bin/env python3
"""
boot_soak.py — boot the ISO repeatedly and report every failure.

    python3 tools/ci/boot_soak.py --iso build/target/boot_image.iso --runs 50

WHY THIS EXISTS

    A single successful boot proves the system CAN come up. It does not prove
    it comes up RELIABLY. KyuzenOS is SMP with per-CPU run queues and work
    stealing, so the scheduler interleaving differs on every boot — and a race
    that loses one time in twenty will never show up in a single boot.

    The roadmap's Phase 2.2 asks for exactly this: "boot 50x, record crashes".
    Until CI existed there was no way to run it. Now the same marker the
    boot-smoke job uses is the pass/fail signal, so 50 boots is just 50
    iterations of a job that already works.

WHAT COUNTS AS A FAILURE

    - no "KyuzenOS ready." marker within the timeout
    - a panic on the serial log
    - QEMU exiting on its own (the guest reset or triple-faulted)
    - any "notifikasi crash" / crash-dump line: the kernel reaching userspace
      and THEN crashing is still a failure, and this is the class of bug the
      soak is looking for

WHAT THIS DOES NOT COVER

    It boots, reaches userspace, and stops. It does not drive the GUI, launch
    applications, or exercise the network beyond DHCP — those need the
    sendkey/monitor machinery in tools/ci/run_fpu_test.py, and belong in a
    separate soak that drives a scripted workload. Stated here so a green soak
    is not mistaken for more coverage than it is.

EXIT CODES
    0  every run passed
    1  at least one run failed (the failing serial logs are kept)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

BOOT_MARKER = "KyuzenOS ready."
PANIC = re.compile(r"KERNEL PANIC|PANIC BERSARANG|EXCEPTION: ", re.IGNORECASE)
# The kernel prints this when it finds a crash dump from a previous boot.
POST_BOOT_CRASH = re.compile(r"notifikasi crash|crash-report|dump ke-", re.IGNORECASE)

# serial_print() has no lock, so on SMP two CPUs can interleave their bytes on
# COM1. A real capture looked like this — the marker and an [init] line fused:
#
#     [init] KyuzenKyOSu zineint OdiSm ulraieady.
#
# (That is "[init] KyuzenOS init dimulai" + "KyuzenOS ready." written at once.)
# A byte-exact search would miss it and report a timeout for a boot that
# actually succeeded. So the marker test below ALSO accepts evidence that the
# boot progressed PAST the marker: the init/login lines that can only appear
# after it.
PAST_MARKER = re.compile(
    r"login\.elf di-spawn|INSTALASI KYUZEN OS|Password Root Baru",
    re.IGNORECASE,
)


def has_booted(text):
    """True if the marker is present, whole or interleaved."""
    if BOOT_MARKER in text:
        return True
    # Fallback: the marker itself was garbled by SMP interleaving, but the
    # boot demonstrably continued past it.
    return bool(PAST_MARKER.search(text))


def qemu_cmd(iso, disk, serial, cpus):
    return [
        "qemu-system-x86_64",
        "-cpu", "max", "-m", "1G", "-boot", "d",
        "-smp", str(cpus),
        "-drive", f"file={disk},format=raw,index=0,media=disk",
        "-drive", f"file={iso},media=cdrom,index=2",
        "-nic", "user,model=e1000",
        "-vga", "std",
        "-display", "none",
        "-serial", f"file:{serial}",
        "-no-reboot",
    ]


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def one_run(iso, run_index, timeout, cpus, keep_dir):
    """Boot once. Returns (ok, reason, serial_text)."""
    work = tempfile.mkdtemp(prefix=f"kyuzen-soak{run_index}-")
    disk = os.path.join(work, "disk.img")
    serial = os.path.join(work, "serial.log")
    with open(disk, "wb") as f:
        f.truncate(100 * 1024 * 1024)

    proc = subprocess.Popen(qemu_cmd(iso, disk, serial, cpus),
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)

    deadline = time.time() + timeout
    ok, reason = False, "timeout"
    try:
        while time.time() < deadline:
            if proc.poll() is not None:
                reason = "qemu-exited"
                break
            text = read(serial)
            if has_booted(text):
                # Give the system a moment to reach userspace and settle, so a
                # crash immediately AFTER the marker is still caught.
                time.sleep(3)
                text = read(serial)
                if PANIC.search(text):
                    reason = "panic-after-marker"
                elif POST_BOOT_CRASH.search(text):
                    reason = "crash-reported"
                else:
                    ok, reason = True, "ok"
                break
            if PANIC.search(text):
                reason = "panic"
                break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    text = read(serial)
    if not ok and keep_dir:
        dest = os.path.join(keep_dir, f"run{run_index:03d}-{reason}.log")
        try:
            with open(dest, "w", encoding="utf-8", errors="replace") as f:
                f.write(text)
        except OSError:
            pass

    shutil.rmtree(work, ignore_errors=True)
    return ok, reason, text


def run_worker(args_tuple):
    """Worker entry point for the parallel pool. Returns (index, ok, reason)."""
    index, iso, timeout, cpus, keep = args_tuple
    ok, reason, _ = one_run(iso, index, timeout, cpus, keep)
    return index, ok, reason


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--runs", type=int, default=50)
    ap.add_argument("--timeout", type=int, default=120,
                    help="seconds per boot")
    ap.add_argument("--cpus", type=int, default=4,
                    help="CPUs INSIDE each guest")
    ap.add_argument("--parallel", type=int, default=0,
                    help="guests to run at once (0 = auto: one per host core, "
                         "capped at 8). 50 serial boots take ~25 minutes; "
                         "8-way parallel takes ~4.")
    ap.add_argument("--keep-failures", default="boot_soak_failures",
                    help="directory for the serial logs of failed runs")
    args = ap.parse_args()

    if shutil.which("qemu-system-x86_64") is None:
        print("FAIL: qemu-system-x86_64 is not on PATH", file=sys.stderr)
        return 1
    if not os.path.isfile(args.iso):
        print(f"FAIL: ISO not found: {args.iso}", file=sys.stderr)
        return 1

    parallel = args.parallel
    if parallel <= 0:
        parallel = min(os.cpu_count() or 1, 8)
    parallel = max(1, min(parallel, args.runs))

    keep = os.path.abspath(args.keep_failures)
    os.makedirs(keep, exist_ok=True)

    print(f"boot_soak: {args.runs} runs, {parallel} in parallel, "
          f"{args.cpus} guest CPUs, {args.timeout}s timeout")
    print(f"boot_soak: ISO {args.iso}")
    print("")

    started = time.time()
    results = []

    # Sequential when parallel == 1 so the output reads as a clean list; a pool
    # otherwise, which is the only way 50 boots is a practical thing to ask for.
    if parallel == 1:
        for i in range(1, args.runs + 1):
            idx, ok, reason = run_worker((i, args.iso, args.timeout,
                                          args.cpus, keep))
            results.append((idx, ok, reason))
            print(f"  run {idx:3d}/{args.runs}: {'ok' if ok else 'FAIL (' + reason + ')'}")
    else:
        from concurrent.futures import ProcessPoolExecutor, as_completed
        with ProcessPoolExecutor(max_workers=parallel) as pool:
            futures = [
                pool.submit(run_worker,
                            (i, args.iso, args.timeout, args.cpus, keep))
                for i in range(1, args.runs + 1)
            ]
            for fut in as_completed(futures):
                idx, ok, reason = fut.result()
                results.append((idx, ok, reason))
                print(f"  run {idx:3d}/{args.runs}: "
                      f"{'ok' if ok else 'FAIL (' + reason + ')'}")

    results.sort(key=lambda r: r[0])
    passed = sum(1 for _, ok, _ in results if ok)
    failures = [(i, r) for i, ok, r in results if not ok]
    elapsed = time.time() - started

    print("")
    print(f"boot_soak: {passed}/{args.runs} passed in {elapsed:.0f}s "
          f"({parallel}-way parallel)")

    if failures:
        print("")
        print("failed runs:")
        for i, reason in failures:
            print(f"  run {i}: {reason}")
        print("")
        print(f"serial logs kept in: {keep}")
        return 1

    print("boot_soak: PASS")
    # A clean soak leaves no failure logs behind.
    try:
        if not os.listdir(keep):
            os.rmdir(keep)
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
