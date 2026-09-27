kyuzen-os/
│
├── apps/         # Aplikasi end-user (ELF Ring-3 via apps/Makefile + app.ld)
│   ├── filemanager/  # C++ SDK (dibangun root Makefile → fileman.elf)
│   ├── gallery/      # C++ (thumbs LRU, media.o)
│   ├── imageview/    # C++
│   ├── calc.c  clock.c  notepad.c  settings.c  taskmgr.c
│   ├── terminal.c  viewer.c  widget_demo.c
│
├── system/       # Komponen inti OS (ikut build kernel Ring-0 + ELF util)
│   ├── desktop/      # C++ SDK (slot boot desktop.elf, via libdesktop)
│   ├── shell.c  shell_core.c  login.c  zen.c   # task kernel
│   └── cat.c  echo.c                            # util CLI (ELF user)
│
├── libs/         # SEMUA pustaka (tanpa folder 'lib' terpisah)
│   ├── core/     # userlib.c  kernel_userlib.c  libgui.c  userutil.c
│   ├── c/        # boundary C SDK (linker/, README) + libc-port/
│   ├── cpp/      # boundary C++ SDK (bin/kyuzen-c++, include/, linker/)
│   ├── gui/      # color/  libdesktop/  widget/
│   └── media/    # media.c  png.c (decoder + abstraksi media bersama)
│
├── tests/        # SEMUA pengujian (pengganti folder test/ lama)
│   ├── host/         # jalan di build machine (clang host, tanpa QEMU)
│   │   ├── probes/   # harness QEMU: _ui_probe, _fileman/_menu/_dialog probe,
│   │   │             # _screen_text/_scan_rows (+ _ui_out/ hasil screendump)
│   │   └── unit/     # unit test C/C++ host: media, desktop_manifest,
│   │                 # libui_theme/fileman_widgets, kyuzenfs_tree, libc_heap…
│   └── target/       # jalan DI DALAM OS (ELF Ring-3): badptr, fork/fd/pipe/
│                     # kill/exit/procinfo + varian desktop uji (test-desktop/)
├── kernel/  drivers/  fs/  arch/  graphics/  include/
├── tools/        # mkfs, libc-phase*, desktop-phase8, harness QEMU
└── build/        # Artifact (gitignored): obj/, bin/myos.bin, apps/*.elf,
                  # iso_root/, boot_image.iso, sdk/ (staging generate)
