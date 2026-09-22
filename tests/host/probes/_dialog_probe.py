#!/usr/bin/env python3
# _dialog_probe.py — verifikasi bar cari (panel + textbox border) di QEMU.
import os, sys, time, importlib.util

spec = importlib.util.spec_from_file_location("pr", os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ui_probe.py"))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)

OUT = pr.OUT


def main():
    pr.prep()
    p = pr.start()
    try:
        m = pr.Mon(pr.connect())
        if not pr.wait_serial("ready", 120):
            print("boot gagal"); return
        m.type("root"); m.key("ret")
        time.sleep(0.6)
        m.type("1"); m.key("ret")
        time.sleep(3.5)
        m.type("start notepad"); m.key("ret")
        time.sleep(3.5)

        m.type("tes")
        m.key("ctrl-f")
        time.sleep(1.0)
        m.cmd("screendump %s/d1_findbar.ppm" % OUT, 1.2)
        m.key("esc")          # ESC menutup bar cari (bukan keluar)
        time.sleep(0.8)
        m.cmd("screendump %s/d2_findclosed.ppm" % OUT, 1.2)
        pr.ppm_to_png("%s/d1_findbar.ppm" % OUT, "%s/d1_findbar.png" % OUT)
        pr.ppm_to_png("%s/d2_findclosed.ppm" % OUT, "%s/d2_findclosed.png" % OUT)
        print("--- serial tail ---")
        print(pr.serial_text()[-400:])
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
