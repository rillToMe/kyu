#!/usr/bin/env python3
# tools/fontdemo/qemu_home.py — homing kursor closed-loop ke ikon fontdemo.
# Boot -> login -> [screendump, move] x N (lacak kursor via diff) ->
# klik saat dalam rect ikon -> "[fontdemo] launch" -> quit.
# Dijalankan: E:/Tools/msys2/clang64/bin/python3.exe tools/fontdemo/qemu_home.py
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "build", "fontdemo")
DISK = os.path.join(OUT, "fontdemo-disk.img")
SERIAL = os.path.join(OUT, "fontdemo-home-serial.log")
MONLOG = os.path.join(OUT, "fontdemo-home-monitor.log")
ISO = os.path.join(ROOT, "build", "boot_image.iso")
QEMU = os.environ.get("QEMU", "E:/Tools/msys2/qemu/qemu-system-x86_64.exe")
W, H = 1920, 1080
TARGET = (1584, 48)  # tengah ikon sel-16
ICON_RECT = (1560, 24, 1608, 72)


def load_ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    assert d[:2] == b"P6"
    i, vals = 2, []
    while len(vals) < 3:
        while d[i : i + 1].isspace():
            i += 1
        if d[i : i + 1] == b"#":
            while d[i : i + 1] != b"\n":
                i += 1
        else:
            j = i
            while d[j : j + 1].isdigit():
                j += 1
            vals.append(int(d[i:j]))
            i = j
    return vals[0], vals[1], d[i + 1 :]


def diff_blobs(a, b):
    pxa, pxb = a[2], b[2]
    cell, n = {}, 0
    seen = set()
    for y in range(H):
        base = y * W * 3
        for x in range(W):
            o = base + x * 3
            if pxa[o : o + 3] != pxb[o : o + 3]:
                n += 1
                seen.add((x // 16, y // 16))
    # flood fill sel 16px
    blobs = []
    while seen:
        stack, comp = [seen.pop()], []
        while stack:
            c = stack.pop()
            comp.append(c)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    nb = (c[0] + dx, c[1] + dy)
                    if nb in seen:
                        seen.discard(nb)
                        stack.append(nb)
        xs = [c[0] * 16 + 8 for c in comp]
        ys = [c[1] * 16 + 8 for c in comp]
        blobs.append((sum(xs) // len(xs), sum(ys) // len(ys), len(comp)))
    return n, blobs


class Qemu:
    def __init__(self):
        import shutil

        os.makedirs(OUT, exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, "disk.img"), DISK)
        for f in (SERIAL, MONLOG):
            if os.path.exists(f):
                os.remove(f)
        open(SERIAL, "w").close()
        self.mon = open(MONLOG, "w")
        self.p = subprocess.Popen(
            [QEMU, "-cpu", "max", "-m", "1G", "-boot", "d", "-smp", "4",
             "-no-reboot", "-drive", "file=%s,format=raw,index=0,media=disk" % DISK,
             "-drive", "file=%s,media=cdrom,index=2" % ISO,
             "-nic", "user,model=e1000", "-vga", "none",
             "-device", "virtio-vga,xres=1920,yres=1080",
             "-usb", "-device", "usb-tablet", "-display", "none",
             "-serial", "file:%s" % SERIAL, "-monitor", "stdio"],
            stdin=subprocess.PIPE, stdout=self.mon, stderr=subprocess.STDOUT,
            cwd=ROOT,
        )

    def cmd(self, s):
        self.p.stdin.write((s + "\n").encode())
        self.p.stdin.flush()
        time.sleep(0.4)

    def key(self, k):
        self.cmd("sendkey %s" % k)
        time.sleep(0.06)

    def type_line(self, s):
        for c in s:
            self.key(c if (c.isalnum()) else "spc")
        self.key("ret")

    def shot(self, name):
        p = os.path.join(OUT, name)
        if os.path.exists(p):
            os.remove(p)
        self.cmd("screendump %s" % p)
        for _ in range(100):
            if os.path.exists(p) and os.path.getsize(p) > 6220800:
                return p
            time.sleep(0.3)
        raise RuntimeError("no screenshot " + name)

    def wait_for(self, pat, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            with open(SERIAL, "rb") as f:
                if pat.encode() in f.read():
                    return True
            time.sleep(2)
        return False

    def stop(self):
        try:
            self.cmd("quit")
            time.sleep(2)
        except Exception:
            pass
        try:
            self.p.kill()
        except Exception:
            pass


def main():
    q = Qemu()
    try:
        assert q.wait_for("Username : ", 300), "no login prompt"
        q.type_line("root")
        assert q.wait_for("Password : ", 120), "no password prompt"
        q.type_line("1")
        assert q.wait_for("@kyuzen>", 120), "no shell"
        time.sleep(6)
        # homing: estimasi transfer 1:1 dari posisi terakhir dikirim
        sent = [960, 540]
        q.cmd("mouse_move 960 540")
        time.sleep(2)
        prev = q.shot("home-0.ppm")
        cur = None
        for it in range(12):
            # langkah eksplorasi kecil agar blob kursor terisolasi
            sent[0] = min(1919, sent[0] + 60)
            q.cmd("mouse_move %d %d" % (sent[0], sent[1]))
            time.sleep(2)
            shot = q.shot("home-%d.ppm" % (it + 1))
            _, blobs = diff_blobs(load_ppm(prev), load_ppm(shot))
            prev = shot
            blobs = [b for b in blobs if 2 <= b[2] <= 60]
            print("iter %d blobs=%s" % (it, blobs), flush=True)
            if len(blobs) == 2:
                # blob baru = yang jauh dari posisi lama (bila diketahui)
                if cur is None:
                    cand = blobs
                else:
                    cand = sorted(blobs,
                                  key=lambda b: abs(b[0] - cur[0]) + abs(b[1] - cur[1]),
                                  reverse=True)
                cur = (cand[0][0], cand[0][1])
            elif len(blobs) == 1 and cur is not None:
                # hanya satu terlihat (menimpa posisi lama?) — pakai itu
                cur = (blobs[0][0], blobs[0][1])
            else:
                continue
            print("cursor ~ %s target %s" % (cur, TARGET), flush=True)
            x0, y0, x1, y1 = ICON_RECT
            if x0 <= cur[0] <= x1 and y0 <= cur[1] <= y1:
                print("ON ICON — click", flush=True)
                q.cmd("mouse_button 1")
                time.sleep(0.3)
                q.cmd("mouse_button 0")
                time.sleep(1)
                q.shot("home-click.ppm")
                if q.wait_for("[fontdemo] launch", 120):
                    q.shot("home-app.ppm")
                    print("ICON-CLICK PASS", flush=True)
                    return 0
                print("ICON-CLICK FAIL: no launch", flush=True)
                return 1
            # koreksi proporsional dari error
            ex, ey = TARGET[0] - cur[0], TARGET[1] - cur[1]
            sent[0] = max(0, min(1919, sent[0] + ex))
            sent[1] = max(0, min(1079, sent[1] + ey))
        print("HOMING FAIL: tidak mencapai ikon", flush=True)
        return 1
    finally:
        q.stop()


sys.exit(main())
