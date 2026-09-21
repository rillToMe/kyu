# ==========================================
# Kyuzen OS Build System
# ==========================================
#
# Target penting:
#   make          → Compile kernel (build/bin/myos.bin)
#   make apps     → Compile user_apps (build/apps/*.elf)
#   make boot_image.iso → Build kernel + apps + ISO
#   make run      → Build + Boot di QEMU
#   make clean    → Bersihkan SELURUH hasil build (build/ + sisa .o/.d lama)
#   make clean-apps → Bersihkan hasil build user_apps saja
#
# --- Tata letak output ---
# Tidak ada lagi object file di source tree; semuanya di build/:
#
#   build/obj/<path sumber>.o    object kernel (mirror struktur sumber)
#       kernel/foo.c                  → build/obj/kernel/foo.o
#       kernel/gfx/fb.c               → build/obj/kernel/gfx/fb.o
#       drivers/net/e1000/e1000.c     → build/obj/drivers/net/e1000/e1000.o
#   build/obj/<path sumber>.d    dependensi header (-MMD -MP), pasangan .o
#   build/obj/user/<path>.o      object user_apps (namespace terpisah: file
#                                seperti apps/userutil.c dipakai kernel DAN
#                                user app dengan flag berbeda, jadi tidak boleh
#                                berbagi object)
#   build/bin/myos.bin           kernel (build/myos.bin = salinan kompatibilitas)
#   build/apps/*.elf             ELF user (C/C++ dan Rust)
#   build/iso_root/              staging ISO
#   build/boot_image.iso         image boot hybrid BIOS+UEFI

# ==========================================
# Tools
# ==========================================
CC = clang
AS = nasm
LD = ld.lld
QEMU = qemu-system-x86_64.exe

# --- Tampilan host (jendela QEMU) ---
# Default: GTK + zoom-to-fit + FULLSCREEN, jadi guest 1920x1080 tampil besar
# (di-scale mengikuti monitor) dan screenshot layar penuh langsung enak dibaca.
# Tanpa zoom-to-fit, jendela hanya menampilkan guest 1:1 — di monitor besar
# terlihat kecil, dan di monitor kecil isinya terpotong.
# Override sesuai kebutuhan:
#   make run FULLSCREEN=0                    → mode jendela (tombol Maximize tetap jalan)
#   make run QEMU_DISPLAY=none FULLSCREEN=0  → headless, jejak hanya di serial.log
#   make run QEMU_DISPLAY=sdl,zoom-to-fit=on → frontend SDL
QEMU_DISPLAY ?= gtk,zoom-to-fit=on
FULLSCREEN   ?= 1
# Kosong bila FULLSCREEN bukan 1 (ekspansi kosong itu sah di shell).
FULLSCREEN_ARG = $(if $(filter 1,$(FULLSCREEN)),-full-screen,)

# Direktori sumber kernel (Ring 0).
# Daftar eksplisit — sengaja TIDAK memakai find rekursif dari root supaya
# third_party/*, legacy/, test/, debug/, rust/target, dan hasil build tidak
# ikut terambil secara tidak sengaja. Sub-directory yang memang bagian kernel
# didaftarkan langsung di sini (driver NIC & NET port lwIP punya daftar sendiri
# di bawah karena flag-nya beda).
SRC_DIRS = arch/x86 drivers kernel kernel/smp kernel/gfx kernel/sched fs kernel/fs apps \
           kernel/panic \
           graphics graphics/backend graphics/backend/intel graphics/memory drivers/graphics/hw libs/color/src


# ==========================================
# lwIP Network Stack
# ==========================================

# Semua file .c dari lwIP core, netif, dan port driver kita.
# File port/sys_arch.c TIDAK diperlukan saat NO_SYS=1 — hanya sys_now()
# yang perlu diimplementasikan di kernel/timer.c atau sejenisnya.
LWIP_CORE_DIR  = third_party/net/lwip/src/core
LWIP_NETIF_DIR = third_party/net/lwip/src/netif
LWIP_PORT_DIR  = drivers/net/port
LWIP_INC_DIR   = third_party/net/lwip/src/include

