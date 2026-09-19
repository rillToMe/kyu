#!/usr/bin/env python3
# _screen_text.py — baca screendump QEMU (PPM) kembali menjadi TEKS.
#
# Dibuat untuk memverifikasi UI di QEMU headless (-display none): tata letak
# window (menu, status bar) bisa "dibaca" tanpa mata, memakai font8x16 ASLI yang
# menggambar layar (include/font8x16.h). Cara deteksi: di dalam satu sel 8x16,
# warna yang paling banyak = latar sel, pixel yang berbeda = tinta glyph; pola
# tinta itu dicocokkan ke tabel font.
#
# Pakai:  python test/_screen_text.py <screendump.ppm> [--rows y0 y1] [--cols x0 x1]
import sys, re, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_font(path=None):
    path = path or os.path.join(ROOT, "include", "font8x16.h")
    glyphs = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*\{\s*(.*?)\s*\}\s*,", line)
            if not m:
                continue
            parts = [p.strip() for p in m.group(1).split(",") if p.strip()]
            vals = [int(p, 0) for p in parts]
            if len(vals) == 16:
                glyphs.append(vals)
    return glyphs


FONT = load_font()
MASK = [sum(1 << (7 - b) for b in range(8) if (g[row] >> b) & 1) for g in FONT for row in [0]]  # unused


def glyph_rows(g):
    # baris r: byte dengan bit7 = pixel paling kiri
    return [(g[r] >> (7 - b)) & 1 for r in range(16) for b in range(8)]


FONT_ROWS = [glyph_rows(g) for g in FONT]


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("bukan P6 PPM: %s" % path)
    idx = 2
    fields = []
    while len(fields) < 3:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx:idx + w * h * 3]


def cell_char(px, w, x, y):
    counts = {}
    cell = []
    for r in range(16):
        for c in range(8):
            i = ((y + r) * w + (x + c)) * 3
            col = px[i:i + 3]
            cell.append(col)
            counts[col] = counts.get(col, 0) + 1
    bg = max(counts.items(), key=lambda kv: kv[1])[0]
    bits = [1 if cell[r * 8 + c] != bg else 0 for r in range(16) for c in range(8)]
    if sum(bits) == 0:
        return " "
    best, best_score = -1, -1
    for gi, gr in enumerate(FONT_ROWS):
        score = 0
        for k in range(128):
            score += 1 if gr[k] == bits[k] else -1
        if score > best_score:
            best_score, best = score, gi
    if best_score < 90:
        return "?"
    ch = chr(best)
    return ch if 32 <= best < 127 else "?"


def decode_row(px, w, y, xoff, x0, x1):
    out = []
    for x in range(x0 + xoff, min(x1, w) - 7, 8):
        out.append(cell_char(px, w, x, y))
    return "".join(out).rstrip()


def best_score(px, w, y, xoff, x0, x1):
    sc = 0
    for x in range(x0 + xoff, min(x1, w) - 7, 8):
        ch = cell_char(px, w, x, y)
        if ch == " ":
            continue
        sc += 1 if ch not in "?" else -2
    return sc


def main():
    path = sys.argv[1]
    y0, y1, x0, x1 = 0, 10 ** 9, 0, 10 ** 9
    a = sys.argv[2:]
    while a:
        k = a.pop(0)
        if k == "--rows":
            y0, y1 = int(a.pop(0)), int(a.pop(0))
        elif k == "--cols":
            x0, x1 = int(a.pop(0)), int(a.pop(0))
    w, h, px = read_ppm(path)
    xoff = max(range(8), key=lambda o: sum(best_score(px, w, y, o, x0, x1)
                                           for y in range(y0, min(y1, h), 2)))
    print("== %s  %dx%d  xoff=%d ==" % (os.path.basename(path), w, h, xoff))
    prev = ""
    for y in range(y0, min(y1, h) - 15):
        sc = best_score(px, w, y, xoff, x0, x1)
        if sc < 2:
            continue
        # hanya cetak puncak lokal (hindari baris teks yang sama dicetak 3x)
        if any(best_score(px, w, yy, xoff, x0, x1) > sc
               for yy in range(max(y0, y - 6), min(y + 7, h - 15)) if yy != y):
            continue
        line = decode_row(px, w, y, xoff, x0, x1)
        if line.strip() and line != prev:
            print("%4d|%s" % (y, line))
            prev = line


if __name__ == "__main__":
    main()
