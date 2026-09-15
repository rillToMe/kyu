# ==========================================
# Kyuzen OS Build System
# ==========================================
#
# Target penting:
#   make          → Compile kernel (myos.bin)
#   make apps     → Compile user_apps (fileman.elf, viewer.elf)
#   make boot_image.iso → Build kernel + apps + ISO
#   make run      → Build + Boot di QEMU
#   make clean    → Bersihkan kernel objects
#   make clean-apps → Bersihkan user_apps objects
# ==========================================

# Tools
CC = clang
AS = nasm
LD = ld.lld
QEMU = qemu-system-x86_64.exe

# Direktori sumber kernel (Ring 0)
SRC_DIRS = arch/x86 drivers kernel kernel/smp kernel/gfx kernel/sched fs apps \
           graphics graphics/backend graphics/memory drivers/graphics/hw

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
E1000_OBJS = $(E1000_SRCS:.c=.o)

# Gabung semua source lwIP + e1000
LWIP_SRCS      = $(LWIP_CORE_SRCS) $(LWIP_NETIF_SRCS) $(LWIP_PORT_SRCS)

# Object files lwIP + e1000 (keduanya di-link bersama)
LWIP_OBJS      = $(LWIP_SRCS:.c=.o) $(E1000_OBJS)

# LWIP_CFLAGS akan didefinisikan di bawah, setelah CFLAGS kernel tersedia

# --- Flags Compiler 64-bit ---
# 1. Target diubah menjadi x86_64
# 2. -m32 DIHAPUS
# 3. DITAMBAHKAN -mno-red-zone (SANGAT PENTING!)
# 4. -mcmodel=kernel: wajib untuk higher-half kernel — mencegah R_X86_64_32
#    relocation error saat simbol berada di atas 4GB (0xFFFFFFFF80000000)
INCLUDE_DIR = include
# -MMD -MP: tulis file .d (dependensi header) di samping tiap .o — perubahan
# header (mis. task.h) memicu rebuild semua .c yang meng-includenya. Tanpa
# ini, object basi membaca struct dengan layout lama (pernah menggigit:
# task_t tambah field, scheduler membaca tasks[] dengan stride basi).
CFLAGS = --target=x86_64-pc-none-elf -ffreestanding -O2 -nostdlib -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -MMD -MP -I$(INCLUDE_DIR) -Igraphics -Igraphics/memory -Idrivers/graphics/hw

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
                        test/aa_math_test.c test/desktop_manifest_test.c test/kyuzenfs_dir_test.c test/virtqueue_test.c test/cred_test.c test/proc_test.c test/kill_test.c test/fd_test.c test/pipe_test.c test/fork_test.c,\
                        $(C_SOURCES_RAW))

# Ubah ekstensi sumber menjadi target object (.o)
# Kernel + arch + drivers object files
OBJS = $(C_SOURCES:.c=.o) $(ASM_SOURCES:.asm=.o)

# Default goal dipatok DULU. Tanpa ini, -include file .d di bawah membuat
# target pertama file .d (arch/x86/gdt.o) menjadi default goal → `make`
# hanya membangun gdt.o. (.DEFAULT_GOAL yang dieksplisit menang atas target
# pertama yang dilihat make.)
.DEFAULT_GOAL := all

# Sertakan dependensi header hasil -MMD (diabaikan saat belum ada / setelah clean)
-include $(OBJS:.o=.d) $(LWIP_OBJS:.o=.d)

# File output
TARGET = myos.bin

# Default target
all: $(TARGET)

# Tahap 3: Link Semuanya
$(TARGET): $(OBJS) $(LWIP_OBJS)
	$(LD) $(LDFLAGS) $(OBJS) $(LWIP_OBJS) -o $(TARGET)

# Tahap 2: Compile C (kernel/arch/drivers/fs/apps)
%.o: %.c
	$(CC) $(CFLAGS) -std=c11 -c $< -o $@

# Tahap 2b: Compile lwIP source files
# Aturan eksplisit ini harus muncul SEBELUM aturan generic %.o: %.c
# agar lwIP mendapat LWIP_CFLAGS (termasuk -I path yang benar).
$(LWIP_CORE_DIR)/%.o: $(LWIP_CORE_DIR)/%.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(LWIP_CORE_DIR)/ipv4/%.o: $(LWIP_CORE_DIR)/ipv4/%.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(LWIP_NETIF_DIR)/%.o: $(LWIP_NETIF_DIR)/%.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

