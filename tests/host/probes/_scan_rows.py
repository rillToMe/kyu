#!/usr/bin/env python3
# _scan_rows.py — cetak teks untuk baris-baris layar tertentu dari screendump.
# Pakai: python tests/host/probes/_scan_rows.py <file.ppm> y0 y1 ... [--cols x0 x1]
import os, sys, importlib.util

spec = importlib.util.spec_from_file_location("st", os.path.join(os.path.dirname(os.path.abspath(__file__)), "_screen_text.py"))
st = importlib.util.module_from_spec(spec)
spec.loader.exec_module(st)

path = sys.argv[1]
cols = (0, 10 ** 9)
ys = []
a = sys.argv[2:]
while a:
    k = a.pop(0)
    if k == "--cols":
        cols = (int(a.pop(0)), int(a.pop(0)))
    else:
        ys.append(int(k))

w, h, px = st.read_ppm(path)
print("== %s %dx%d ==" % (path, w, h))
for y in ys:
    best, bestline = -10 ** 9, ""
    for off in range(8):
        line = st.decode_row(px, w, y, off, cols[0], cols[1])
        sc = st.best_score(px, w, y, off, cols[0], cols[1])
        if sc > best:
            best, bestline = sc, line
    print("%4d|%s" % (y, bestline))
