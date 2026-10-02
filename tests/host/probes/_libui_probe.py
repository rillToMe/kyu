#!/usr/bin/env python3
# _libui_probe.py - verifikasi RUNTIME redesign libui di QEMU (headless).
#
# MENGAPA ADA
# Test host membuktikan geometri, warna, dan anggaran render, tapi TIDAK
# membuktikan bahwa aplikasi benar-benar hidup di dalam OS: ELF termuat, widget
# terinflasi, jendela ter-render, dan tidak ada panic. Itu hanya terbukti
# dengan boot.
#
# Alur (mengikuti _settings_probe.py yang sudah terbukti):
#   boot -> tunggu prompt "Username" -> root -> "Password" -> 1
#        -> tunggu "kyuzen>" (shell) -> tunggu "[desktop] shell started"
#        -> jalankan app lewat shell: `start <nama>`
#
# Yang diperiksa:
#   1. login + desktop siap;
#   2. Settings (UI baru + XML eksternal) start tanpa panic;
#   3. UI Gallery (app baru) start tanpa panic;
#   4. XML demo (dokumen eksternal) start tanpa panic;
#   5. tidak ada panic sepanjang sesi.
#
# Catatan deteksi panic: JANGAN cari kata "PANIC" polos — kernel mencetak
# `[PANIC_LOG] tidak ada crash ...` pada setiap boot sehat.
#
# Jalankan dari root repo:  python tests/host/probes/_libui_probe.py
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import _ui_probe as P

P.QEMU = r"E:\Tools\msys2\ucrt64\bin\qemu-system-x86_64.exe"
OUT = os.path.join(P.ROOT, "tests", "host", "probes", "_ui_out")

# Harness _ui_probe.py menunjuk build/boot_image.iso (path Makefile lama).
# Build CMake menaruh ISO di build/target/boot_image.iso.
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


def no_panic(tag):
    txt = P.serial_text()
    markers = ("[P1] lockdown", "[P2] layar", "[P3] persist", "[P4] loop",
               "KERNEL PANIC", "EXCEPTION", "BSOD")
    hit = [m for m in markers if m in txt]
    check(not hit, "%s: tanpa panic/BSOD" % tag)
    if hit:
        print("  (penanda: %s)" % ", ".join(hit))
    return not hit


def wait_new(pos, needle, timeout=60.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in P.serial_text()[pos:]:
            return True
        time.sleep(0.4)
    return False


def boot_and_login(m):
    if not P.wait_serial("ready", 180):
        return False
    pos = len(P.serial_text())
    if not wait_new(pos, "Username", 60):
        return False
    m.type("root", wait=0.2)
    m.key("ret")
    if not wait_new(pos, "Password", 30):
        return False
    time.sleep(0.5)
    m.type("1", wait=0.2)
    m.key("ret")
    if not wait_new(pos, "kyuzen>", 90):
        return False
    t0 = time.time()
    while time.time() - t0 < 180:
        if P.wait_serial("[desktop] shell started", 20):
            break
        time.sleep(1.0)
    else:
        return False
    time.sleep(2.0)
    return True


def run_app(m, name, shot, expect=None):
    """`start <name>` di shell desktop, tunggu bukti hidup, screendump, tutup.

    BUKTI HIDUP: baris log spesifik app (kalau ada) atau setidaknya baris
    "start: <name>.elf" dari shell. Screendump diambil SEBELUM ESC supaya
    jendela masih di layar — urutan ini penting, kalau tidak yang tertangkap
    adalah desktop kosong.
    """
    pos = len(P.serial_text())
    m.type("start %s" % name, wait=0.2)
    m.key("ret")

    # Tunggu baris "start: <name>.elf" (shell mengonfirmasi spawn).
    spawned = wait_new(pos, "start: %s.elf" % name, 25)
    check(spawned, "%s: shell men-spawn proses" % name)

    # Tunggu bukti app sendiri hidup (kalau punya trace), lalu render.
    if expect:
        check(wait_new(pos, expect, 30), "%s: mencapai event loop (%s)"
              % (name, expect))
    time.sleep(3.5)

    tail = P.serial_text()[pos:]
    if "not found" in tail.lower() or "no such" in tail.lower():
        check(False, "%s: perintah dikenali shell" % name)
    else:
        check(True, "%s: perintah dikenali shell" % name)
    no_panic(name)

    # Screendump SAAT jendela masih terbuka.
    try:
        m.cmd("screendump %s/%s.ppm" % (OUT, shot), 1.5)
        print("WROTE %s.ppm" % shot)
    except Exception as e:
        print("NOTE screendump %s gagal: %s" % (shot, e))
    m.key("esc")
    time.sleep(1.5)
    return True


def main():
    os.makedirs(OUT, exist_ok=True)
    P.prep()
    proc = P.start()
    mon = None
    try:
        mon = P.Mon(P.connect(timeout=40))
        time.sleep(3)

        ok = boot_and_login(mon)
        check(ok, "boot+login: desktop siap ([desktop] shell started)")
        if not ok:
            print("serial tail:\n%s" % P.serial_text()[-800:])
            return 1
        no_panic("login")

        run_app(mon, "settings", "libui_settings", expect="[settings] start")
        run_app(mon, "uigallery", "libui_gallery")
        run_app(mon, "xml_demo", "libui_xmldemo")

        no_panic("sesi")
    finally:
        try:
            if mon:
                mon.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        try:
            proc.terminate()
        except Exception:
            pass

    print("\nlibui-probe: %s" % ("FAIL" if FAILS else "OK"))
    if FAILS:
        print("gagal: %s" % ", ".join(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