$(LWIP_PORT_DIR)/%.o: $(LWIP_PORT_DIR)/%.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# e1000 driver: pakai CFLAGS kernel biasa (bukan LWIP_CFLAGS)
# e1000.c tidak butuh lwIP headers — hanya kernel headers (heap, string, pci)
$(E1000_DIR)/%.o: $(E1000_DIR)/%.c
	$(CC) $(CFLAGS) -std=c11 -I$(INCLUDE_DIR) -c $< -o $@

# net_init.c: butuh LWIP_CFLAGS karena include lwIP headers (dhcp.h, dns.h, dll)
# Aturan ini OVERRIDE aturan generic %.o:%.c untuk file ini saja.
kernel/net_init.o: kernel/net_init.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# net_ping.c: butuh LWIP_CFLAGS karena include lwIP raw/icmp/dns headers
kernel/net_ping.o: kernel/net_ping.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# net_socket.c: butuh LWIP_CFLAGS karena include lwIP tcp headers
kernel/net_socket.o: kernel/net_socket.c
	$(CC) $(LWIP_CFLAGS) -c $< -o $@

# Tahap 1: Compile Assembly
%.o: %.asm
	$(AS) $(ASFLAGS) $< -o $@

# --- compile_commands.json untuk IntelliSense VS Code ---
# Menangkap flag compile PERSIS dari build sungguhan (via dry-run) sehingga
# IntelliSense tidak pernah out-of-sync dengan Makefile. Jalankan ulang setiap
# kali menambah file .c baru atau mengubah -I path.
#   Butuh: python -m pip install compiledb
.PHONY: compile_commands
compile_commands:
	python -m compiledb -n make clean all

# --- Host-side unit test (roadmap §11): virtqueue multi-chain ---
# Dikompilasi dengan compiler host (bukan freestanding) — mock MMIO berupa
# struct biasa di memori. TIDAK ikut build kernel (terkecualikan dari
# C_SOURCES, jalankan eksplisit: make test-virtqueue).
HOSTCC = clang
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

# --- USER APPS (ELF Terpisah, dimuat oleh Kernel via sys_load_elf) ---
# Panggil Makefile di dalam user_apps/ untuk mengompilasi fileman & viewer
.PHONY: apps
apps:
	$(MAKE) -C user_apps all

# --- RUST APPS (Phase 1: no_std userspace Rust) ---
# Cargo workspace ada di rust/; hasil akhir di-link dengan user_apps/app.ld
# yang sama (ELF64 single-base 0x4000000, PT_LOAD saja) agar bisa dimuat
# loader kernel. SELALU bangun lewat target ini: RUSTFLAGS meng-inject
# script linker (path relatif tidak bisa ditaruh di rust/.cargo/config.toml).
RUST_DIR  = rust
RUST_TRIP = x86_64-unknown-none
RUST_OUT  = $(RUST_DIR)/target/$(RUST_TRIP)/release

.PHONY: rust-apps
rust-apps:
	cd $(RUST_DIR) && RUSTFLAGS="-C relocation-model=static -C link-arg=-T../user_apps/app.ld" cargo build --release
	cp $(RUST_OUT)/hello-slint hello-slint.elf
	cp $(RUST_OUT)/control-center control-center.elf

.PHONY: rust-clean
rust-clean:
	cd $(RUST_DIR) && cargo clean

# Shortcut: bangun ELF secara individual
fileman.elf:
	$(MAKE) -C user_apps fileman

viewer.elf:
	$(MAKE) -C user_apps viewer

clock.elf:
	$(MAKE) -C user_apps clock

calc.elf:
	$(MAKE) -C user_apps calc

taskmgr.elf:
	$(MAKE) -C user_apps taskmgr

kill_test.elf:
	$(MAKE) -C user_apps kill_test

fd_test.elf:
	$(MAKE) -C user_apps fd_test

echo.elf:
	$(MAKE) -C user_apps echo

cat.elf:
	$(MAKE) -C user_apps cat

pipe_test.elf:
	$(MAKE) -C user_apps pipe_test

fork_test.elf:
	$(MAKE) -C user_apps fork_test

notepad.elf:
	$(MAKE) -C user_apps notepad

