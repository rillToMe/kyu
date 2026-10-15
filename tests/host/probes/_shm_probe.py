# _shm_probe.py - R3 QEMU integration: shm_demo in two processes +
# live VirtIO fence. Boots with virtio-vga (the fence path only exists
# there; -vga std uses the sync software backend).
#
# Run:  python tests/host/probes/_shm_probe.py
# Pass: "SHM DEMO PASS" on serial, DIRECT CK == SHADOW CK, FENCE LIVE,
#       no panic markers.
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P
import importlib

_libui = importlib.import_module("_libui_probe")

for _q in (r"E:\Tools\msys2\mingw64\bin\qemu-system-x86_64.exe",
           r"E:\Tools\msys2\ucrt64\bin\qemu-system-x86_64.exe"):
    if os.path.exists(_q):
        P.QEMU = _q
        break
_ISO_CMAKE = os.path.join(P.ROOT, "build", "target", "boot_image.iso")
if os.path.exists(_ISO_CMAKE):
    P.ISO = _ISO_CMAKE

FAILS = []


def check(cond, what):
    if cond:
        print("PASS %s" % what)
    else:
        print("FAIL %s" % what)
        FAILS.append(what)


def start_virtio():
    cmd = [P.QEMU, "-cpu", "max", "-m", "1G", "-boot", "d", "-smp", "4",
           "-display", "none", "-vga", "none",
           "-device", "virtio-vga,xres=1280,yres=720",
           "-drive", "file=%s,format=raw,index=0,media=disk" % P.DISK,
           "-drive", "file=%s,media=cdrom,index=2" % P.ISO,
           "-nic", "user,model=e1000",
           "-serial", "file:%s" % P.LOG,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % P.PORT]
    return subprocess.Popen(cmd, cwd=P.ROOT)


def main():
    P.prep()
    proc = start_virtio()
    try:
        mon = P.Mon(P.connect())
        if not _libui.boot_and_login(mon):
            check(False, "boot dan login")
            return 1
        pos = len(P.serial_text())
        mon.type("start shm_demo", wait=0.2)
        mon.key("ret")
        # NOTE: no "start: shm_demo.elf" needle — SMP serial interleaving
        # splits it ("start: SHM D...shm_demo...elf...EMO", see boot_check
        # PAST_MARKER fallback for the same reason). DIRECT CK proves the
        # process spawned and reached publish.
        check(_libui.wait_new(pos, "DIRECT CK", 60),
              "shm_demo hidup sampai publish")
        ok = _libui.wait_new(pos, "SHM DEMO PASS", 90)
        check(ok, "SHM DEMO PASS di serial")
        if not ok:
            tail = P.serial_text()[pos:]
            bad = [l for l in tail.splitlines() if "FAIL" in l]
            for l in bad[:10]:
                print("  serial: %s" % l)
        txt = P.serial_text()[pos:]
        dm = re.search(r"DIRECT CK (\d+)", txt)
        sm = re.search(r"SHADOW CK (\d+)", txt)
        check(dm is not None, "DIRECT CK tercatat")
        check(sm is not None, "SHADOW CK tercatat")
        if dm and sm:
            check(dm.group(1) == sm.group(1),
                  "differential lintas-proses (ck sama)")
            print("  ck=%s" % dm.group(1))
        check("FRAME2 MATCH" in txt, "frame2 pasca-fork dibaca lintas-proses")
        check("stat-dims: OK" in txt, "metadata otoritatif (R4)")
        check("replace-destroy: OK" in txt, "replacement + retire (R4)")
        if "FENCE LIVE" in txt:
            check(True, "fence VirtIO live (present maju, err 0)")
        elif "FENCE SKIP" in txt:
            check(False, "fence VirtIO live (backend tanpa stats!)")
        else:
            check(False, "fence VirtIO live (FENCE QUIET)")
        _libui.no_panic("shm_demo")
        for f in _libui.FAILS:
            if f not in FAILS:
                FAILS.append(f)
    finally:
        proc.terminate()
    if FAILS:
        print("PROBE FAIL: %s" % FAILS)
        return 1
    print("PROBE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