# Kumpulkan semua source lwIP secara otomatis
LWIP_CORE_SRCS = $(wildcard $(LWIP_CORE_DIR)/*.c)       \
                 $(wildcard $(LWIP_CORE_DIR)/ipv4/*.c)

# Daftar eksplisit netif yang kita butuhkan:
#   ethernet.c   — Ethernet frame input/output (wajib untuk LWIP_ETHERNET=1)
# File-file berikut SENGAJA TIDAK diinclude:
#   slipif.c     — Serial Line IP (butuh sio_open/sio_send/sio_tryread)
#   zepif.c      — IEEE 802.15.4 ZEP encapsulation
#   bridgeif*.c  — L2 bridge (butuh infrastruktur terpisah)
#   lowpan6*.c   — 6LoWPAN untuk IoT (butuh LWIP_IPV6)
#   ppp/         — Point-to-Point Protocol
LWIP_NETIF_SRCS= $(LWIP_NETIF_DIR)/ethernet.c

LWIP_PORT_SRCS = $(LWIP_PORT_DIR)/kyuzen_netif.c \
                 $(LWIP_PORT_DIR)/sys_arch.c

# Driver e1000 (Intel 82540EM NIC) — dikompilasi dengan CFLAGS kernel biasa
# (tidak perlu lwIP headers, driver ini standalone)
E1000_DIR  = drivers/net/e1000
E1000_SRCS = $(E1000_DIR)/e1000.c

# Gabung semua source lwIP + e1000 (di-link bersama, flag berbeda per grup)
LWIP_SRCS      = $(LWIP_CORE_SRCS) $(LWIP_NETIF_SRCS) $(LWIP_PORT_SRCS)

# LWIP_CFLAGS akan didefinisikan di bawah, setelah CFLAGS kernel tersedia

# --- Flags Compiler 64-bit ---
# 1. Target diubah menjadi x86_64
# 2. -m32 DIHAPUS
# 3. DITAMBAHKAN -mno-red-zone (SANGAT PENTING!)
# 4. -mcmodel=kernel: wajib untuk higher-half kernel — mencegah R_X86_64_32
#    relocation error saat simbol berada di atas 4GB (0xFFFFFFFF80000000)
INCLUDE_DIR = include

# --- Folder output ---
# build/obj   : object + dependency file (mirror source tree)
# build/bin   : kernel binary
# build/apps  : ELF user
# build/iso_root + build/boot_image.iso
# Direktori TIDAK dibuat saat parse (`$(shell mkdir -p ...)`). Direktori
# object dibuat lewat order-only prerequisite (lihat OBJ_DIRS di bawah):
# sekali per direktori, aman untuk `make -j8`, dan tidak menambah satu proses
# `mkdir` untuk setiap object (mahal di Windows, ~20ms per spawn).
BUILD_DIR  = build
OBJ_DIR    = $(BUILD_DIR)/obj
BIN_DIR    = $(BUILD_DIR)/bin
ELF_DIR    = $(BUILD_DIR)/apps
ISO_ROOT   = $(BUILD_DIR)/iso_root
ISO_IMAGE  = $(BUILD_DIR)/boot_image.iso

# -MMD -MP: tulis file .d (dependensi header) di samping tiap .o — perubahan
# header (mis. task.h) memicu rebuild semua .c yang meng-includenya. Tanpa
# ini, object basi membaca struct dengan layout lama (pernah menggigit:
# task_t tambah field, scheduler membaca tasks[] dengan stride basi).
# Jalur baca ATA (Stage 1): 0 = LEGACY (default, apa adanya), 1 = BATCH.
# Implementasi: drivers/ata.c + include/ata.h. Desain: DOCUMENTATION/design/
# ata-driver-redesign-proposal.md. Dua jalur selalu dikompilasi; yang ini
# hanya memilih default saat boot — jalur lama tetap ada sebagai fallback.
# Contoh pakai: make ATA_READ_PATH_DEFAULT=1 boot_image.iso
ATA_READ_PATH_DEFAULT ?= 0
CFLAGS = --target=x86_64-pc-none-elf -ffreestanding -O2 -nostdlib -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -MMD -MP -I$(INCLUDE_DIR) -Igraphics -Igraphics/memory -Idrivers/graphics/hw -Ilibs/color/include -DATA_READ_PATH_DEFAULT=$(ATA_READ_PATH_DEFAULT)

# Flags compiler untuk unit lwIP:
#   - Mewarisi semua flag kernel (freestanding, mcmodel, mno-red-zone, dll.)
#   - Tambahkan path header lwIP dan port Kyuzen
#   - -Wno-error mencegah warning internal lwIP memblok build
LWIP_CFLAGS    = $(CFLAGS) -std=c11 -I$(LWIP_INC_DIR) -I$(LWIP_PORT_DIR) -Wno-error

# --- Flags Assembler ---
# NASM sekarang merakit output 64-bit
ASFLAGS = -f elf64

# --- Flags Linker ---
# LLD sekarang menyatukan file dengan format x86_64
LDFLAGS = -flavor gnu -T linker.ld -m elf_x86_64 --build-id=none -nostdlib

# Cari semua file .c dan .asm di dalam SRC_DIRS
C_SOURCES_RAW = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))
ASM_SOURCES = $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.asm))

# Exclude file user-space — dikompilasi terpisah oleh user_apps/Makefile,
# BUKAN bagian dari kernel Ring 0 (myos.bin).
#   apps/userlib.c  → berisi int $0x80 syscall wrappers
#   apps/libgui.c   → GUI framework (mendefinisikan font8x16, dll)
# Juga exlude test host-side (punya main()/assert.h/stdio.h) yang dijalankan di
# host, bukan sebagai task QEMU — dikompilasi freestanding akan fatal (assert.h
# tidak ada). aa_math_test / desktop_manifest_test / kyuzenfs_dir_test /
# virtqueue_test / cred_test ada di test/ yang ikut SRC_DIRS saat
# `make conc`/`make heap-stress`.
C_SOURCES = $(filter-out apps/userlib.c apps/libgui.c \
                        test/aa_math_test.c test/desktop_manifest_test.cpp test/kyuzenfs_dir_test.c test/kyuzenfs_v4_test.c test/kyuzenfs_xcheck.c test/panic_test.c                        test/virtqueue_test.c test/virtio_gpu_cmd_test.c test/cred_test.c test/proc_test.c test/kill_test.c test/fd_test.c test/pipe_test.c test/fork_test.c test/color_test.c test/ata_devmodel_test.c,\
                        $(C_SOURCES_RAW))

# --- Pemetaan sumber → object (generik, tidak ada daftar object manual) ---
#   kernel/foo.c              → build/obj/kernel/foo.o
#   kernel/gfx/fb.c           → build/obj/kernel/gfx/fb.o
#   drivers/net/e1000/e1000.c → build/obj/drivers/net/e1000/e1000.o
#   arch/x86/gdt_flush.asm    → build/obj/arch/x86/gdt_flush.o
OBJS       = $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES)) \
             $(patsubst %.asm,$(OBJ_DIR)/%.o,$(ASM_SOURCES))
LWIP_OBJS  = $(patsubst %.c,$(OBJ_DIR)/%.o,$(LWIP_SRCS))
E1000_OBJS = $(patsubst %.c,$(OBJ_DIR)/%.o,$(E1000_SRCS))
ALL_OBJS   = $(OBJS) $(LWIP_OBJS) $(E1000_OBJS)

# Default goal dipatok DULU. Tanpa ini, -include file .d di bawah membuat
# target pertama file .d menjadi default goal → `make` hanya membangun satu
# object. (.DEFAULT_GOAL yang eksplisit menang atas target pertama yang
# dilihat make.)
.DEFAULT_GOAL := all

# Sertakan dependensi header hasil -MMD/-MD (diabaikan saat belum ada / setelah
# clean). File .d yang sama juga memberi tahu make saat sebuah HEADER berubah.
-include $(ALL_OBJS:.o=.d)

# File output kernel. build/myos.bin dipertahankan sebagai salinan kompatibel
# untuk catatan/script lokal yang masih menunjuk path lama; isinya hanya
# ditulis ulang bila berubah (jadi tidak memicu rebuild ISO yang sia-sia).
TARGET     = $(BIN_DIR)/myos.bin
COMPAT_BIN = $(BUILD_DIR)/myos.bin

# Default target (kernel saja; ISO dibangun lewat `make boot_image.iso`)
.PHONY: all
all: $(TARGET) $(COMPAT_BIN)

# Tahap 3: Link Semuanya
$(TARGET): $(ALL_OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@

$(COMPAT_BIN): $(TARGET)
	@cmp -s $< $@ || cp $< $@

# Direktori object: dibuat sekali (order-only) sebelum object-object di
# dalamnya. `$@` di sini adalah direktori itu sendiri; kalau direktorinya sudah
# ada make tidak menjalankan apa pun (target tanpa prerequisite = up to date).
OBJ_DIRS = $(sort $(dir $(ALL_OBJS)))
$(OBJ_DIRS):
	@mkdir -p $@

# Tahap 2: Compile C (kernel/arch/drivers/fs/apps)
$(OBJ_DIR)/%.o: %.c | $(OBJ_DIRS)
	$(CC) $(CFLAGS) -std=c11 -c $< -o $@

# Tahap 2b: Compile lwIP source files (LWIP_CFLAGS).
# Pattern rule yang lebih spesifik menang atas $(OBJ_DIR)/%.o: %.c di atas —
# stem-nya lebih pendek — sehingga file lwIP selalu dapat -I header yang benar.
$(OBJ_DIR)/third_party/net/lwip/src/%.o: third_party/net/lwip/src/%.c | $(OBJ_DIRS)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# Port lwIP Kyuzen (kyuzen_netif.c, sys_arch.c) — juga LWIP_CFLAGS
$(OBJ_DIR)/$(LWIP_PORT_DIR)/%.o: $(LWIP_PORT_DIR)/%.c | $(OBJ_DIRS)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# e1000 driver: pakai CFLAGS kernel biasa (bukan LWIP_CFLAGS)
# e1000.c tidak butuh lwIP headers — hanya kernel headers (heap, string, pci),
# jadi cukup rule generik di atas.

# net_init.c: butuh LWIP_CFLAGS karena include lwIP headers (dhcp.h, dns.h, dll)
# Aturan ini OVERRIDE aturan generic $(OBJ_DIR)/%.o:%.c untuk file ini saja.
$(OBJ_DIR)/kernel/net_init.o: kernel/net_init.c | $(OBJ_DIRS)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# net_ping.c: butuh LWIP_CFLAGS karena include lwIP raw/icmp/dns headers
$(OBJ_DIR)/kernel/net_ping.o: kernel/net_ping.c | $(OBJ_DIRS)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# net_socket.c: butuh LWIP_CFLAGS karena include lwIP tcp headers
$(OBJ_DIR)/kernel/net_socket.o: kernel/net_socket.c | $(OBJ_DIRS)
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# Tahap 1: Compile Assembly
# -MD menulis dependensi (%include, mis. arch/x86/isr_macro.inc) ke .d di
# samping object; file itu ikut di-include di atas bersama .d hasil clang.
$(OBJ_DIR)/%.o: %.asm | $(OBJ_DIRS)
	$(AS) $(ASFLAGS) -MD $(@:.o=.d) $< -o $@

# --- compile_commands.json untuk IntelliSense VS Code ---
# Menangkap flag compile PERSIS dari build sungguhan (via dry-run) sehingga
# IntelliSense tidak pernah out-of-sync dengan Makefile. Jalankan ulang setiap
# kali menambah file .c baru atau mengubah -I path.
#   Butuh: python -m pip install compiledb
# `env -u MAKELEVEL`: make mengekspor MAKELEVEL=1 ke recipe-nya, dan make yang
# dijalankan compiledb lalu menganggap dirinya SUB-make sehingga output dry-run
# tidak tertangkap (database jadi kosong). Tanpa variabel itu, seluruh command
# ter-capture normal.
.PHONY: compile_commands
compile_commands:
	env -u MAKELEVEL python -m compiledb -n make clean all

# --- Host-side unit test (roadmap §11): virtqueue multi-chain ---
# Dikompilasi dengan compiler host (bukan freestanding) — mock MMIO berupa
# struct biasa di memori. TIDAK ikut build kernel (terkecualikan dari
# C_SOURCES, jalankan eksplisit: make test-virtqueue).
HOSTCC = clang
HOSTCXX = clang++
.PHONY: test-virtqueue
test-virtqueue: test/virtqueue_test
	./test/virtqueue_test

test/virtqueue_test: test/virtqueue_test.c \
                     drivers/graphics/hw/virtqueue.c \
                     drivers/graphics/hw/virtqueue.h \
                     drivers/graphics/hw/virtio_gpu_regs.h \
                     graphics/memory/gpu_alloc.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/virtqueue_test.c \
	    drivers/graphics/hw/virtqueue.c \
	    -Idrivers/graphics/hw -Igraphics/memory

# --- Host-side unit test: encoder command virtio-gpu ---
# Mengunci semantik wire-format yang tidak kelihatan di log serial — terutama
# offset TRANSFER_TO_HOST_2D (device membacanya sebagai awal baris sumber, jadi
# rect di luar (0,0) tidak boleh mengirim 0). Test mengemulasi loop transfer
# QEMU apa adanya dan membandingkan isi rect hasilnya.
# Jalankan: make test-virtio-cmd
.PHONY: test-virtio-cmd
test-virtio-cmd: test/virtio_gpu_cmd_test
	./test/virtio_gpu_cmd_test

test/virtio_gpu_cmd_test: test/virtio_gpu_cmd_test.c \
                          drivers/graphics/hw/virtio_gpu_cmd.c \
                          drivers/graphics/hw/virtio_gpu_cmd.h \
                          drivers/graphics/hw/virtio_gpu_regs.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/virtio_gpu_cmd_test.c \
	    drivers/graphics/hw/virtio_gpu_cmd.c -Idrivers/graphics/hw

# --- Host-side unit test (P0 Phase 1): credential policy ---
# Pure policy in include/cred.h, no scheduler needed. Run: make test-cred.
.PHONY: test-cred
test-cred: test/cred_test
	./test/cred_test

test/cred_test: test/cred_test.c include/cred.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/cred_test.c -Iinclude

# --- Host-side unit test (P0 Phase 2): process policy ---
# Pure policy in include/proc.h (+cred/task constants), no scheduler/SMP.
# Run: make test-proc.
.PHONY: test-proc
test-proc: test/proc_test
	./test/proc_test

test/proc_test: test/proc_test.c include/proc.h include/cred.h include/task.h include/vfs.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/proc_test.c -Iinclude

# --- Host-side unit test (P0 Phase 3): kill policy + lifecycle ---
# Pure policy in include/proc.h (proc_can_kill, kill codes/reasons) plus a
# mock of the proc_kill/observe/reap decision table. No scheduler/SMP.
# Run: make test-kill.
.PHONY: test-kill
test-kill: test/kill_test
	./test/kill_test

test/kill_test: test/kill_test.c include/proc.h include/cred.h include/task.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/kill_test.c -Iinclude

# --- Host-side unit test (P0 Phase 4): fd / open-description model ---
# Mock of kernel/vfs_fd.c semantics (per-task entries, shared refcounted
# descriptions, dup/dup2 sharing, close/close_all release, invalid-fd
# rejection, leak accounting). No scheduler/SMP/KyuzenFS.
# Run: make test-fd.
.PHONY: test-fd
test-fd: test/fd_test
	./test/fd_test

test/fd_test: test/fd_test.c include/vfs.h include/proc.h include/cred.h include/task.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/fd_test.c -Iinclude

# --- Host-side unit test (P0 Phase 5): pipe logic + shell pipelines ---
# Kernel pipe decision table (mock) + the REAL apps/shell_core.c against
# stub syscalls (operator scan, stage split, spawn_redir specs, parent
# close discipline, reaping). No scheduler/SMP/KyuzenFS.
# Run: make test-pipe.
.PHONY: test-pipe
test-pipe: test/pipe_test
	./test/pipe_test

test/pipe_test: test/pipe_test.c apps/shell_core.c include/shell.h include/userlib.h include/proc.h include/cred.h include/task.h include/vfs.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/pipe_test.c apps/shell_core.c -Iinclude

# --- Host-side unit test (P0 Phase 6B): fork algorithm mock ---
# AS-clone table (fresh frames, content copy, verbatim flags, huge skip,
# OOM-injection rollback sweep) + fd sharing + slot publish rules.
# No scheduler/SMP/paging hardware.
# Run: make test-fork.
.PHONY: test-fork
test-fork: test/fork_test
	./test/fork_test

test/fork_test: test/fork_test.c include/proc.h include/cred.h include/task.h include/vfs.h
	$(HOSTCC) -O2 -Wall -Wextra -o $@ test/fork_test.c -Iinclude

# --- Host-side unit test: libs/color (tipe, blend, ruang warna, utility UI) ---
# Sumber library asli dikompilasi di host (integer murni → tak butuh QEMU);
# header dicek juga sebagai C++ (app userspace C++ memakainya).
# Jalankan: make test-color
COLOR_SRCS = libs/color/src/color_blend.c libs/color/src/color_space.c \
             libs/color/src/color_utils.c
COLOR_HDRS = libs/color/include/color_types.h libs/color/include/color_blend.h \
             libs/color/include/color_space.h libs/color/include/color_utils.h

.PHONY: test-color
test-color: test/color_test
	./test/color_test

test/color_test: test/color_test.c test/color_cxx_check.cpp $(COLOR_SRCS) $(COLOR_HDRS) include/aa_math.h
	$(HOSTCC) -O2 -Wall -Wextra -Iinclude -Ilibs/color/include -o $@ test/color_test.c $(COLOR_SRCS)
	$(HOSTCXX) -std=c++17 -Wall -Wextra -fsyntax-only -Ilibs/color/include test/color_cxx_check.cpp

# --- Host-side unit test: KyuzenFS V4 (bcache + extent engine + direktori) ---
# test/kyuzenfs_v4_test.c meng-include kernel/fs/bcache.c dan modul
# kfs_*.c langsung; ATA/heap/spinlock di-mock ke RAM.
# Jalan dengan: make test-kyuzenfs-v4
.PHONY: test-kyuzenfs-v4
test-kyuzenfs-v4: test/kyuzenfs_v4_test
	./test/kyuzenfs_v4_test

KFS4_SRCS = kernel/fs/bcache.c kernel/fs/kfs_super.c kernel/fs/kfs_balloc.c \
            kernel/fs/kfs_inode.c kernel/fs/kfs_extent.c kernel/fs/kfs_dir.c \
            kernel/fs/kfs_vnode.c kernel/fs/kfs_shim.c
KFS4_HDRS = include/kyuzenfs_v4.h include/vnode.h include/bcache.h include/ata.h \
            include/kyuzenfs.h kernel/fs/kfs_internal.h

test/kyuzenfs_v4_test: test/kyuzenfs_v4_test.c $(KFS4_HDRS) $(KFS4_SRCS)
	$(HOSTCC) -O1 -Wall -iquote test -iquote include -o $@ test/kyuzenfs_v4_test.c

# --- Host tool: formatter disk KyuzenFS V4 (Modul 2) ---
# Format disk.img dari host sebelum boot: make mkfs && ./mkfs.kyuzenfs disk.img
.PHONY: mkfs
mkfs: mkfs.kyuzenfs

mkfs.kyuzenfs: tools/mkfs.kyuzenfs.c include/kyuzenfs_v4.h
	$(HOSTCC) -O2 -Wall -Wextra -iquote include -o $@ tools/mkfs.kyuzenfs.c

# Binary host test (test/*.exe) semuanya generated — `make clean` memanggil ini.
.PHONY: clean-tool
clean-tool:
	rm -f mkfs.kyuzenfs testimg.img test/color_utils_host.o
	rm -f test/*.exe test/kyuzenfs_v4_test test/kyuzenfs_xcheck test/panic_test \
	      test/ata_devmodel_test test/color_test test/textedit_test test/libui_theme_test

# ==========================================
# LLVM libc 22.1.8 — freestanding x86_64 (Phase 0)
# ==========================================
# Cross build LLVM libc (third_party/stdlib/llvm-project @ tag llvmorg-22.1.8)
# untuk triple KyuzenOS + link smoke test. Referensi desain:
# docs/design/audit-llvm-libc-22-freestanding.md.
#
# Opt-in dan modular: TIDAK ada kernel/aplikasi yang di-link ke archive ini.
# Artifact hanya hidup di build/libc/ (gitignored, ikut `make clean`).
#
# Prasyarat host: cmake + make/sh (msys2) di PATH — generator "Unix Makefiles",
# ninja tidak diperlukan; python3 + pyyaml untuk hdrgen LLVM. Bila `python`
# bukan interpreter yang punya pyyaml, set LIBC_PYTHON_EXE.
#
#   make libc-phase0
#   make libc-phase0 LIBC_PYTHON_EXE=E:/Tools/Language/Python/python.exe
LIBC_SRC       = third_party/stdlib/llvm-project
LIBC_TRIPLE    = x86_64-pc-none-elf
LIBC_OUT       = $(BUILD_DIR)/libc
LIBC_CMAKE_DIR = $(LIBC_OUT)/cmake
LIBC_INCLUDE   = $(LIBC_CMAKE_DIR)/libc/include
LIBC_ARCHIVE   = $(LIBC_OUT)/$(LIBC_TRIPLE)/lib/libc.a
LIBC_CACHE     = $(LIBC_SRC)/libc/cmake/caches/$(LIBC_TRIPLE).cmake
LIBC_CFG_DIR   = $(LIBC_SRC)/libc/config/baremetal/x86_64
LIBC_CC        = clang
LIBC_CXX       = clang++
LIBC_LD        = ld.lld
LIBC_NM        = llvm-nm
LIBC_OBJDUMP   = llvm-objdump
LIBC_JOBS     ?= 8
LIBC_PYTHON_EXE ?=
LIBC_PYTHON_ARG = $(if $(LIBC_PYTHON_EXE),-DPython3_EXECUTABLE=$(LIBC_PYTHON_EXE),)
LIBC_TARGET_FLAGS = --target=$(LIBC_TRIPLE) -ffreestanding -nostdlib -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
LIBC_SMOKE_SRC = tools/libc-phase0/smoke.c
LIBC_SMOKE_OBJ = $(LIBC_OUT)/smoke.o
LIBC_SMOKE_ELF = $(LIBC_OUT)/smoke.elf

.PHONY: libc-phase0
libc-phase0: $(LIBC_SMOKE_ELF)
	@echo "[libc] archive : $(LIBC_ARCHIVE)"
	@echo "[libc] headers : $(LIBC_INCLUDE)"
	@echo "[libc] smoke   : $(LIBC_SMOKE_ELF) (entry phase0_entry)"

# Konfigurasi ulang otomatis bila cache/config berubah (ninja tidak dipakai).
$(LIBC_CMAKE_DIR)/Makefile: $(LIBC_CACHE) $(LIBC_CFG_DIR)/entrypoints.txt $(LIBC_CFG_DIR)/headers.txt
	"$(CMAKE)" -S $(LIBC_SRC)/runtimes -B $(LIBC_CMAKE_DIR) -G "Unix Makefiles" \
		-C $(LIBC_CACHE) \
		-DCMAKE_C_COMPILER=$(LIBC_CC) -DCMAKE_CXX_COMPILER=$(LIBC_CXX) \
		-DCMAKE_BUILD_TYPE=Release $(LIBC_PYTHON_ARG)

$(LIBC_ARCHIVE): $(LIBC_CMAKE_DIR)/Makefile
	"$(CMAKE)" --build $(LIBC_CMAKE_DIR) --target libc -- -j$(LIBC_JOBS)
	mkdir -p $(dir $(LIBC_ARCHIVE))
	cp $(LIBC_CMAKE_DIR)/libc/lib/libc.a $(LIBC_ARCHIVE)

$(LIBC_SMOKE_OBJ): $(LIBC_SMOKE_SRC) $(LIBC_ARCHIVE)
	mkdir -p $(LIBC_OUT)
	$(LIBC_CC) $(LIBC_TARGET_FLAGS) -I$(LIBC_INCLUDE) -c $< -o $@

# Link-only smoke test: header + archive + linkage untuk fungsi pure. Tidak ada
# vendor hook, tidak ada _start (entry = phase0_entry), -nostdlib, jadi setiap
# simbol wajib datang dari libc.a.
$(LIBC_SMOKE_ELF): $(LIBC_SMOKE_OBJ) $(LIBC_ARCHIVE)
	$(LIBC_LD) -m elf_x86_64 -nostdlib --entry=phase0_entry -o $@ $(LIBC_SMOKE_OBJ) $(LIBC_ARCHIVE)
	@for sym in memcpy memset memcmp strlen strcmp isalpha isdigit; do \
		$(LIBC_NM) --defined-only $@ | grep -qE "[TtWw] $$sym$$" || { echo "[libc] FAIL: $$sym tidak resolve dari libc.a"; exit 1; }; \
	done
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[libc] FAIL: masih ada simbol undefined di smoke.elf"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[libc] smoke OK: 7/7 simbol pure dari libc.a, 0 undefined symbol"

.PHONY: libc-clean
libc-clean:
	rm -rf $(LIBC_OUT)

# ==========================================
# LLVM libc 22.1.8 — Phase 1: port layer (exit + errno + malloc)
# ==========================================
# Port layer `libs/libc-port/src/kyuzen_libc_port.cpp` di-compile dengan flag & 
# namespace internal libc 22.1.8 (LIBC_NAMESPACE) supaya bisa menggantikan
# instance `freelist_heap` bawaan libc; backing memory-nya datang dari syscall
# #9 (uheap Kyuzen), bukan simbol linker _end/__llvm_libc_heap_limit.
#
#   make libc-phase1        → build app ELF statis (libc + port)
#   make libc-phase1-qemu   → jalankan smoke test otomatis di QEMU
#
# Disk image untuk test DIBUAT TERPISAH (build/libc/phase1-disk.img): disk.img
# milik user tidak pernah disentuh.
LIBC_PORT_SRC    = libs/libc-port/src/kyuzen_libc_port.cpp
LIBC_PORT_OBJ    = $(LIBC_OUT)/port/kyuzen_libc_port.o
LIBC_PORT_DEFS   = -DLIBC_NAMESPACE=__llvm_libc_22_1_8_ -DLIBC_FULL_BUILD -DLIBC_TARGET_OS_IS_BAREMETAL \
                   -DLIBC_ERRNO_MODE=LIBC_ERRNO_MODE_EXTERNAL -DLIBC_THREAD_MODE=LIBC_THREAD_MODE_SINGLE \
                   -DLIBC_COPT_PUBLIC_PACKAGING
LIBC_PORT_CFLAGS = $(LIBC_TARGET_FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -O2 -fno-builtin \
                   -fno-unwind-tables -fno-asynchronous-unwind-tables -fvisibility-inlines-hidden \
                   -I$(LIBC_SRC)/libc -I$(LIBC_CMAKE_DIR)/libc -isystem $(LIBC_INCLUDE) $(LIBC_PORT_DEFS)
LIBC_PHASE1_SRC  = tools/libc-phase1/libc_phase1.c
LIBC_PHASE1_LD   = tools/libc-phase1/libc_app.ld
LIBC_PHASE1_OBJ  = $(LIBC_OUT)/phase1_app.o
LIBC_PHASE1_APP  = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/libc_phase1.elf
# Varian limine.conf untuk ISO smoke test: isi repo + satu module app Phase 1.
# Namanya harus `limine.conf` (Limine mencari nama itu di root ISO), jadi
# digenerate di subdirektori sendiri.
LIBC_PHASE1_CONF = $(LIBC_OUT)/iso/limine.conf

.PHONY: libc-phase1
libc-phase1: $(LIBC_PHASE1_APP)
	@echo "[libc] phase1 app : $(LIBC_PHASE1_APP)"

$(LIBC_PORT_OBJ): $(LIBC_PORT_SRC) $(LIBC_ARCHIVE)
	mkdir -p $(dir $@)
	$(LIBC_CXX) $(LIBC_PORT_CFLAGS) -c $< -o $@

$(LIBC_PHASE1_OBJ): $(LIBC_PHASE1_SRC) $(LIBC_ARCHIVE)
	mkdir -p $(LIBC_OUT)
	$(LIBC_CC) $(LIBC_TARGET_FLAGS) -I$(LIBC_INCLUDE) -c $< -o $@

$(LIBC_PHASE1_APP): $(LIBC_PHASE1_OBJ) $(LIBC_PORT_OBJ) $(LIBC_ARCHIVE) $(LIBC_PHASE1_LD)
	mkdir -p $(dir $@)
	$(LIBC_LD) -m elf_x86_64 -nostdlib -T $(LIBC_PHASE1_LD) -o $@ $(LIBC_PHASE1_OBJ) $(LIBC_PORT_OBJ) $(LIBC_ARCHIVE)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[libc] FAIL: _start tidak ada di app Phase 1"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[libc] FAIL: masih ada simbol undefined di app Phase 1"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[libc] phase1 link OK: $(notdir $@) (entry _start, 0 undefined)"

# Smoke test QEMU otomatis. Keystroke dikirim lewat monitor QEMU (sendkey); bukti
# diambil dari COM1 (TTY di-mirror ke serial oleh drivers/tty.c). Disk uji = image
# nol mentah (kernel memformat sendiri saat boot) — lihat tools/libc-phase1/run-qemu.sh.
#   make libc-phase1-qemu [QEMU=/path/ke/qemu-system-x86_64.exe]
$(LIBC_PHASE1_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test LLVM libc 22.1.8 Phase 1 (hanya ada di ISO uji).\n    module_path: boot():/libc_phase1.elf\n    module_string: libc_phase1.elf\n\n' >> $@

.PHONY: libc-phase1-qemu
libc-phase1-qemu: $(LIBC_PHASE1_APP) $(LIBC_PHASE1_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(LIBC_PHASE1_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase1/run-qemu.sh
	@echo "--- bukti serial [phase1] ---"; grep "\[phase1\]" $(LIBC_OUT)/phase1-serial.log || true
	@grep -q "\[phase1\] PASS" $(LIBC_OUT)/phase1-serial.log || { echo "[libc] FAIL: [phase1] PASS tidak terlihat di serial"; exit 1; }

# ==========================================
# LLVM libc 22.1.8 — Phase 2: stdio (console I/O via #48/#49)
# ==========================================
# Hook `__llvm_libc_stdio_read/write` + 3 cookie di port layer memetakan
# stdout/stderr/stdin LLVM ke fd 1/2/0 Kyuzen (int 0x80 #49/#48). Entrypoint
# stdio baremetal yang diaktifkan ada di libc/config/baremetal/x86_64/
# entrypoints.txt (printf/fprintf/snprintf/sprintf/puts/putchar/fwrite/fread
# + varian v-/f- yang didukung backend baremetal 22.1.8).
#
#   make libc-phase2        → build app ELF statis (libc + port + stdio)
#   make libc-phase2-qemu   → jalankan smoke test otomatis di QEMU
#
LIBC_PHASE2_SRC  = tools/libc-phase2/libc_phase2.c
LIBC_PHASE2_LD   = tools/libc-phase2/libc_app.ld
LIBC_PHASE2_OBJ  = $(LIBC_OUT)/phase2_app.o
LIBC_PHASE2_APP  = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/libc_phase2.elf
# Varian limine.conf untuk ISO smoke test Phase 2 (direktori sendiri supaya
# tidak bentrok dengan varian Phase 1: keduanya bernama `limine.conf`).
LIBC_PHASE2_CONF = $(LIBC_OUT)/iso2/limine.conf

.PHONY: libc-phase2
libc-phase2: $(LIBC_PHASE2_APP)
	@echo "[libc] phase2 app : $(LIBC_PHASE2_APP)"

$(LIBC_PHASE2_OBJ): $(LIBC_PHASE2_SRC) $(LIBC_ARCHIVE)
	mkdir -p $(LIBC_OUT)
	$(LIBC_CC) $(LIBC_TARGET_FLAGS) -I$(LIBC_INCLUDE) -c $< -o $@

$(LIBC_PHASE2_APP): $(LIBC_PHASE2_OBJ) $(LIBC_PORT_OBJ) $(LIBC_ARCHIVE) $(LIBC_PHASE2_LD)
	mkdir -p $(dir $@)
	$(LIBC_LD) -m elf_x86_64 -nostdlib -T $(LIBC_PHASE2_LD) -o $@ $(LIBC_PHASE2_OBJ) $(LIBC_PORT_OBJ) $(LIBC_ARCHIVE)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[libc] FAIL: _start tidak ada di app Phase 2"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[libc] FAIL: masih ada simbol undefined di app Phase 2"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[libc] phase2 link OK: $(notdir $@) (entry _start, 0 undefined)"

$(LIBC_PHASE2_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test LLVM libc 22.1.8 Phase 2 (hanya ada di ISO uji).\n    module_path: boot():/libc_phase2.elf\n    module_string: libc_phase2.elf\n\n' >> $@

.PHONY: libc-phase2-qemu
libc-phase2-qemu: $(LIBC_PHASE2_APP) $(LIBC_PHASE2_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(LIBC_PHASE2_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase2/run-qemu.sh
	@echo "--- bukti serial [phase2] ---"; grep "\[phase2\]" $(LIBC_OUT)/phase2-serial.log || true
	@grep -q "\[phase2\] PASS" $(LIBC_OUT)/phase2-serial.log || { echo "[libc] FAIL: [phase2] PASS tidak terlihat di serial"; exit 1; }

# ==========================================
# LLVM libc 22.1.8 — Phase 4: C runtime completeness (time + utils)
# ==========================================
# Waktu (hook __llvm_libc_timespec_get_active/utc → syscall #14/#20) +
# utilitas murni-userspace (strtol-family, qsort/bsearch, rand/srand,
# abs/div-family, strdup/strndup, aligned_alloc). Klasifikasi per API +
# backend mapping: docs/design/audit-llvm-libc-22-freestanding.md §16.
#
# App smoke DIBANGUN VIA SDK (build/sdk/c, pola Phase 3) — sekaligus bukti
# SDK mengekspos runtime baru. Bergantung pada $(SDK_STAGE) sehingga
# `make sdk-c` otomatis memakai libc baru.
#
#   make libc-phase4        → build app ELF statis via SDK
#   make libc-phase4-qemu   → jalankan smoke test otomatis di QEMU
#
LIBC_PHASE4_SRC  = tools/libc-phase4/sdk_smoke.c
LIBC_PHASE4_OBJ  = $(LIBC_OUT)/phase4_app.o
LIBC_PHASE4_APP  = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/libc_phase4.elf
# Varian limine.conf untuk ISO smoke test Phase 4 (direktori sendiri supaya
# tidak bentrok dengan varian Phase 1/2/3).
LIBC_PHASE4_CONF = $(LIBC_OUT)/iso4/limine.conf

.PHONY: libc-phase4
libc-phase4: $(LIBC_PHASE4_APP)
	@echo "[libc] phase4 app : $(LIBC_PHASE4_APP)"

# Kompilasi MURNI via SDK (guard anti-third_party = batas ergonomi Phase 3).
$(LIBC_PHASE4_OBJ): $(LIBC_PHASE4_SRC) $(SDK_STAGE)
	@if grep -q "third_party" $(LIBC_PHASE4_SRC); then echo "[libc] FAIL: phase4 app menyebut third_party (bocor ke internal LLVM)"; exit 1; fi
	@mkdir -p $(LIBC_OUT)
	$(LIBC_CC) $(SDK_CFLAGS) -isystem $(SDK_INC) -c $< -o $@

# Link MURNI via SDK: crt + libc + ld semuanya dari build/sdk/c.
$(LIBC_PHASE4_APP): $(LIBC_PHASE4_OBJ) $(SDK_STAGE)
	@mkdir -p $(dir $@)
	$(LIBC_LD) -m elf_x86_64 -nostdlib -T $(SDK_LD) -o $@ $(LIBC_PHASE4_OBJ) $(SDK_CRT) $(SDK_LIB)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[libc] FAIL: _start tidak ada di app Phase 4"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[libc] FAIL: masih ada simbol undefined di app Phase 4"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[libc] phase4 link OK: $(notdir $@) (entry _start, 0 undefined)"

$(LIBC_PHASE4_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test LLVM libc 22.1.8 Phase 4 (hanya ada di ISO uji).\n    module_path: boot():/libc_phase4.elf\n    module_string: libc_phase4.elf\n\n' >> $@

.PHONY: libc-phase4-qemu
libc-phase4-qemu: $(LIBC_PHASE4_APP) $(LIBC_PHASE4_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(LIBC_PHASE4_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase4/run-qemu.sh
	@echo "--- bukti serial [phase4] ---"; grep "\[phase4\]" $(LIBC_OUT)/phase4-serial.log || true
	@grep -q "\[phase4\] PASS" $(LIBC_OUT)/phase4-serial.log || { echo "[libc] FAIL: [phase4] PASS tidak terlihat di serial"; exit 1; }

# ==========================================
# Kyuzen C++ SDK — Phase 5 (runtime/foundation) + Phase 6 (libc++ subset)
# ==========================================
# Boundary publik C++ di atas C SDK: yang di-commit hanya sumber boundary
# (sdk/cpp/linker/app.ld + sdk/cpp/include/__config_site +
#  sdk/cpp/include/__assertion_handler + sdk/cpp/README.md);
# yang di-generate di-stage ke build/sdk/cpp (gitignored) lewat `make sdk-cpp`:
#
#   build/sdk/cpp/include/       ← CLOSURE header libc++ (8 header publik +
#                                  internal __* yang mereka butuhkan; disalin
#                                  dari tree LLVM, di-rebase ke root include)
#   build/sdk/cpp/include/__config_site + __assertion_handler ← milik Kyuzen
#   build/sdk/cpp/cxxrt.o        ← runtime C++ Phase 5 (new/delete, guard,
#                                  __dso_handle, pure_virtual, cxa_atexit/
#                                  finalize; dari libs/libc-port, BUKAN LLVM)
#   build/sdk/cpp/lib/libcxxrt.a ← runtime libc++ Phase 6 (subset .cpp persis
#                                  audit §18; BUKAN seluruh libc++)
#   build/sdk/cpp/linker/app.ld  ← salinan linker script C++ (+ .init_array)
#
# CRT (_start + heap + init walk) dan libc.a dipakai ulang dari build/sdk/c
# (TIDAK diduplikasi). __cxa_atexit/finalize milik cxxrt.o (versi libc.a hanya
# memfinalisasi dso==NULL sementara clang mendaftar dso=&__dso_handle —
# bila dipakai, dtor global tak pernah jalan; lihat audit §17).
#
#   make sdk-cpp             → stage SDK C++ (+guard anti-stale)
#   make sdk-cpp-smoke       → app uji Phase 5 murni lewat SDK C++
#   make sdk-cpp-smoke-qemu  → QEMU Phase 5 (harap PASS + dtor)
#   make libc-phase5         → alias menu Phase 5
#   make libc-phase6         → app uji Phase 6 (libc++ subset) via SDK C++
#   make libc-phase6-qemu    → QEMU Phase 6 (harap [phase6] PASS)
#   make cpp-app             → contoh hello via wrapper publik `kyuzen-c++`
#   make cpp-app-run         → QEMU contoh hello (harap Hello from Kyuzen C++)
#   make cpp-examples        → semua contoh examples/cpp via wrapper publik
#   make libc-phase7         → smoke SDK publik Phase 7 via wrapper
#   make libc-phase7-qemu    → QEMU Phase 7 (harap [phase7] PASS)
#
SDK_CPP_SRC_DIR = sdk/cpp
SDK_CPP_DIR     = $(BUILD_DIR)/sdk/cpp
SDK_CPP_INC     = $(SDK_CPP_DIR)/include
SDK_CPP_CXXRT   = $(SDK_CPP_DIR)/cxxrt.o
SDK_CPP_LD      = $(SDK_CPP_DIR)/linker/app.ld
SDK_CPP_LD_SRC  = $(SDK_CPP_SRC_DIR)/linker/app.ld
SDK_CPP_WRAPPER_SRC = $(SDK_CPP_SRC_DIR)/bin/kyuzen-c++
SDK_CPP_WRAPPER = $(SDK_CPP_DIR)/bin/kyuzen-c++
SDK_CPP_SITE_FILES = $(SDK_CPP_SRC_DIR)/include/__config_site \
                     $(SDK_CPP_SRC_DIR)/include/__assertion_handler
SDK_CPP_PUBLIC_HEADERS = $(SDK_CPP_SRC_DIR)/include/kyuzen/config.hpp \
                         $(SDK_CPP_SRC_DIR)/include/kyuzen/app.hpp \
                         $(SDK_CPP_SRC_DIR)/include/kyuzen/panic.hpp
# ---- Phase 8: header publik libdesktop (sumber kanonis di libs/, BUKAN
# duplikat di sdk/cpp — stage menyalinnya ke kyuzen/desktop/) ----
LIBDESKTOP_SRC_DIR = libs/libdesktop
LIBDESKTOP_INC_SRC = $(LIBDESKTOP_SRC_DIR)/include
LIBDESKTOP_PUBLIC_HEADERS = $(wildcard $(LIBDESKTOP_INC_SRC)/kyuzen/desktop/*.hpp)
SDK_CPP_STAGE   = $(SDK_CPP_DIR)/.staged
# Flag kanonis app C++: flag C SDK + C++ (-nostdinc++ agar hermetis dari
# libc++ host; <stddef.h> tetap dari header freestanding clang).
SDK_CXXFLAGS    = $(LIBC_TARGET_FLAGS) -O2 -std=c++17 -fno-exceptions -fno-rtti -nostdinc++
LIBC_CXXRT_SRC  = libs/libc-port/src/kyuzen_cxx_runtime.cpp
LIBC_CXXRT_OBJ  = $(LIBC_OUT)/port/kyuzen_cxx_runtime.o
SDK_CPP_SMOKE_SRC = tools/libc-phase5/sdk_smoke.cpp
SDK_CPP_SMOKE_OBJ = $(LIBC_OUT)/sdk_cpp_smoke.o
SDK_CPP_SMOKE_APP = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/libc_phase5.elf
# Varian limine.conf untuk ISO smoke test C++ (direktori sendiri).
SDK_CPP_SMOKE_CONF = $(LIBC_OUT)/iso5/limine.conf
# ---- Phase 6: libc++ subset (audit §18) ----
LIBCXX_SRC_DIR  = third_party/stdlib/llvm-project/libcxx
LIBCXX_INCLUDE  = $(LIBCXX_SRC_DIR)/include
# Header publik yang diaktifkan (tugas §9 + new untuk runtime + functional
# sebagai dependensi build <algorithm>: __sort memakai ranges::less).
# functional di-stage karena dibutuhkan kompilasi, BUKAN API Phase 6 yang
# didukung (lihat guard denylist smoke + audit §18).
LIBCXX_PUBLIC_HEADERS = array algorithm memory string string_view type_traits utility vector new functional
# Source libc++ yang dikompilasi (tepat, bukan seluruh src/):
#   stdexcept.cpp    — kelas exception + __throw_* inline backend (verbose_abort)
#   verbose_abort.cpp— __libcpp_verbose_abort (vfprintf+abort; fail keras)
# string.cpp + algorithm.cpp SENGAJA absen (§18): string.cpp (stof/stod return
# FP) dan instantiasi sort float/double/long-double di algorithm.cpp tak bisa
# dikompilasi -mno-sse (clang menolak / mengemisikan x87). Penggantinya: slice
# instantiation milik Kyuzen (verbatim hulu, hanya varian FP-free) — bukan
# implementasi baru, hanya slicing TU.
LIBCXXRT_SRCS   = src/stdexcept.cpp src/verbose_abort.cpp
LIBCXXRT_OBJDIR = $(LIBC_OUT)/libcxxrt
LIBCXXRT_OBJS   = $(patsubst src/%.cpp,$(LIBCXXRT_OBJDIR)/%.o,$(LIBCXXRT_SRCS)) \
                  $(LIBCXXRT_OBJDIR)/shims.o $(LIBCXXRT_OBJDIR)/string_inst.o \
                  $(LIBCXXRT_OBJDIR)/sort_inst.o
LIBCXXRT_ARCHIVE = $(SDK_CPP_DIR)/lib/libcxxrt.a
LIBC_AR         = llvm-ar
# Shim milik Kyuzen (libs/, BUKAN tree LLVM): stub abort() untuk strtof/
# strtod/strtold yang direferensikan string.cpp tetapi di luar subset C
# (tipe FP tak bisa dikompilasi -mno-sse; lihat file sumbernya).
LIBCXXRT_SHIM_SRC = libs/libc-port/src/kyuzen_libcxx_shims.cpp
$(LIBCXXRT_OBJDIR)/shims.o: $(LIBCXXRT_SHIM_SRC) $(SDK_STAGE) $(LIBCXXRT_PROLOGUE)
	@mkdir -p $(dir $@)
	$(LIBC_CXX) $(LIBCXXRT_FLAGS) -c $< -o $@
# Slice instantiation basic_string<char> (TU milik Kyuzen, §18): kompilasi
# dengan standar SAMA seperti app (C++17) agar instantiation persis cocok
# dengan yang diharapkan extern-template declarations sisi app.
LIBCXXRT_STRING_INST_SRC = libs/libc-port/src/kyuzen_libcxx_string_inst.cpp
$(LIBCXXRT_OBJDIR)/string_inst.o: $(LIBCXXRT_STRING_INST_SRC) $(SDK_STAGE) $(SDK_CPP_SITE_FILES)
	@mkdir -p $(dir $@)
	$(LIBC_CXX) $(LIBCXX_BUILD_FLAGS) -c $< -o $@
# Slice instantiation __sort non-FP (TU milik Kyuzen, §18): butuh C++20
# (ranges::less) seperti algorithm.cpp hulu; simbol yang diekspor
# ABI-nya sama untuk app C++17.
LIBCXXRT_SORT_INST_SRC = libs/libc-port/src/kyuzen_libcxx_sort_inst.cpp
$(LIBCXXRT_OBJDIR)/sort_inst.o: $(LIBCXXRT_SORT_INST_SRC) $(SDK_STAGE) $(SDK_CPP_SITE_FILES)
	@mkdir -p $(dir $@)
	$(LIBC_CXX) $(LIBCXXRT_FLAGS) -c $< -o $@
# Flag build DARI tree LLVM (input build, BUKAN boundary app): site config
# milik Kyuzen dulu, lalu header libc++ tree, lalu header C SDK.
# _LIBCPP_BUILDING_LIBRARY + AVAILABILITY_MINIMUM_HEADER_VERSION=2 = tepat
# yang dilakukan CMake hulu saat mengompilasi library (tanpanya, deklarasi
# legacy-ABI di header vs definisi di .cpp tak konsisten — ditemukan audit).
LIBCXX_BUILD_FLAGS = $(LIBC_TARGET_FLAGS) -std=c++17 -fno-exceptions -fno-rtti -nostdinc++ -O2 -fno-builtin \
	-D_LIBCPP_BUILDING_LIBRARY -D_LIBCPP_AVAILABILITY_MINIMUM_HEADER_VERSION=2 \
	-isystem $(SDK_CPP_SRC_DIR)/include -isystem $(LIBCXX_INCLUDE) -isystem $(SDK_INC)
LIBC_PHASE6_SRC  = tools/libc-phase6/sdk_smoke.cpp
LIBC_PHASE6_OBJ  = $(LIBC_OUT)/phase6_app.o
LIBC_PHASE6_APP  = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/libc_phase6.elf
LIBC_PHASE6_CONF = $(LIBC_OUT)/iso6/limine.conf

.PHONY: sdk-cpp
sdk-cpp: $(SDK_CPP_STAGE)
	@echo "[sdk-cpp] staged : $(SDK_CPP_DIR)"

.PHONY: libc-phase5
libc-phase5: $(SDK_CPP_SMOKE_APP)
	@echo "[libc] phase5 app : $(SDK_CPP_SMOKE_APP)"

.PHONY: libc-phase6
libc-phase6: $(LIBC_PHASE6_APP)
	@echo "[libc] phase6 app : $(LIBC_PHASE6_APP)"

# Runtime C++ Phase 5: freestanding biasa (malloc/free dari header C SDK,
# <new>/__config dari tree libc++ + site config Kyuzen). Prereq SDK_STAGE =
# header C sudah ada. apps TIDAK melihat path tree ini (hanya staged).
$(LIBC_CXXRT_OBJ): $(LIBC_CXXRT_SRC) $(SDK_STAGE) $(SDK_CPP_SITE_FILES)
	@mkdir -p $(dir $@)
	$(LIBC_CXX) $(LIBCXX_BUILD_FLAGS) -c $< -o $@

# Runtime libc++ Phase 6: tepat file audit §18, satu rule pola.
# Prologue (-include) mendeklarasikan strtof/strtod/strtold untuk TU libc++
# (dibutuhkan string.cpp; definisi = stub abort di shims.o).
# -std=c++20 HANYA untuk TU archive (bukan app): algorithm.cpp memakai
# ranges::less (butuh C++20); hulu mengompilasi library sekali sebagai
# standar terbaru untuk semua standar app — model yang sama. Simbol yang
# diekspor (__sort untuk iterator char/int) ABI-nya tak terpengaruh versi
# standar TU. App tetap -std=c++17 (SDK_CXXFLAGS tak berubah).
LIBCXXRT_PROLOGUE = libs/libc-port/src/kyuzen_libcxx_prologue.h
LIBCXXRT_FLAGS = $(LIBCXX_BUILD_FLAGS) -std=c++20 -include $(LIBCXXRT_PROLOGUE)
$(LIBCXXRT_OBJDIR)/%.o: $(LIBCXX_SRC_DIR)/src/%.cpp $(SDK_STAGE) $(SDK_CPP_SITE_FILES) $(LIBCXXRT_PROLOGUE)
	@mkdir -p $(dir $@)
	$(LIBC_CXX) $(LIBCXXRT_FLAGS) -c $< -o $@

$(LIBCXXRT_ARCHIVE): $(LIBCXXRT_OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(LIBC_AR) rcs $@ $(LIBCXXRT_OBJS)
	@echo "[libcxxrt] archive : $@ ($$(llvm-ar t $@ | wc -l) member)"

# Satu stage atomik: closure header + site files + cxxrt.o + libcxxrt.a + ld.
# Closure dihitung OTOMATIS via clang -M per header publik (bukan daftar
# manual): hanya file di bawah libcxx/include yang disalin, di-rebase ke
# root staged include. Bila tree LLVM berubah, closure mengikuti tanpa edit
# Makefile — dan -M yang gagal membuat stage ikut gagal (keras, bukan diam).
$(SDK_CPP_STAGE): $(LIBC_CXXRT_OBJ) $(LIBCXXRT_ARCHIVE) $(SDK_CPP_LD_SRC) $(SDK_CPP_WRAPPER_SRC) $(SDK_STAGE) $(SDK_CPP_SITE_FILES) $(SDK_CPP_PUBLIC_HEADERS) $(LIBDESKTOP_PUBLIC_HEADERS)
	@mkdir -p $(SDK_CPP_INC) $(dir $(SDK_CPP_CXXRT)) $(dir $(SDK_CPP_LD))
	@rm -rf $(SDK_CPP_INC)
	@mkdir -p $(SDK_CPP_INC)
	@cp $(SDK_CPP_SITE_FILES) $(SDK_CPP_INC)/
	@mkdir -p $(SDK_CPP_INC)/kyuzen
	@cp $(SDK_CPP_PUBLIC_HEADERS) $(SDK_CPP_INC)/kyuzen/
	@mkdir -p $(SDK_CPP_INC)/kyuzen/desktop
	@cp $(LIBDESKTOP_PUBLIC_HEADERS) $(SDK_CPP_INC)/kyuzen/desktop/
	@mkdir -p $(dir $(SDK_CPP_WRAPPER))
	@cp $(SDK_CPP_WRAPPER_SRC) $(SDK_CPP_WRAPPER)
	@chmod +x $(SDK_CPP_WRAPPER)
	@for h in $(LIBCXX_PUBLIC_HEADERS); do \
		echo "#include <$$h>" | $(LIBC_CXX) $(LIBCXX_BUILD_FLAGS) -x c++ -M -MT x - 2>/dev/null | tr ' ' '\n' | grep "^$(LIBCXX_INCLUDE)/" | sed 's|\\$$||'; \
	done | sort -u | while read f; do \
		rel=$${f#$(LIBCXX_INCLUDE)/}; \
		mkdir -p $(SDK_CPP_INC)/$$(dirname $$rel); \
		cp $$f $(SDK_CPP_INC)/$$rel; \
	done
	@for h in $(LIBCXX_PUBLIC_HEADERS); do \
		test -f $(SDK_CPP_INC)/$$h || { echo "[sdk-cpp] FAIL: header <$$h> tak ter-stage"; exit 1; }; \
	done
	@test -f $(SDK_CPP_INC)/__config_site -a -f $(SDK_CPP_INC)/__assertion_handler || { echo "[sdk-cpp] FAIL: site files tak ter-stage"; exit 1; }
	@for h in config.hpp app.hpp panic.hpp; do \
		test -f $(SDK_CPP_INC)/kyuzen/$$h || { echo "[sdk-cpp] FAIL: header publik <kyuzen/$$h> tak ter-stage"; exit 1; }; \
	done
	@test -n "$(LIBDESKTOP_PUBLIC_HEADERS)" || { echo "[sdk-cpp] FAIL: header publik libdesktop kosong (libs/libdesktop/include/kyuzen/desktop/*.hpp hilang?)"; exit 1; }
	@for h in $(notdir $(LIBDESKTOP_PUBLIC_HEADERS)); do \
		test -f $(SDK_CPP_INC)/kyuzen/desktop/$$h || { echo "[sdk-cpp] FAIL: header publik <kyuzen/desktop/$$h> tak ter-stage"; exit 1; }; \
	done
	@test -x $(SDK_CPP_WRAPPER) || { echo "[sdk-cpp] FAIL: wrapper kyuzen-c++ tak ter-stage executable"; exit 1; }
	@cp $(LIBC_CXXRT_OBJ) $(SDK_CPP_CXXRT)
	@cp $(SDK_CPP_LD_SRC) $(SDK_CPP_LD)
	@$(LIBC_NM) --defined-only $(SDK_CPP_CXXRT) | grep -qE " [Tt] _Znwm$$" || { echo "[sdk-cpp] FAIL: cxxrt.o tanpa operator new"; exit 1; }
	@$(LIBC_NM) --defined-only $(SDK_CPP_CXXRT) | grep -qE " [Tt] __cxa_guard_acquire$$" || { echo "[sdk-cpp] FAIL: cxxrt.o tanpa __cxa_guard_acquire"; exit 1; }
	@$(LIBC_NM) --defined-only $(SDK_CPP_CXXRT) | grep -qE " [BbDd] __dso_handle$$" || { echo "[sdk-cpp] FAIL: cxxrt.o tanpa __dso_handle"; exit 1; }
	@$(LIBC_NM) --defined-only $(LIBCXXRT_ARCHIVE) | grep -q "__libcpp_verbose_abort" || { echo "[sdk-cpp] FAIL: libcxxrt.a tanpa __libcpp_verbose_abort (verbose_abort.cpp basi?)"; exit 1; }
	@$(LIBC_NM) --defined-only $(LIBCXXRT_ARCHIVE) | grep -q "_ZNSt11logic_errorC1EPKc" || { echo "[sdk-cpp] FAIL: libcxxrt.a tanpa logic_error (stdexcept.cpp basi?)"; exit 1; }
	@touch $@
	@echo "[sdk-cpp] stage OK: include/closure + kyuzen/ + kyuzen-c++ + cxxrt.o + libcxxrt.a + app.ld"

# Ordering eksplisit untuk file TANPA rule sendiri (salinan cp): relink bila
# stage berubah. (LIBCXXRT_ARCHIVE punya rule ar sendiri — di luar sini agar
# tak sirkular dengan SDK_CPP_STAGE yang membutuhkannya sebagai prereq.)
$(SDK_CPP_CXXRT) $(SDK_CPP_LD) $(SDK_CPP_WRAPPER): $(SDK_CPP_STAGE)
	@:

.PHONY: sdk-cpp-smoke
sdk-cpp-smoke: $(SDK_CPP_SMOKE_APP)
	@echo "[sdk-cpp] smoke app : $(SDK_CPP_SMOKE_APP)"

# Kompilasi MURNI via SDK C++ (guard anti-third_party + anti-libc++ host).
$(SDK_CPP_SMOKE_OBJ): $(SDK_CPP_SMOKE_SRC) $(SDK_CPP_STAGE)
	@if grep -q "third_party" $(SDK_CPP_SMOKE_SRC); then echo "[sdk-cpp] FAIL: smoke app menyebut third_party (bocor ke internal LLVM)"; exit 1; fi
	@if grep -qE "#include <(string|vector|iostream|exception|typeinfo|memory|utility|tuple|array)>" $(SDK_CPP_SMOKE_SRC); then echo "[sdk-cpp] FAIL: smoke app memakai header libc++ (di luar Phase 5)"; exit 1; fi
	@mkdir -p $(LIBC_OUT)
	$(LIBC_CXX) $(SDK_CXXFLAGS) -isystem $(SDK_CPP_INC) -isystem $(SDK_INC) -c $< -o $@

# Link MURNI via SDK: cxxrt + crt + libc + ld C++. Dependensi file eksplisit
# (bukan hanya .staged) agar relink terjadi bila crt/lib berubah.
$(SDK_CPP_SMOKE_APP): $(SDK_CPP_SMOKE_OBJ) $(SDK_CPP_STAGE) $(SDK_CPP_CXXRT) $(SDK_CRT) $(SDK_LIB)
	@mkdir -p $(dir $@)
	$(LIBC_LD) -m elf_x86_64 -nostdlib -T $(SDK_CPP_LD) -o $@ $(SDK_CPP_SMOKE_OBJ) $(SDK_CPP_CXXRT) $(SDK_CRT) $(SDK_LIB)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[sdk-cpp] FAIL: _start tidak ada di app C++"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[sdk-cpp] FAIL: masih ada simbol undefined di app C++"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[sdk-cpp] smoke link OK: $(notdir $@) (entry _start, 0 undefined)"

$(SDK_CPP_SMOKE_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test Kyuzen C++ SDK Phase 5 (hanya ada di ISO uji).\n    module_path: boot():/libc_phase5.elf\n    module_string: libc_phase5.elf\n\n' >> $@

.PHONY: sdk-cpp-smoke-qemu
sdk-cpp-smoke-qemu: $(SDK_CPP_SMOKE_APP) $(SDK_CPP_SMOKE_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(SDK_CPP_SMOKE_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase5/run-qemu.sh
	@echo "--- bukti serial [phase5] ---"; grep "\[phase5\]" $(LIBC_OUT)/phase5-serial.log || true
	@grep -q "\[phase5\] PASS" $(LIBC_OUT)/phase5-serial.log || { echo "[sdk-cpp] FAIL: [phase5] PASS tidak terlihat di serial"; exit 1; }
	@grep -q "\[phase5\] global dtor ok" $(LIBC_OUT)/phase5-serial.log || { echo "[sdk-cpp] FAIL: dtor global tak jalan (finalisasi rusak)"; exit 1; }

# ---- Phase 6: smoke libc++ subset (via SDK C++ staged, hermetis) ----
.PHONY: libc-phase6-qemu
libc-phase6-qemu: $(LIBC_PHASE6_APP) $(LIBC_PHASE6_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(LIBC_PHASE6_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase6/run-qemu.sh
	@echo "--- bukti serial [phase6] ---"; grep "\[phase6\]" $(LIBC_OUT)/phase6-serial.log || true
	@grep -q "\[phase6\] PASS" $(LIBC_OUT)/phase6-serial.log || { echo "[libc] FAIL: [phase6] PASS tidak terlihat di serial"; exit 1; }

# Kompilasi MURNI via staged SDK (SATU-SATUNYA -isystem C++ = staged;
# guard menolak rujukan tree LLVM dan header libc++ di luar subset).
$(LIBC_PHASE6_OBJ): $(LIBC_PHASE6_SRC) $(SDK_CPP_STAGE)
	@if grep -q "third_party" $(LIBC_PHASE6_SRC); then echo "[libc] FAIL: phase6 app menyebut third_party (bocor ke internal LLVM)"; exit 1; fi
	@if grep -qE "#include <(iostream|fstream|filesystem|regex|locale|thread|mutex|future|chrono|random|shared_mutex|atomic|condition_variable|stop_token|semaphore|latch|barrier|functional)" $(LIBC_PHASE6_SRC); then echo "[libc] FAIL: phase6 app memakai header di luar subset Phase 6"; exit 1; fi
	@mkdir -p $(LIBC_OUT)
	$(LIBC_CXX) $(SDK_CXXFLAGS) -isystem $(SDK_CPP_INC) -isystem $(SDK_INC) -c $< -o $@

# Link: app + libcxxrt.a + cxxrt + crt + libc + ld C++.
$(LIBC_PHASE6_APP): $(LIBC_PHASE6_OBJ) $(SDK_CPP_STAGE) $(LIBCXXRT_ARCHIVE) $(SDK_CPP_CXXRT) $(SDK_CRT) $(SDK_LIB)
	@mkdir -p $(dir $@)
	$(LIBC_LD) -m elf_x86_64 -nostdlib -T $(SDK_CPP_LD) -o $@ $(LIBC_PHASE6_OBJ) $(LIBCXXRT_ARCHIVE) $(SDK_CPP_CXXRT) $(SDK_CRT) $(SDK_LIB)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[libc] FAIL: _start tidak ada di app Phase 6"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[libc] FAIL: masih ada simbol undefined di app Phase 6"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@if $(LIBC_NM) --defined-only $@ | grep -qE " (__cxa_throw|__cxa_begin_catch|__cxa_end_catch|_Unwind_|__gxx_personality_|pthread_)"; then \
		echo "[libc] FAIL: app Phase 6 menarik runtime exception/thread"; $(LIBC_NM) --defined-only $@ | grep -E " (__cxa_throw|_Unwind_|pthread_)"; exit 1; \
	fi
	@echo "[libc] phase6 link OK: $(notdir $@) (entry _start, 0 undefined, 0 cxa/unwind/pthread)"

$(LIBC_PHASE6_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test libc++ subset Phase 6 (hanya ada di ISO uji).\n    module_path: boot():/libc_phase6.elf\n    module_string: libc_phase6.elf\n\n' >> $@

# ---- Phase 7: C++ Application SDK (boundary publik + wrapper) ----
# Aplikasi hanya memakai `build/sdk/cpp/bin/kyuzen-c++`; recipe di bawah
# tidak menyebut path LLVM/port/build internal. Wrapper memiliki flag,
# include, arsip, dan urutan link kanonis. Dependensi normal pada
# $(SDK_CPP_STAGE) membuat app relink bila SDK di-stage ulang.
LIBC_PHASE7_SRC  = tools/libc-phase7/sdk_smoke.cpp
LIBC_PHASE7_APP  = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/libc_phase7.elf
LIBC_PHASE7_CONF = $(LIBC_OUT)/iso7/limine.conf
LIBC_PHASE7_ISOLATION = tools/libc-phase7/check-sdk-isolation.sh
CPP_EX_DIR       = examples/cpp
CPP_OUT          = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin
CPP_HELLO_SRC    = $(CPP_EX_DIR)/hello/hello.cpp
CPP_HELLO_APP    = $(CPP_OUT)/cpp_hello.elf
CPP_HELLO_CONF   = $(LIBC_OUT)/iso-hello/limine.conf
CPP_EX_SRCS      = $(CPP_HELLO_SRC) \
                   $(CPP_EX_DIR)/containers/containers.cpp \
                   $(CPP_EX_DIR)/strings/strings.cpp
CPP_EX_APPS      = $(CPP_HELLO_APP) \
                   $(CPP_OUT)/cpp_containers.elf \
                   $(CPP_OUT)/cpp_strings.elf

.PHONY: cpp-sdk-isolation
cpp-sdk-isolation: $(SDK_CPP_STAGE)
	@bash $(LIBC_PHASE7_ISOLATION) $(LIBC_PHASE7_SRC) $(CPP_EX_SRCS)

.PHONY: libc-phase7
libc-phase7: $(LIBC_PHASE7_APP)
	@echo "[libc] phase7 app : $(LIBC_PHASE7_APP)"

$(LIBC_PHASE7_APP): $(LIBC_PHASE7_SRC) $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@bash $(LIBC_PHASE7_ISOLATION) $<
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) $< -o $@
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[libc] FAIL: _start tidak ada di app Phase 7"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[libc] FAIL: masih ada simbol undefined di app Phase 7"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@if $(LIBC_NM) --defined-only $@ | grep -qE " (__cxa_throw|__cxa_begin_catch|__cxa_end_catch|_Unwind_|__gxx_personality_|pthread_)"; then \
		echo "[libc] FAIL: app Phase 7 menarik runtime exception/thread"; $(LIBC_NM) --defined-only $@ | grep -E " (__cxa_throw|_Unwind_|pthread_)"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "%xmm|%ymm|%zmm"; then \
		echo "[libc] FAIL: app Phase 7 mengandung instruksi SSE"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "	(fld|fst|fxch|fucom|fadd|fmul|fdiv|fsub|fild|fist|fcom)"; then \
		echo "[libc] FAIL: app Phase 7 mengandung instruksi x87"; exit 1; \
	fi
	@echo "[libc] phase7 link OK: $(notdir $@) (entry _start, 0 undefined, 0 cxa/unwind/pthread, 0 SSE/x87)"

$(LIBC_PHASE7_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test C++ Application SDK Phase 7 (hanya ada di ISO uji).\n    module_path: boot():/libc_phase7.elf\n    module_string: libc_phase7.elf\n\n' >> $@

.PHONY: libc-phase7-qemu
libc-phase7-qemu: $(LIBC_PHASE7_APP) $(LIBC_PHASE7_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(LIBC_PHASE7_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase7/run-qemu.sh
	@echo "--- bukti serial [phase7] ---"; grep "\[phase7\]" $(LIBC_OUT)/phase7-serial.log || true
	@grep -q "\[phase7\] PASS" $(LIBC_OUT)/phase7-serial.log || { echo "[libc] FAIL: [phase7] PASS tidak terlihat di serial"; exit 1; }

.PHONY: cpp-app cpp-examples
cpp-app: $(CPP_HELLO_APP)
	@echo "[cpp-app] example : $(CPP_HELLO_APP)"

cpp-examples: $(CPP_EX_APPS)
	@echo "[cpp-app] examples: $(CPP_EX_APPS)"

$(CPP_HELLO_APP): $(CPP_HELLO_SRC) $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@bash $(LIBC_PHASE7_ISOLATION) $<
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) $< -o $@
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[cpp-app] FAIL: _start tidak ada di hello"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[cpp-app] FAIL: masih ada simbol undefined di hello"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "%xmm|%ymm|%zmm"; then \
		echo "[cpp-app] FAIL: contoh hello mengandung instruksi SSE"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "	(fld|fst|fxch|fucom|fadd|fmul|fdiv|fsub|fild|fist|fcom)"; then \
		echo "[cpp-app] FAIL: contoh hello mengandung instruksi x87"; exit 1; \
	fi
	@echo "[cpp-app] link OK: $(notdir $@) (entry _start, 0 undefined, 0 SSE/x87)"

$(CPP_OUT)/cpp_containers.elf: $(CPP_EX_DIR)/containers/containers.cpp $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@bash $(LIBC_PHASE7_ISOLATION) $<
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) $< -o $@
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[cpp-app] FAIL: _start tidak ada di containers"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[cpp-app] FAIL: masih ada simbol undefined di containers"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "%xmm|%ymm|%zmm"; then \
		echo "[cpp-app] FAIL: contoh containers mengandung instruksi SSE"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "	(fld|fst|fxch|fucom|fadd|fmul|fdiv|fsub|fild|fist|fcom)"; then \
		echo "[cpp-app] FAIL: contoh containers mengandung instruksi x87"; exit 1; \
	fi
	@echo "[cpp-app] link OK: $(notdir $@) (entry _start, 0 undefined, 0 SSE/x87)"

$(CPP_OUT)/cpp_strings.elf: $(CPP_EX_DIR)/strings/strings.cpp $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@bash $(LIBC_PHASE7_ISOLATION) $<
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) $< -o $@
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[cpp-app] FAIL: _start tidak ada di strings"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[cpp-app] FAIL: masih ada simbol undefined di strings"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "%xmm|%ymm|%zmm"; then \
		echo "[cpp-app] FAIL: contoh strings mengandung instruksi SSE"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "	(fld|fst|fxch|fucom|fadd|fmul|fdiv|fsub|fild|fist|fcom)"; then \
		echo "[cpp-app] FAIL: contoh strings mengandung instruksi x87"; exit 1; \
	fi
	@echo "[cpp-app] link OK: $(notdir $@) (entry _start, 0 undefined, 0 SSE/x87)"

$(CPP_HELLO_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Contoh Kyuzen C++ SDK (hanya ada di ISO uji).\n    module_path: boot():/cpp_hello.elf\n    module_string: cpp_hello.elf\n\n' >> $@

.PHONY: cpp-app-run
cpp-app-run: $(CPP_HELLO_APP) $(CPP_HELLO_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(CPP_HELLO_CONF)
	@KYUZEN_TEST_APP_PATH="$(CPP_HELLO_APP)" \
		KYUZEN_TEST_APP="cpp_hello" \
		KYUZEN_TEST_DISK="$(LIBC_OUT)/cpp-app-disk.img" \
		KYUZEN_TEST_SERIAL="$(LIBC_OUT)/cpp-app-serial.log" \
		KYUZEN_TEST_MONLOG="$(LIBC_OUT)/cpp-app-qemu-monitor.log" \
		KYUZEN_TEST_MARKER="Hello from Kyuzen C++ SDK" \
		KYUZEN_TEST_SUCCESS="Hello from Kyuzen C++ SDK 7.0" \
		KYUZEN_TEST_FAILURE="[cpp-app] FAIL" \
		QEMU="$(QEMU)" bash tools/libc-phase7/run-qemu.sh
	@echo "--- bukti serial [cpp-app] ---"; grep "Hello from Kyuzen C++ SDK" $(LIBC_OUT)/cpp-app-serial.log || true
	@grep -qF "Hello from Kyuzen C++ SDK 7.0" $(LIBC_OUT)/cpp-app-serial.log || { echo "[cpp-app] FAIL: sapaan contoh tidak terlihat di serial"; exit 1; }

# ---- Phase 8: Desktop Framework Foundation (libdesktop + desktop ganti) ----
# Arsitektur: kernel/KWM (tak tersentuh) ← libdesktop (framework, arsip
# statis) ← apps/<DESKTOP_APP> (implementasi userspace biasa, ELF statis).
# Slot boot tetap $(ELF_DIR)/desktop.elf (login/limine/manifest tak berubah);
# DESKTOP_APP memilih SUMBER yang mengisi slot itu. Stamp .selected memaksa
# relink saat implementasi diganti (object per-impl terisolasi di objdir
# masing-masing, jadi ganti bolak-balik tak pernah basi).
LIBDESKTOP_SRCS   = $(LIBDESKTOP_SRC_DIR)/src/event.cpp \
                    $(LIBDESKTOP_SRC_DIR)/src/window_manager.cpp \
                    $(LIBDESKTOP_SRC_DIR)/src/canvas.cpp \
                    $(LIBDESKTOP_SRC_DIR)/src/system.cpp \
                    $(LIBDESKTOP_SRC_DIR)/src/application.cpp
LIBDESKTOP_OBJDIR = $(BUILD_DIR)/obj/libdesktop
LIBDESKTOP_OBJS   = $(patsubst $(LIBDESKTOP_SRC_DIR)/src/%.cpp,$(LIBDESKTOP_OBJDIR)/%.o,$(LIBDESKTOP_SRCS))
LIBDESKTOP_LIB    = $(BUILD_DIR)/desktop/libdesktop.a
LIBDESKTOP_ISOLATION = tools/desktop-phase8/check-desktop-isolation.sh
# Header C bersama untuk TU framework/implementasi (userlib.h/libgui.h +
# color_types.h — pola yang sama dipakai user_apps/Makefile CFLAGS_COMMON).
LIBDESKTOP_SYS_INC = -I$(LIBDESKTOP_INC_SRC) -I$(INCLUDE_DIR) -Ilibs/color/include

DESKTOP_APP      ?= desktop
DESKTOP_IMPL_DIR  = apps/$(DESKTOP_APP)
DESKTOP_SRCS      = $(wildcard $(DESKTOP_IMPL_DIR)/*.cpp)
DESKTOP_OBJDIR    = $(BUILD_DIR)/obj/desktop-$(DESKTOP_APP)
DESKTOP_OBJS      = $(patsubst $(DESKTOP_IMPL_DIR)/%.cpp,$(DESKTOP_OBJDIR)/%.o,$(DESKTOP_SRCS))
DESKTOP_SELECTED  = $(BUILD_DIR)/desktop/.selected
DESKTOP_ELF       = $(ELF_DIR)/desktop.elf
# Object C userspace yang dipakai link desktop (userlib/libgui — dibangun
# sub-make user_apps; pola di bawah memicu sub-make itu bila berkas hilang).
USERAPP_LIB_OBJS  = $(BUILD_DIR)/obj/user/apps/userlib.o $(BUILD_DIR)/obj/user/apps/libgui.o

DTSMOKE_SRC   = tools/desktop-phase8/dtsmoke.cpp
DTSMOKE_APP   = $(LIBC_OUT)/desktop-phase8/dtsmoke.elf
DTSMOKE_CONF  = $(LIBC_OUT)/desktop-phase8/limine.conf

.PHONY: libdesktop
libdesktop: $(LIBDESKTOP_LIB)
	@echo "[libdesktop] archive : $(LIBDESKTOP_LIB)"

.PHONY: desktop
desktop: $(DESKTOP_ELF)
	@echo "[desktop] selected : $(DESKTOP_APP) -> $(DESKTOP_ELF)"

.PHONY: desktop-isolation
desktop-isolation:
	@bash $(LIBDESKTOP_ISOLATION)

$(LIBDESKTOP_OBJDIR)/%.o: $(LIBDESKTOP_SRC_DIR)/src/%.cpp $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) -c $< -o $@ $(LIBDESKTOP_SYS_INC)

$(LIBDESKTOP_LIB): $(LIBDESKTOP_OBJS) $(SDK_CPP_STAGE)
	@bash $(LIBDESKTOP_ISOLATION)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(LIBC_AR) rcs $@ $(LIBDESKTOP_OBJS)
	@test "$$(llvm-ar t $@ | wc -l)" = "5" || { echo "[libdesktop] FAIL: arsip harus 5 member (event/window_manager/canvas/system/application)"; exit 1; }
	@$(LIBC_NM) --defined-only $@ | grep -q "Application.*run\|_ZN6kyuzen7desktop11Application3run" || { echo "[libdesktop] FAIL: arsip tanpa Application::run"; exit 1; }
	@echo "[libdesktop] archive OK: $(notdir $@) (5 member)"

# Stamp pilihan implementasi: ganti isi + touch bila DESKTOP_APP berubah
# (inilah yang memaksa relink desktop.elf bolak-balik tanpa `make clean`).
.PHONY: FORCE
$(DESKTOP_SELECTED): FORCE
	@mkdir -p $(dir $@)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(DESKTOP_APP)" ]; then echo "$(DESKTOP_APP)" > $@; echo "[desktop] selected implementation: $(DESKTOP_APP)"; fi

$(DESKTOP_OBJDIR)/%.o: $(DESKTOP_IMPL_DIR)/%.cpp $(SDK_CPP_STAGE) $(DESKTOP_SELECTED) | $(SDK_CPP_WRAPPER)
	@test -d $(DESKTOP_IMPL_DIR) || { echo "[desktop] FAIL: implementasi '$(DESKTOP_APP)' tidak ada (apps/$(DESKTOP_APP)/ hilang?)"; exit 1; }
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) -c $< -o $@ $(LIBDESKTOP_SYS_INC)

$(BUILD_DIR)/obj/user/apps/%.o:
	$(MAKE) -C user_apps all

$(DESKTOP_ELF): $(DESKTOP_SELECTED) $(DESKTOP_OBJS) $(LIBDESKTOP_LIB) $(USERAPP_LIB_OBJS) $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@test -n "$(DESKTOP_SRCS)" || { echo "[desktop] FAIL: implementasi '$(DESKTOP_APP)' tanpa *.cpp di $(DESKTOP_IMPL_DIR)/"; exit 1; }
	@bash $(LIBDESKTOP_ISOLATION)
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) $(DESKTOP_OBJS) $(LIBDESKTOP_LIB) $(USERAPP_LIB_OBJS) -o $@ $(LIBDESKTOP_SYS_INC)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[desktop] FAIL: _start tidak ada di desktop.elf ($(DESKTOP_APP))"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[desktop] FAIL: masih ada simbol undefined di desktop.elf ($(DESKTOP_APP))"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@if $(LIBC_NM) --defined-only $@ | grep -qE " (__cxa_throw|__cxa_begin_catch|__cxa_end_catch|_Unwind_|__gxx_personality_|pthread_)"; then \
		echo "[desktop] FAIL: desktop.elf menarik runtime exception/thread"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "%xmm|%ymm|%zmm"; then \
		echo "[desktop] FAIL: desktop.elf mengandung instruksi SSE ($(DESKTOP_APP))"; exit 1; \
	fi
	@if $(LIBC_OBJDUMP) -d $@ | grep -Eq "	(fld|fst|fxch|fucom|fadd|fmul|fdiv|fsub|fild|fist|fcom)"; then \
		echo "[desktop] FAIL: desktop.elf mengandung instruksi x87 ($(DESKTOP_APP))"; exit 1; \
	fi
	@echo "[desktop] link OK: desktop.elf ($(DESKTOP_APP), entry _start, 0 undefined, 0 SSE/x87)"

# Framework smoke: konsol biasa (tanpa window) yang memakai libdesktop.
$(DTSMOKE_APP): $(DTSMOKE_SRC) $(LIBDESKTOP_LIB) $(SDK_CPP_STAGE) | $(SDK_CPP_WRAPPER)
	@bash $(LIBDESKTOP_ISOLATION)
	@mkdir -p $(dir $@)
	@KYUZEN_CXX="$(LIBC_CXX)" KYUZEN_LD="$(LIBC_LD)" $(SDK_CPP_WRAPPER) $< $(LIBDESKTOP_LIB) -o $@ $(LIBDESKTOP_SYS_INC)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[desktop] FAIL: _start tidak ada di dtsmoke"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[desktop] FAIL: masih ada simbol undefined di dtsmoke"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[desktop] smoke link OK: $(notdir $@)"

$(DTSMOKE_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Framework smoke desktop Phase 8 (hanya ada di ISO uji).\n    module_path: boot():/dtsmoke.elf\n    module_string: dtsmoke.elf\n\n' >> $@

.PHONY: desktop-smoke
desktop-smoke: $(DTSMOKE_APP) $(DTSMOKE_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(DTSMOKE_CONF)
	@KYUZEN_TEST_APP_PATH="$(DTSMOKE_APP)" \
		KYUZEN_TEST_APP="dtsmoke" \
		KYUZEN_TEST_START="dtsmoke" \
		KYUZEN_TEST_DISK="$(LIBC_OUT)/desktop-phase8/dtsmoke-disk.img" \
		KYUZEN_TEST_SERIAL="$(LIBC_OUT)/desktop-phase8/dtsmoke-serial.log" \
		KYUZEN_TEST_MONLOG="$(LIBC_OUT)/desktop-phase8/dtsmoke-qemu-monitor.log" \
		KYUZEN_TEST_MARKER="[dtsmoke]" \
		KYUZEN_TEST_SUCCESS="[dtsmoke] PASS" \
		KYUZEN_TEST_FAILURE="[dtsmoke] FAIL" \
		QEMU="$(QEMU)" bash tools/desktop-phase8/run-qemu.sh
	@echo "--- bukti serial [dtsmoke] ---"; grep "\[dtsmoke\]" $(LIBC_OUT)/desktop-phase8/dtsmoke-serial.log || true
	@grep -q "\[dtsmoke\] PASS" $(LIBC_OUT)/desktop-phase8/dtsmoke-serial.log || { echo "[desktop] FAIL: [dtsmoke] PASS tidak terlihat di serial"; exit 1; }

# Boot desktop pilihan di QEMU (ISO default — desktop.elf = slot boot).
# Bukti: marker startup + shell tetap hidup (echo) + tanpa panic.
.PHONY: desktop-qemu
desktop-qemu: $(DESKTOP_ELF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso
	@KYUZEN_TEST_DISK="$(LIBC_OUT)/desktop-phase8/desktop-disk.img" \
		KYUZEN_TEST_SERIAL="$(LIBC_OUT)/desktop-phase8/desktop-serial.log" \
		KYUZEN_TEST_MONLOG="$(LIBC_OUT)/desktop-phase8/desktop-qemu-monitor.log" \
		KYUZEN_TEST_MARKER="[desktop]" \
		KYUZEN_TEST_SUCCESS="[desktop] shell started (libdesktop)" \
		KYUZEN_TEST_FAILURE="PANIC" \
		QEMU="$(QEMU)" bash tools/desktop-phase8/run-qemu.sh
	@echo "--- bukti serial [desktop] ---"; grep "\[desktop\]" $(LIBC_OUT)/desktop-phase8/desktop-serial.log || true
	@grep -qF "[desktop] shell started (libdesktop)" $(LIBC_OUT)/desktop-phase8/desktop-serial.log || { echo "[desktop] FAIL: marker startup tak terlihat di serial"; exit 1; }

.PHONY: desktop-qemu-test
desktop-qemu-test:
	@$(MAKE) desktop DESKTOP_APP=test-desktop
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso
	@KYUZEN_TEST_DISK="$(LIBC_OUT)/desktop-phase8/test-desktop-disk.img" \
		KYUZEN_TEST_SERIAL="$(LIBC_OUT)/desktop-phase8/test-desktop-serial.log" \
		KYUZEN_TEST_MONLOG="$(LIBC_OUT)/desktop-phase8/test-desktop-qemu-monitor.log" \
		KYUZEN_TEST_MARKER="[test-desktop]" \
		KYUZEN_TEST_SUCCESS="[test-desktop] shutdown ok" \
		KYUZEN_TEST_FAILURE="PANIC" \
		QEMU="$(QEMU)" bash tools/desktop-phase8/run-qemu.sh
	@echo "--- bukti serial [test-desktop] ---"; grep "\[test-desktop\]" $(LIBC_OUT)/desktop-phase8/test-desktop-serial.log || true
	@grep -qF "[test-desktop] shutdown ok" $(LIBC_OUT)/desktop-phase8/test-desktop-serial.log || { echo "[desktop] FAIL: siklus test-desktop tak lengkap di serial"; exit 1; }
	@$(MAKE) desktop DESKTOP_APP=desktop
	@echo "[desktop] implementasi default dipulihkan (desktop.elf = Kyuzen Desktop lagi)"

# ==========================================
# Kyuzen C SDK — Phase 3 (staging + smoke app)
# ==========================================
# Boundary publik aplikasi C: yang di-commit hanya sumber boundary
# (sdk/c/linker/app.ld + sdk/c/README.md); yang di-generate di-stage ke
# build/sdk/c (gitignored) lewat `make sdk-c`:
#
#   build/sdk/c/include/  ← salinan header hasil hdrgen (bukan manual,
#                            bukan internal src/__support/...)
#   build/sdk/c/lib/libc.a ← salinan archive Phase 0–4 terverifikasi
#   build/sdk/c/crt/crt.o  ← salinan object port (_start + exit/errno/heap/stdio/time)
#   build/sdk/c/linker/app.ld ← salinan linker script kanonis
#
#   make sdk-c             → stage SDK dari sumber LLVM yang di-pin
#   make sdk-c-smoke       → bangun app uji murni lewat SDK (tanpa third_party)
#   make sdk-c-smoke-qemu  → jalankan app uji di QEMU (harap [phase3] PASS)
#
SDK_SRC_DIR   = sdk/c
SDK_DIR       = $(BUILD_DIR)/sdk/c
SDK_INC       = $(SDK_DIR)/include
SDK_LIB       = $(SDK_DIR)/lib/libc.a
SDK_CRT       = $(SDK_DIR)/crt/crt.o
SDK_LD        = $(SDK_DIR)/linker/app.ld
SDK_LD_SRC    = $(SDK_SRC_DIR)/linker/app.ld
SDK_STAGE     = $(SDK_DIR)/.staged
# Flag kanonis app SDK: sama persis dengan flag pembangun libc.a
# (freestanding, tanpa SSE — kernel tidak mengaktifkan CR4.OSFXSR)
# + -O2 mengikuti konvensi library user_apps (bukan -O0).
SDK_CFLAGS    = $(LIBC_TARGET_FLAGS) -O2
SDK_SMOKE_SRC = tools/libc-phase3/sdk_smoke.c
SDK_SMOKE_OBJ = $(LIBC_OUT)/sdk_smoke.o
SDK_SMOKE_APP = $(LIBC_OUT)/$(LIBC_TRIPLE)/bin/sdk_smoke.elf
# Varian limine.conf untuk ISO smoke test SDK (direktori sendiri).
SDK_SMOKE_CONF = $(LIBC_OUT)/iso3/limine.conf

.PHONY: sdk-c
sdk-c: $(SDK_STAGE)
	@echo "[sdk] staged : $(SDK_DIR)"

# Satu stage atomik: header + archive + crt + linker script. Prereq
# LIBC_ARCHIVE/LIBC_PORT_OBJ menarik rebuild LLVM bila entrypoints berubah.
# Guard di bawah menolak state basi tanpa suara (lihat audit §14.7.6:
# incremental cmake tidak mendeteksi perubahan entrypoints.txt — bila guard
# ini gagal, hapus build/libc/cmake lalu ulangi dari make libc-phase0).
$(SDK_STAGE): $(LIBC_ARCHIVE) $(LIBC_PORT_OBJ) $(SDK_LD_SRC)
	@mkdir -p $(SDK_INC) $(dir $(SDK_LIB)) $(dir $(SDK_CRT)) $(dir $(SDK_LD))
	@# Header = HANYA *.h publik (top + llvm-libc-types/ + llvm-libc-macros/):
	@# direktori generated LLVM juga berisi file build CMakeFiles/Makefile
	@# yang TIDAK boleh ikut ke SDK. rm dulu agar stage idempoten.
	@rm -rf $(SDK_INC)
	@mkdir -p $(SDK_INC)/llvm-libc-types $(SDK_INC)/llvm-libc-macros
	@cp $(LIBC_INCLUDE)/*.h $(SDK_INC)/
	@cp $(LIBC_INCLUDE)/llvm-libc-types/*.h $(SDK_INC)/llvm-libc-types/
	@cp $(LIBC_INCLUDE)/llvm-libc-macros/*.h $(SDK_INC)/llvm-libc-macros/
	@# Subdirektori per-OS (Phase 4: time-macros.h meng-include
	@# "baremetal/time-macros.h" di bawah __ELF__; tanpa ini time.h gagal
	@# dikompilasi). Hanya *.h yang disalin — artefak build CMakeFiles/
	@# Makefile/cmake_install.cmake TIDAK ikut ke SDK.
	@mkdir -p $(SDK_INC)/llvm-libc-macros/baremetal
	@cp $(LIBC_INCLUDE)/llvm-libc-macros/baremetal/*.h $(SDK_INC)/llvm-libc-macros/baremetal/
	@cp $(LIBC_ARCHIVE) $(SDK_LIB)
	@cp $(LIBC_PORT_OBJ) $(SDK_CRT)
	@cp $(SDK_LD_SRC) $(SDK_LD)
	@test -f $(SDK_INC)/stdio.h -a -f $(SDK_INC)/stdlib.h -a -f $(SDK_INC)/string.h -a -f $(SDK_INC)/ctype.h -a -f $(SDK_INC)/errno.h || { echo "[sdk] FAIL: header SDK tak lengkap di $(SDK_INC)"; exit 1; }
	@test -f $(SDK_INC)/time.h || { echo "[sdk] FAIL: time.h tak ada di SDK (libc.a basi pra-Phase 4?) — hapus build/libc/cmake lalu make libc-phase0"; exit 1; }
	@$(LIBC_NM) --defined-only $(SDK_LIB) | grep -qE "[Tt] printf$$" || { echo "[sdk] FAIL: libc.a basi (tanpa printf) — hapus build/libc/cmake lalu make libc-phase0"; exit 1; }
	@$(LIBC_NM) --defined-only $(SDK_LIB) | grep -qE "[Tt] malloc$$" || { echo "[sdk] FAIL: libc.a basi (tanpa malloc) — hapus build/libc/cmake lalu make libc-phase0"; exit 1; }
	@$(LIBC_NM) --defined-only $(SDK_LIB) | grep -qE "[Tt] qsort$$" || { echo "[sdk] FAIL: libc.a basi (tanpa qsort Phase 4) — hapus build/libc/cmake lalu make libc-phase0"; exit 1; }
	@$(LIBC_NM) --defined-only $(SDK_LIB) | grep -qE "[Tt] timespec_get$$" || { echo "[sdk] FAIL: libc.a basi (tanpa timespec_get Phase 4) — hapus build/libc/cmake lalu make libc-phase0"; exit 1; }
	@touch $@
	@echo "[sdk] stage OK: include + libc.a + crt.o + app.ld"

# File stage sebagai target nyata (ordering eksplisit): app yang me-link
# file-file ini dijamin dibangun SETELAH stage, bukan dari salinan basi.
$(SDK_CRT) $(SDK_LIB) $(SDK_LD): $(SDK_STAGE)
	@:

.PHONY: sdk-c-smoke
sdk-c-smoke: $(SDK_SMOKE_APP)
	@echo "[sdk] smoke app : $(SDK_SMOKE_APP)"

# Kompilasi MURNI via SDK: satu-satunya include libc adalah -isystem SDK.
# Guard third_party membuktikan batas ergonomi (app → SDK → LLVM, bukan
# app → internal LLVM).
$(SDK_SMOKE_OBJ): $(SDK_SMOKE_SRC) $(SDK_STAGE)
	@if grep -q "third_party" $(SDK_SMOKE_SRC); then echo "[sdk] FAIL: smoke app menyebut third_party (bocor ke internal LLVM)"; exit 1; fi
	@mkdir -p $(LIBC_OUT)
	$(LIBC_CC) $(SDK_CFLAGS) -isystem $(SDK_INC) -c $< -o $@

# Link MURNI via SDK: crt + libc + ld semuanya dari build/sdk/c.
$(SDK_SMOKE_APP): $(SDK_SMOKE_OBJ) $(SDK_STAGE)
	@mkdir -p $(dir $@)
	$(LIBC_LD) -m elf_x86_64 -nostdlib -T $(SDK_LD) -o $@ $(SDK_SMOKE_OBJ) $(SDK_CRT) $(SDK_LIB)
	@$(LIBC_NM) $@ | grep -qE "[Tt] _start$$" || { echo "[sdk] FAIL: _start tidak ada di app SDK"; exit 1; }
	@if $(LIBC_NM) --undefined-only $@ | grep -q .; then \
		echo "[sdk] FAIL: masih ada simbol undefined di app SDK"; $(LIBC_NM) --undefined-only $@; exit 1; \
	fi
	@echo "[sdk] smoke link OK: $(notdir $@) (entry _start, 0 undefined)"

$(SDK_SMOKE_CONF): limine.conf
	@mkdir -p $(dir $@)
	@cp limine.conf $@
	@printf '\n\n    # Smoke test Kyuzen C SDK Phase 3 (hanya ada di ISO uji).\n    module_path: boot():/sdk_smoke.elf\n    module_string: sdk_smoke.elf\n\n' >> $@

.PHONY: sdk-c-smoke-qemu
sdk-c-smoke-qemu: $(SDK_SMOKE_APP) $(SDK_SMOKE_CONF)
	@rm -f $(ISO_IMAGE)
	@$(MAKE) boot_image.iso LIMINE_CONF=$(SDK_SMOKE_CONF)
	@QEMU="$(QEMU)" bash tools/libc-phase3/run-qemu.sh
	@echo "--- bukti serial [phase3] ---"; grep "\[phase3\]" $(LIBC_OUT)/phase3-serial.log || true
	@grep -q "\[phase3\] PASS" $(LIBC_OUT)/phase3-serial.log || { echo "[sdk] FAIL: [phase3] PASS tidak terlihat di serial"; exit 1; }

# --- Cross-check: image buatan mkfs host harus termount oleh parser kernel ---
# Target memformat testimg.img via ./mkfs.kyuzenfs lalu menjalankan test host
# yang memuat image tersebut ke RAM disk mock. Jalankan: make test-kyuzenfs-xcheck
.PHONY: test-kyuzenfs-xcheck
test-kyuzenfs-xcheck: test/kyuzenfs_xcheck mkfs.kyuzenfs
	rm -f testimg.img
	dd if=/dev/zero of=testimg.img bs=1M count=32 2>/dev/null
	./mkfs.kyuzenfs testimg.img
	./test/kyuzenfs_xcheck testimg.img

test/kyuzenfs_xcheck: test/kyuzenfs_xcheck.c $(KFS4_HDRS) $(KFS4_SRCS)
	$(HOSTCC) -O1 -Wall -iquote test -iquote include -o $@ test/kyuzenfs_xcheck.c

# --- Host test panic handler (BSOD): diagnostik, lockdown, interaktif ---
# kernel/panic/*.c di-include dengan -DPANIC_HOST_TEST (instruksi privileged → stub),
# jadi alur countdown → flush FS → reboot bisa diverifikasi tanpa QEMU.
# Jalankan: make test-panic
.PHONY: test-panic
test-panic: test/panic_test
	./test/panic_test

# Test ekuivalensi jalur baca ATA (Stage 1): model device ATA di host,
# jalur LEGACY vs BATCH (byte-identik + urutan sektor + hitung perintah +
# error/timeout/out-of-range). Image disk.img dipakai READ-ONLY kalau ada.
# Jalankan: make test-ata
# Ubah ATA_READ_PATH_DEFAULT WAJIB mengompilasi ulang driver ATA (make hanya
# melihat timestamp file, bukan isi variabel — tanpa ini `make
# ATA_READ_PATH_DEFAULT=1` tidak mengompilasi apa pun). Stamp ini jadi
# prerequisite drivers/ata.o dan hanya ditulis ulang saat nilainya berubah.
ATA_FLAG_STAMP = $(BUILD_DIR)/ata_read_path_default.stamp
FORCE:

$(ATA_FLAG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@printf '%s' "$(ATA_READ_PATH_DEFAULT)" | cmp -s - $@ || printf '%s' "$(ATA_READ_PATH_DEFAULT)" > $@

$(OBJ_DIR)/drivers/ata.o: $(ATA_FLAG_STAMP)
test/ata_devmodel_test: $(ATA_FLAG_STAMP)

.PHONY: test-ata
test-ata: test/ata_devmodel_test
	./test/ata_devmodel_test
	@if [ -f disk.img ]; then ./test/ata_devmodel_test disk.img; fi

test/ata_devmodel_test: test/ata_devmodel_test.c test/atamock/io.h \
                         drivers/ata.c include/ata.h include/io.h
	$(HOSTCC) -O1 -Wall -Wextra -iquote test/atamock -iquote test -iquote include \
	         -DATA_READ_PATH_DEFAULT=$(ATA_READ_PATH_DEFAULT) -o $@ test/ata_devmodel_test.c

test/panic_test: test/panic_test.c kernel/panic/panic.c kernel/panic/panic_draw.c \
                 kernel/panic/panic_hw.c kernel/panic/panic_explain.c kernel/panic/panic_internal.h \
                 kernel/panic_log.c kernel/crashdump.c drivers/acpi.c \
                 include/panic.h include/crashdump.h include/acpi.h include/display.h include/task.h include/timer.h
	$(HOSTCC) -DPANIC_HOST_TEST -O1 -Wall -iquote test -iquote include -o $@ test/panic_test.c

# TextEdit host test: libs/widget/**/*.cpp di-link apa adanya, syscall+libgui di-stub
# (20 symbol). Menguji logika editor yang dipakai notepad: undo/redo per operasi,
# seleksi + clipboard, find/replace_all, dan aritmetika baris LAYAR word wrap.
# Jalankan: make test-textedit
.PHONY: test-textedit
test-textedit: test/textedit_test
	./test/textedit_test

test/textedit_test: test/textedit_test.cpp $(wildcard libs/widget/src/*/*.cpp) libs/widget/abi/libui_abi.cpp include/libui.h include/libgui.h \
                    libs/color/src/color_utils.c libs/color/include/color_utils.h
	$(HOSTCC) -O1 -Ilibs/color/include -c libs/color/src/color_utils.c -o test/color_utils_host.o
	$(HOSTCXX) -std=c++17 -O1 -Wall -iquote include -Ilibs/widget/include -Ilibs/color/include -o $@ test/textedit_test.cpp $(wildcard libs/widget/src/*/*.cpp) libs/widget/abi/libui_abi.cpp test/color_utils_host.o

# Host test tema + render libui: libs/widget/**/*.cpp di-LINK (object toolkit)
# supaya Window/Button/Painter bisa diperiksa, lalu render sungguhan dicek
# piksel-per-piksel. Mengunci regresi "gradien tombol rata" yang muncul saat
# warna tema ABI (XRGB, alpha 0) mulai dilewatkan color_blend_alpha.
# Jalankan: make test-libui-theme
.PHONY: test-libui-theme
test-libui-theme: test/libui_theme_test
	./test/libui_theme_test

test/libui_theme_test: test/libui_theme_test.cpp $(wildcard libs/widget/src/*/*.cpp) libs/widget/abi/libui_abi.cpp include/libui.h \
                       include/libgui.h include/aa_math.h \
                       libs/color/src/color_utils.c libs/color/include/color_utils.h
	$(HOSTCC) -O1 -Ilibs/color/include -c libs/color/src/color_utils.c -o test/color_utils_host.o
	$(HOSTCXX) -std=c++17 -O1 -Wall -iquote . -iquote include -Ilibs/widget/include -Ilibs/color/include -o $@ test/libui_theme_test.cpp $(wildcard libs/widget/src/*/*.cpp) libs/widget/abi/libui_abi.cpp test/color_utils_host.o

# Desktop host test: modul apps/desktop + backend libdesktop dikompilasi
# langsung dengan syscall di-stub. Menguji discovery/manifest launcher,
# terjemahan event + WindowManager, poll/klik taskbar, DAN siklus notifikasi
# crash (kartu harus bisa ditutup oleh klik/waktu habis). C++ (HOSTCXX)
# karena modulnya C++ — tanpa libc host untuk string (helper lokal).
# Jalankan: make test-desktop
.PHONY: test-desktop
test-desktop: test/desktop_manifest_test
	./test/desktop_manifest_test

DESKTOP_HOST_TUS = test/desktop_manifest_test.cpp \
                   apps/desktop/launcher.cpp apps/desktop/crash_notice.cpp apps/desktop/taskbar.cpp \
                   libs/libdesktop/src/event.cpp libs/libdesktop/src/window_manager.cpp libs/libdesktop/src/system.cpp libs/libdesktop/src/canvas.cpp
test/desktop_manifest_test: $(DESKTOP_HOST_TUS) include/userlib.h include/libgui.h \
                            $(LIBDESKTOP_PUBLIC_HEADERS) apps/desktop/launcher.hpp apps/desktop/taskbar.hpp apps/desktop/crash_notice.hpp apps/desktop/theme.hpp
	$(HOSTCXX) -O1 -Wall -iquote include -Ilibs/libdesktop/include -Iapps/desktop -Ilibs/color/include -o $@ test/desktop_manifest_test.cpp apps/desktop/launcher.cpp apps/desktop/crash_notice.cpp apps/desktop/taskbar.cpp libs/libdesktop/src/event.cpp libs/libdesktop/src/window_manager.cpp libs/libdesktop/src/system.cpp libs/libdesktop/src/canvas.cpp


# ==========================================
# USER APPS (ELF Terpisah, dimuat oleh Kernel via sys_load_elf)
# ==========================================

# Daftar app HARUS sinkron dengan APP_NAMES di user_apps/Makefile (kecuali
# desktop — dibangun aturan Phase 8 dari apps/$(DESKTOP_APP)/, bukan
# user_apps/), manifests/*.app, dan blok module_path di limine.conf.
APP_NAMES = fileman viewer clock calc taskmgr notepad badptr widget_demo desktop \
            terminal settings procinfo exit_test kill_test fd_test echo cat \
            pipe_test fork_test
APP_ELFS  = $(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(APP_NAMES)))