badptr.elf:
	$(MAKE) -C user_apps badptr

widget_demo.elf:
	$(MAKE) -C user_apps widget_demo

# Bersihkan hanya file objek user_apps (bukan ELF output)
clean-apps:
	$(MAKE) -C user_apps clean

# ISO: tergantung pada kernel + ELF apps (auto-rebuild jika source berubah)
# Tahap 3: Pembuatan ISO Hybrid (BIOS + UEFI 64-bit)
boot_image.iso: $(TARGET) apps rust-apps limine.conf kyuzen.png logo.png
	rm -rf iso_root
	mkdir -p iso_root
	# Buat folder EFI untuk standar boot UEFI 64-bit
	mkdir -p iso_root/EFI/BOOT
	cp limine/BOOTX64.EFI iso_root/EFI/BOOT/
	
	# Salin semua kebutuhan (termasuk limine-uefi-cd.bin)
	cp $(TARGET) limine.conf kyuzen.png logo.png fileman.elf viewer.elf clock.elf calc.elf taskmgr.elf notepad.elf badptr.elf widget_demo.elf desktop.elf terminal.elf settings.elf procinfo.elf exit_test.elf kill_test.elf fd_test.elf echo.elf cat.elf pipe_test.elf fork_test.elf hello-slint.elf control-center.elf limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	# Manifest launcher (name=/color=/hidden=), dibaca desktop.elf saat scan
	# app. Setiap file baru di manifests/ HARUS ditambah juga ke limine.conf.
	cp manifests/*.app iso_root/
	
	# Xorriso sakti: Menggabungkan BIOS dan UEFI ke dalam 1 file ISO!
	xorriso -as mkisofs -b limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o boot_image.iso
		
	./limine/limine.exe bios-install boot_image.iso

# Tahap 4: Boot up QEMU (Dengan Fitur Debugging 64-bit)
run: boot_image.iso
	qemu-system-x86_64.exe -cpu max -m 1G -boot d \
		-smp 8 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000

# run + serial stdio: tangkap panic dump ke terminal (bukan cuma framebuffer BSOD)
.PHONY: run-serial
run-serial: boot_image.iso
	qemu-system-x86_64.exe -cpu max -m 1G -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000 \
		-serial stdio

# run + serial ke FILE (lebih andal di Windows daripada stdio): panic dump
# tertulis ke serial.log — buka & paste setelah QEMU berhenti.
.PHONY: run-wd
run-wd: boot_image.iso
	qemu-system-x86_64.exe -cpu max -m 1G -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000 \
		-serial file:serial.log

# Stress test: recursive make with STRESS_TEST flag + debug/ sources
.PHONY: stress
stress:
	$(MAKE) clean
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) debug" CFLAGS="$(CFLAGS) -DSTRESS_TEST"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000

# Concurrency test: sleep/mutex/semaphore/condvar (Fase 1-3) + test/ sources
.PHONY: conc
conc:
	$(MAKE) clean
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) test" CFLAGS="$(CFLAGS) -DCONC_TEST -Itest"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000

# Heap stress test: overflow guard + canary corruption detection (test/ sources)
.PHONY: heap-stress
heap-stress:
	$(MAKE) clean
	$(MAKE) boot_image.iso SRC_DIRS="$(SRC_DIRS) test" \
		CFLAGS="$(CFLAGS) -g -DHEAP_STRESS_TEST -Itest"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000

.PHONY: heap-watch
heap-watch:
	$(MAKE) clean
	$(MAKE) boot_image.iso CFLAGS="$(CFLAGS) -g -DHEAP_WATCH_DEBUG"
	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
		-smp 4 \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-drive file=boot_image.iso,media=cdrom,index=2 \
		-nic user,model=e1000 \
		-serial stdio

# Bersihkan file hasil build (kernel + lwIP objects)
clean:
	rm -f $(OBJS) $(LWIP_OBJS) $(TARGET) debug/pmm_stress.o debug/pmm_valid.o test/conc_test.o test/heap_stress_test.o

# run: boot_image.iso
# 	qemu-system-x86_64.exe -cpu max -m 512M -boot d \
# 		-drive file=disk.img,format=raw,index=0,media=disk \
# 		-drive file=boot_image.iso,media=cdrom,index=2 \
# 		-no-reboot -no-shutdown
