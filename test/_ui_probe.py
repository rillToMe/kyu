#!/usr/bin/env python3
# Probe UI QEMU untuk Notepad (gaya Windows): boot -> login -> start notepad ->
# buka menu / bar cari / ketik, lalu screendump + sampling piksel.
#
# Pakai SALINAN disk.img (test_disk.img) supaya disk asli tidak tersentuh.
# Jalankan dari root repo:  python test/_ui_probe.py <step>
import os, socket, subprocess, sys, time, shutil, re, zlib, struct

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"E:\Tools\msys2\mingw64\bin\qemu-system-x86_64.exe"
PORT = 4461
DISK = os.path.join(ROOT, "test_disk.img")
ISO = os.path.join(ROOT, "build", "boot_image.iso")
LOG = os.path.join(ROOT, "serial.log")
OUT = os.path.join(ROOT, "test", "_ui_out")

KEYMAP = {
    " ": "spc", "\n": "ret", "/": "slash", ".": "dot", ",": "comma",
    "-": "minus", "_": "shift-minus", ":": "shift-semicolon", "=": "equal",
}


def prep():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(DISK):
        shutil.copy(os.path.join(ROOT, "disk.img"), DISK)
    if os.path.exists(LOG):
        os.remove(LOG)


def start():
    cmd = [QEMU, "-cpu", "max", "-m", "1G", "-boot", "d", "-smp", "4",
           "-display", "none", "-vga", "std",
           "-drive", "file=%s,format=raw,index=0,media=disk" % DISK,
           "-drive", "file=%s,media=cdrom,index=2" % ISO,
           "-nic", "user,model=e1000",
           "-serial", "file:%s" % LOG,
           "-monitor", "tcp:127.0.0.1:%d,server,nowait" % PORT]
    p = subprocess.Popen(cmd, cwd=ROOT)
    return p


def connect(timeout=30):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.create_connection(("127.0.0.1", PORT), 2)
            s.settimeout(5)
            time.sleep(0.3)
            try:
                s.recv(65536)
            except Exception:
                pass
            return s
        except OSError:
            time.sleep(0.5)
    raise RuntimeError("monitor tidak bisa connect")


class Mon:
    def __init__(self, s):
        self.s = s

    def cmd(self, c, wait=0.12):
        self.s.sendall((c + "\n").encode())
        time.sleep(wait)
        try:
            return self.s.recv(65536).decode(errors="replace")
        except Exception:
            return ""

    def key(self, k, wait=0.06):
        return self.cmd("sendkey " + k, wait)

    def type(self, text, wait=0.12):
        for ch in text:
            if ch in KEYMAP:
                self.key(KEYMAP[ch], wait)
            elif ch.isupper():
                self.key("shift-" + ch.lower(), wait)
            else:
                self.key(ch, wait)

    def move(self, dx, dy):
        self.cmd("mouse_move %d %d" % (dx, dy), 0.03)

    def click(self):
        self.cmd("mouse_button 1", 0.05)
        self.cmd("mouse_button 0", 0.15)


def serial_text():
    try:
        with open(LOG, "rb") as f:
            return f.read().decode("latin-1", errors="replace")
    except OSError:
        return ""


def wait_serial(needle, timeout=90.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in serial_text():
            return True
        time.sleep(0.5)
    return False


def ppm_to_png(src, dst):
    """Tulis ulang screendump PPM sebagai PNG (agar bisa dibuka mata manusia)."""
    w, h, px = read_ppm(src)
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(dst, "wb") as f:
        f.write(png)
    return dst


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n255\n
    parts = re.split(rb"\s+", data[:32], 3)
    w, h = int(parts[1]), int(parts[2])
    px = data[data.index(b"255\n") + 4:]
    return w, h, px


def sample(path, x, y):
    w, h, px = read_ppm(path)
    i = (y * w + x) * 3
    return (px[i], px[i + 1], px[i + 2])


def count_colors(path, x0, y0, x1, y1, maxc=6):
    w, h, px = read_ppm(path)
    hist = {}
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            i = (y * w + x) * 3
            c = (px[i], px[i + 1], px[i + 2])
            hist[c] = hist.get(c, 0) + 1
    return sorted(hist.items(), key=lambda kv: -kv[1])[:maxc]


def main():
    steps = sys.argv[1:] or ["all"]
    prep()
    p = start()
    try:
        m = Mon(connect())
        print("monitor ok")
        if not wait_serial("ready", 120):
            print("!! serial belum 'ready'; ekor log:")
            print(serial_text()[-1500:])
            return
        print("boot ready")
        # login root / 1
        m.type("root")
        m.key("ret")
        time.sleep(0.6)
        m.type("1")
        m.key("ret")
        if not wait_serial("$", 30):
            time.sleep(3)
        time.sleep(1.5)
        print("logged in")

        if not wait_serial("root@kyuzen>", 30):
            time.sleep(3)
        time.sleep(1.0)
        m.type("start notepad")
        m.key("ret")
        time.sleep(2.5)
        m.cmd("screendump %s/s1_editor.ppm" % OUT, 1.0)

        if "all" in steps or "type" in steps:
            m.type("Halo notepad baru")
            time.sleep(0.5)
            m.cmd("screendump %s/s2_typed.ppm" % OUT, 1.0)

        if "all" in steps or "find" in steps:
            m.key("ctrl-f")
            time.sleep(0.8)
            m.cmd("screendump %s/s3_find.ppm" % OUT, 1.0)
            m.key("alt-x")   # tidak ada; hanya cek tidak crash

        if "all" in steps or "menu" in steps:
            # Menu "File" ada di kiri-atas window. Kursor PS/2 relatif: slam
            # ke (0,0) lalu maju terukur ke tengah judul "File" (~x=120,y=124).
            for _ in range(250):
                m.move(-40, -40)
            time.sleep(0.5)
            for _ in range(30):
                m.move(4, 0)
            for _ in range(31):
                m.move(0, 4)
            time.sleep(0.5)
            m.cmd("screendump %s/s4_before_menu.ppm" % OUT, 1.0)
            m.click()
            time.sleep(1.0)
            m.cmd("screendump %s/s5_menu_open.ppm" % OUT, 1.0)

        for p_ in sorted(os.listdir(OUT)):
            if p_.endswith(".ppm"):
                ppm_to_png(os.path.join(OUT, p_),
                           os.path.join(OUT, p_[:-4] + ".png"))
        print("--- serial tail ---")
        print(serial_text()[-1200:])
    finally:
        try:
            m.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        if p.poll() is None:
            p.terminate()


if __name__ == "__main__":
    main()