# `apps` tetap target phony (menu, kompatibel dengan workflow lama). Setiap ELF
# adalah FILE target nyata dengan prerequisite order-only ke `apps`, sehingga:
#   - `make apps` / `make boot_image.iso` menjalankan sub-make (yang incremental
#     di dalamnya: hanya app/header yang berubah yang dikompilasi ulang), dan
#   - ISO tetap dibangun ulang HANYA kalau timestamp ELF benar-benar berubah.
.PHONY: apps
apps: sdk-c sdk-cpp libdesktop
	$(MAKE) -C user_apps all
	$(MAKE) $(DESKTOP_ELF) DESKTOP_APP=$(DESKTOP_APP)

# desktop.elf DIKECUALIKAN dari relay ini: ia punya rule file nyata Phase 8
# (DESKTOP_ELF) dengan prereq-nya sendiri. Menggabungkannya ke sini akan
# menggabungkan prereq order-only `apps` ke rule desktop → `apps` memanggil
# `$(MAKE) $(DESKTOP_ELF)` → loop rekursi RH (`make desktop` fork-bomb).
$(filter-out $(DESKTOP_ELF),$(APP_ELFS)): | apps

# --- RUST APPS (Phase 1: no_std userspace Rust) ---
# Cargo tetap build system Rust (workspace di rust/); hasil akhir di-link dengan
# user_apps/app.ld yang sama (ELF64 single-base 0x4000000, PT_LOAD saja) agar
# bisa dimuat loader kernel. SELALU bangun lewat target ini: RUSTFLAGS
# meng-inject script linker (path relatif tidak bisa ditaruh di
# rust/.cargo/config.toml).
RUST_DIR   = rust
RUST_TRIP  = x86_64-unknown-none
RUST_OUT   = $(RUST_DIR)/target/$(RUST_TRIP)/release
RUST_NAMES = hello-slint control-center
RUST_ELFS  = $(addprefix $(ELF_DIR)/,$(addsuffix .elf,$(RUST_NAMES)))

# `$a.elf` disalin hanya kalau isinya berubah (cmp -s): cp apa adanya akan
# memperbarui timestamp setiap build dan membuat ISO selalu dianggap basi.
.PHONY: rust-apps
rust-apps:
	@mkdir -p $(ELF_DIR)
	cd $(RUST_DIR) && RUSTFLAGS="-C relocation-model=static -C link-arg=-T../user_apps/app.ld" cargo build --release
	@for a in $(RUST_NAMES); do \
		cmp -s $(RUST_OUT)/$$a $(ELF_DIR)/$$a.elf || cp $(RUST_OUT)/$$a $(ELF_DIR)/$$a.elf; \
	done

$(RUST_ELFS): | rust-apps

.PHONY: rust-clean
rust-clean:
	cd $(RUST_DIR) && cargo clean

# Shortcut: bangun ELF individual (nama target lama dipertahankan).
.PHONY: $(addsuffix .elf,$(APP_NAMES))
fileman.elf: $(ELF_DIR)/fileman.elf
viewer.elf: $(ELF_DIR)/viewer.elf
clock.elf: $(ELF_DIR)/clock.elf
calc.elf: $(ELF_DIR)/calc.elf
taskmgr.elf: $(ELF_DIR)/taskmgr.elf
notepad.elf: $(ELF_DIR)/notepad.elf
badptr.elf: $(ELF_DIR)/badptr.elf
widget_demo.elf: $(ELF_DIR)/widget_demo.elf
desktop.elf: $(ELF_DIR)/desktop.elf
terminal.elf: $(ELF_DIR)/terminal.elf
settings.elf: $(ELF_DIR)/settings.elf
procinfo.elf: $(ELF_DIR)/procinfo.elf
exit_test.elf: $(ELF_DIR)/exit_test.elf
kill_test.elf: $(ELF_DIR)/kill_test.elf
fd_test.elf: $(ELF_DIR)/fd_test.elf
echo.elf: $(ELF_DIR)/echo.elf
cat.elf: $(ELF_DIR)/cat.elf
pipe_test.elf: $(ELF_DIR)/pipe_test.elf
fork_test.elf: $(ELF_DIR)/fork_test.elf

# Bersihkan hanya file objek/ELF user_apps (kernel tidak disentuh)
.PHONY: clean-apps
clean-apps:
	$(MAKE) -C user_apps clean


# ==========================================
# ISO (Limine, hybrid BIOS + UEFI 64-bit)
# ==========================================

# Prerequisite ISO adalah FILE nyata (kernel, ELF, limine.conf, aset, manifest,
# file Limine) — bukan target phony — sehingga ISO hanya disusun ulang kalau
# salah satu inputnya berubah. Staging dir tidak di-`rm -rf` lagi: isinya
# disinkronkan (ELF lama dibuang supaya app yang dihapus tidak tertinggal).
MANIFESTS    = $(wildcard manifests/*.app)
LIMINE_FILES = limine/BOOTX64.EFI limine/limine-bios.sys \
               limine/limine-bios-cd.bin limine/limine-uefi-cd.bin

# Sumber limine.conf untuk ISO. Default: file di root repo (perilaku lama, tidak
# berubah). Smoke test libc Phase 1 menyuntikkan varian hasil generate lewat
# `make boot_image.iso LIMINE_CONF=...` supaya file repo tidak pernah memuat
# module yang belum tentu ada di ISO (Limine gagal memuat module yang hilang).
LIMINE_CONF ?= limine.conf

.PHONY: boot_image.iso
boot_image.iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(TARGET) $(COMPAT_BIN) $(APP_ELFS) $(RUST_ELFS) \
              $(LIMINE_CONF) kyuzen.png logo.png $(MANIFESTS) $(LIMINE_FILES)
	@mkdir -p $(ISO_ROOT)/EFI/BOOT
	@rm -f $(ISO_ROOT)/*.elf
	@cp $(APP_ELFS) $(RUST_ELFS) $(TARGET) $(LIMINE_CONF) kyuzen.png logo.png $(MANIFESTS) $(LIMINE_FILES) $(ISO_ROOT)/
	@# Opsional: app smoke test libc Phase 1/2/4/5/6/7 + SDK Phase 3 + contoh C++ (tidak diproduksi build normal).
	@if [ -f $(LIBC_PHASE1_APP) ]; then cp $(LIBC_PHASE1_APP) $(ISO_ROOT)/libc_phase1.elf; fi
	@if [ -f $(LIBC_PHASE2_APP) ]; then cp $(LIBC_PHASE2_APP) $(ISO_ROOT)/libc_phase2.elf; fi
	@if [ -f $(LIBC_PHASE4_APP) ]; then cp $(LIBC_PHASE4_APP) $(ISO_ROOT)/libc_phase4.elf; fi
	@if [ -f $(SDK_CPP_SMOKE_APP) ]; then cp $(SDK_CPP_SMOKE_APP) $(ISO_ROOT)/libc_phase5.elf; fi
	@if [ -f $(LIBC_PHASE6_APP) ]; then cp $(LIBC_PHASE6_APP) $(ISO_ROOT)/libc_phase6.elf; fi
	@if [ -f $(LIBC_PHASE7_APP) ]; then cp $(LIBC_PHASE7_APP) $(ISO_ROOT)/libc_phase7.elf; fi
	@if [ -f $(CPP_HELLO_APP) ]; then cp $(CPP_HELLO_APP) $(ISO_ROOT)/cpp_hello.elf; fi
	@if [ -f $(SDK_SMOKE_APP) ]; then cp $(SDK_SMOKE_APP) $(ISO_ROOT)/sdk_smoke.elf; fi
	@cp limine/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/
	# Xorriso sakti: Menggabungkan BIOS dan UEFI ke dalam 1 file ISO!
	xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_ROOT) -o $@
	./limine/limine.exe bios-install $@

# Tahap 4: Boot up QEMU (Dengan Fitur Debugging 64-bit)
# COM1 selalu diarahkan ke serial.log: kalau sistem membeku / panic, jejaknya
# sudah ada di file itu tanpa perlu mengubah cara menjalankan (dan tanpa
# menutup jendela QEMU lebih dulu).
.PHONY: run
run: boot_image.iso
	-rm -f serial.log
	qemu-system-x86_64.exe -cpu max -m 1G -boot d \
		-smp 8 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-vga none -device virtio-vga,xres=1920,yres=1080 \
		-display $(QEMU_DISPLAY) \
		-serial file:serial.log
	@echo "Jejak COM1 ada di serial.log (ekor file = kejadian terakhir)."

# run + serial stdio: tangkap panic dump ke terminal (bukan cuma framebuffer BSOD)
.PHONY: run-serial
run-serial: boot_image.iso
	qemu-system-x86_64.exe -cpu max -m 1G -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-vga none -device virtio-vga,xres=1920,yres=1080 \
		-display $(QEMU_DISPLAY) \
		-serial stdio

# run + serial ke FILE (lebih andal di Windows daripada stdio): panic dump
# tertulis ke serial.log — buka & paste setelah QEMU berhenti.
.PHONY: run-wd
run-wd: boot_image.iso
	qemu-system-x86_64.exe -cpu max -m 1G -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-vga none -device virtio-vga,xres=1920,yres=1080 \
		-display $(QEMU_DISPLAY)\
		-serial file:serial.log

# Stress test: recursive make with STRESS_TEST flag + debug/ sources
.PHONY: stress
stress:
	$(MAKE) clean
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) debug" CFLAGS="$(CFLAGS) -DSTRESS_TEST"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-display $(QEMU_DISPLAY)

# Concurrency test: sleep/mutex/semaphore/condvar (Fase 1-3) + test/ sources
.PHONY: conc
conc:
	$(MAKE) clean
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) test" CFLAGS="$(CFLAGS) -DCONC_TEST -Itest"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-display $(QEMU_DISPLAY)

# Heap stress test: overflow guard + canary corruption detection (test/ sources)
.PHONY: heap-stress
heap-stress:
	$(MAKE) clean
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) test" \
		CFLAGS="$(CFLAGS) -g -DHEAP_STRESS_TEST -Itest"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-display $(QEMU_DISPLAY)

.PHONY: heap-watch
heap-watch:
	$(MAKE) clean
	$(MAKE) boot_image.iso CFLAGS="$(CFLAGS) -g -DHEAP_WATCH_DEBUG"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=$(ISO_IMAGE),media=cdrom,index=2 \
		-nic user,model=e1000 \
		-display $(QEMU_DISPLAY) \
		-serial stdio


# ==========================================
# CLEAN
# ==========================================

# Sisa .o/.d dari Makefile lama (object ditulis di samping sumber) ikut
# dibersihkan supaya source tree benar-benar bersih. Hanya file *.o dan *.d
# yang dihapus — source, header, konfigurasi, dan file third_party yang bukan
# hasil build tidak disentuh. rust/target dan build tool lain di luar daftar
# ini juga tidak disentuh.
LEGACY_SWEEP_DIRS = arch apps drivers fs graphics kernel libs test tools user_apps third_party/net

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	rm -rf libs/widget/build
	$(MAKE) clean-tool
	@n=$$(find $(LEGACY_SWEEP_DIRS) -type f \( -name '*.o' -o -name '*.d' \) -delete -print 2>/dev/null | wc -l); \
	 echo "[CLEAN] build/ dihapus, $$n file .o/.d lama dibersihkan dari source tree"
