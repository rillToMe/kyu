# Peta Kelengkapan Subsistem `arch/x86/`

Dokumen ini memetakan subsistem apa saja yang lazim ada di `arch/x86/` sebuah
kernel x86-64, mana yang **sudah** kita punya, mana yang **belum**, dan - yang
sama pentingnya - mana yang **tidak perlu** kita punya.

Tujuannya bukan mengejar jumlah file. Tujuannya menjawab satu pertanyaan:
*apakah `arch/x86/` kita kurang sesuatu yang penting, atau sudah proporsional?*

> **Cara memakai dokumen ini.** Bagian 3 dan 4 adalah daftar kerja. Bagian 5
> adalah daftar "jangan dikerjakan" - bacalah sebelum menambah apa pun, karena
> sebagian besar `arch/x86/` Linux tidak relevan untuk lingkup proyek ini.

---

## 1. Batas perbandingan

Pembandingnya adalah `arch/x86/` Linux `master`, diambil lewat GitHub Trees API.

| | File | Ukuran |
| --- | --- | --- |
| Linux `arch/x86/` | 1.408 | 13.101 KB |
| KyuzenOS `arch/x86/` | 17 | 57 KB |
| KyuzenOS `arch` + `kernel` + `drivers` + `include` | 176 | 1.540 KB |

Rasio 83x itu menyesatkan, dan penyebabnya terukur:

| Penyebab | Bukti di Linux |
| --- | --- |
| Melayani 3 ABI sekaligus | `ia32/` 2 file, `realmode/` 23, `um/` 70, `entry_32.S` 32 KB |
| Hypervisor + paravirtualisasi | `kvm/` 94 file (2.893 KB), `xen/` 31, `hyperv/` 14, `coco/` 14, `virt/` 14 |
| Kebun binatang hardware | `platform/` 53 file (OLPC, Geode, Intel MID, CE4100, TS5500, SCX200), `kernel/cpu/` 43 file vendor (cyrix, centaur, transmeta, hygon, zhaoxin, vortex, acrn, bhyve, vmware) |
| Header hasil generate | 380 dari 430 file `include/` adalah `asm-offsets.h`, `cpufeatures.h`, `syscalls.h`, `unistd.h` |
| Konvensi Kbuild | 353 file (25%) berukuran < 1 KB - `Makefile`, `Kbuild`, `Kconfig`, glue |
| Fitur opsional besar | `events/` (perf) 39 file 1.303 KB, `crypto/` (AES-NI/Camellia/Serpent/SM4/ARIA asm) 57 file 760 KB, `vdso/` 40, `boot/` 82 |

Sebagai kalibrasi: `kernel/idt.c` Linux **9,7 KB**, `arch/x86/idt.c` kita
**10 KB**. Pada inti yang benar-benar setara, ukurannya sama.

---

## 2. Peta subsistem

Status: **ADA** (memadai) - **SEBAGIAN** (ada, dengan celah bernama) - **BELUM**.

### 2.1 Interrupt & exception

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| IDT, 256 vektor, DPL3 | `kernel/idt.c` 9,7 KB | `arch/x86/idt.c` 10 KB + `vectors.c` | **ADA** |
| ISR stub + frame | `entry/entry_64.S` 44 KB, `kernel/traps.c` 45 KB | `exceptions.asm` + 7 stub + `isr_macro.inc` | **ADA** |
| IST untuk #DF/NMI/#MC | `entry/entry_64.S` | ARCH-2a | **ADA** |
| Local APIC (xAPIC) | `kernel/apic/apic.c` 67 KB | `arch/x86/lapic.c` 401 baris | **ADA** |
| x2APIC | `kernel/apic/` | hanya `MSR_IA32_APIC_BASE` dibaca; MMIO saja | **BELUM** |
| IOAPIC + redirection table | `kernel/apic/ioapic.c` 21 KB | - | **BELUM** |
| ACPI MADT (routing IRQ) | `kernel/acpi/` 11 file | - | **BELUM** |
| 8259 PIC | `kernel/i8259.c` 26 KB | `drivers/keyboard.c:63` `pic_remap()` | **SEBAGIAN** |
| MSI / MSI-X | `kernel/apic/` | - | **BELUM** |
| EOI, spurious LAPIC | `kernel/apic/apic.c` | `vectors.c:57` `pic_eoi()`, `lapic.c:338` | **ADA** |
| Machine Check (#MC) | `kernel/mce/` | vektor 18 -> panic generik | **BELUM** |
| NMI watchdog | `kernel/nmi.c` 21 KB | - | **BELUM** |

Catatan penting: **hanya tiga IRQ eksternal yang benar-benar dipakai** - IRQ0
(PIT), IRQ1 (keyboard), dan IRQ12 (mouse). Semuanya lewat 8259 PIC, yang
di-remap ke vektor `0x20`-`0x2F` oleh `pic_remap()`. Unmask-nya terlihat di
`drivers/keyboard.c:68` (`outb(0x21, 0xF8); outb(0xA1, 0xEF)`).

Sisanya **polling**, bukan interrupt:

| Perangkat | Cara kerja | Bukti |
| --- | --- | --- |
| ATA PIO | polling status register | `drivers/ata.c:66` - "Polling: Tunggu DRQ" |
| e1000 | dipanggil dari callback timer PIT | `e1000.c:20`, `e1000.c:546` `e1000_poll()` |
| virtio-gpu | polling virtqueue | `virtio_gpu_dev.c:102` - "ISR_CFG tidak dipakai v1 (polling)" |

Konsekuensi yang perlu disadari: karena tidak ada IOAPIC, **tidak ada cara
merutekan IRQ ke CPU selain BSP**. LAPIC timer memang jalan di tiap AP, tapi
ketiga IRQ itu terpusat di satu core. Selama sisanya polling, ini belum menjadi
masalah - tapi begitu ada perangkat yang butuh interrupt (mis. NIC dengan
throughput tinggi, atau ATA DMA), keterbatasan ini langsung terasa.

### 2.2 Waktu

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| PIT 8254 | `kernel/i8254.c` 21 KB | `drivers/timer.c:142` `timer_program_pit()` | **ADA** |
| LAPIC timer + kalibrasi | `kernel/apic/apic.c` | `arch/x86/lapic.c` (ARCH-12) | **ADA** |
| TSC | `kernel/tsc.c` 41 KB | hanya `rdtsc` di benchmark (`intel_bench.c`) | **BELUM** |
| TSC-deadline | `kernel/tsc.c` | - | **BELUM** |
| HPET | `kernel/hpet.c` 36 KB | - | **BELUM** |
| RTC / CMOS | `kernel/rtc.c` | `drivers/rtc.c` | **ADA** |
| Clock event/source framework | `kernel/time.c` | `kernel/timer_callbacks.c` (8 slot) | **SEBAGIAN** |

Sumber waktu sekarang: PIT kanal 0 pada 60 Hz (default), LAPIC timer 100 Hz
untuk tick scheduler. `sys_uptime` mengembalikan `timer_get_ms()` - resolusinya
terikat tick PIT.

### 2.3 CPU & feature

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| Deteksi vendor/model | `kernel/cpu/common.c`, `proc.c` | `kernel/cpu.c` `get_cpu_string()` | **ADA** |
| Framework feature CPUID | `kernel/cpu/cpufeatures.h`, `cpuid-deps.c` | tidak ada - tiap fitur dicek ad-hoc | **BELUM** |
| NX (EFER.NXE) | `kernel/cpu/common.c` | ARCH-5 `cpu_enable_nx()` | **ADA** |
| SMEP / SMAP | `kernel/cpu/common.c` | `smap.h`, `kernel.c` | **ADA** |
| CR0.WP | - | `cpu_verify_wp()` | **ADA** |
| UMIP | `kernel/umip.c` 14 KB | - | **BELUM** |
| PCID / INVPCID | `mm/` | - | **BELUM** |
| PAT | `kernel/pat/` 6 file 109 KB | - | **BELUM** |
| MTRR | `kernel/mtrr.c` | - | **BELUM** |
| Microcode | `kernel/microcode/` | - | **BELUM** |
| Topologi (SMT/core/NUMA) | `kernel/cpu/topology.c` | `smp.c` (LAPIC id saja) | **SEBAGIAN** |
| CPU hotplug | `kernel/cpu/` | - | **BELUM** |
| Cpufreq / thermal / RAPL | `kernel/cpu/`, `power/` | - | **BELUM** |

### 2.4 Memori

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| Page table 4-level | `mm/` 44 file 524 KB | `kernel/mm/paging.c` 821 baris | **ADA** |
| Per-process AS + clone | `mm/` | `vmm_create_address_space`, `vmm_clone_user_as` | **ADA** |
| W^X via NX + `p_flags` | `mm/mprotect.c` | ARCH-5 | **ADA** |
| Guard page stack | `mm/` | ARCH-2b `task_stack.c` | **ADA** |
| TLB shootdown lintas-CPU | `mm/tlb.c` 52 KB | **lokal saja** - `paging.c:68` | **SEBAGIAN** |
| COW + refcount halaman | `mm/` | ditolak - `paging.c:573` | **BELUM** |
| Exception fixup table | `mm/extable.c` 13 KB | ARCH-7 (OPEN) | **BELUM** |
| KPTI | `mm/pti.c` 18 KB | ARCH-14 (OPEN) | **BELUM** |
| Huge page (2 MB / 1 GB) | `mm/hugetlbpage.c` | - | **BELUM** |
| KASLR | `kernel/kaslr.c` | - | **BELUM** |
| Page table dump/debug | `mm/dump_pagetables.c`, `debug_pagetables.c` | `kernel/mm/heap_watch.c` (DR0 watchpoint) | **SEBAGIAN** |

### 2.5 Entry & syscall

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| `int 0x80` (vektor 128) | `entry/entry_64_compat.S` | `arch/x86/isr128.asm`, DPL=3 | **ADA** |
| ABI & dispatch syscall | `kernel/syscall_64.c` | `kernel/syscall/syscall.c`, 89 nomor | **ADA** |
| Boundary copy + SMAP window | `lib/usercopy_64.c` | `kernel/proc/usercopy.c` | **ADA** |
| `SYSCALL`/`SYSRET` (MSR STAR/LSTAR) | `entry/entry_64.S` | - (EFER.SCE tidak diset) | **BELUM** |
| vDSO | `vdso/` 40 file | - | **BELUM** |
| Signal | `kernel/signal.c` 15 KB | tidak ada | **BELUM** |
| ptrace | `kernel/ptrace.c` 43 KB | tidak ada | **BELUM** |
| LDT | `kernel/ldt.c` 17 KB | - | **BELUM** |
| vm86 / 32-bit compat | `kernel/vm86_32.c`, `ia32/` | - | **BELUM** |
| FPU/SSE context | `kernel/fpu/` 11 file | ARCH-10 `FXSAVE64` | **SEBAGIAN** |
| XSAVE / AVX | `kernel/fpu/xstate.c` | ditolak dengan alasan - `fpu.c:39` | **BELUM** |
| AMX | `kernel/fpu/` | - | **BELUM** |

### 2.6 SMP

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| Bring-up AP | `kernel/smpboot.c` 39 KB | `arch/x86/smp_ap_entry.asm`, Limine MP | **ADA** |
| Per-CPU data | `kernel/setup_percpu.c` | ARCH-9, `GS:0` -> `percpu_t` | **ADA** |
| IPI | `kernel/apic/` | `lapic.c` reschedule IPI (vektor 253) | **ADA** |
| Spinlock | `kernel/spinlock.c` | `include/spinlock.h`, `kernel/sync/spinlock.c` | **ADA** |
| Atomics | `include/asm/atomic.h` | hand-rolled `lock xchg`/`lock xadd` di `spinlock.h`, `smp.c` | **SEBAGIAN** |
| RCU | `kernel/` | - | **BELUM** |
| Workqueue | `kernel/workqueue.c` | - | **BELUM** |
| TLB shootdown IPI | `mm/tlb.c` | - | **BELUM** |
| `swapgs` / KERNEL_GS_BASE | `entry/entry_64.S` | MSR didefinisikan, belum dipakai | **BELUM** |

### 2.7 Debug & diagnosa

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| Panic + BSOD | `kernel/panic.c` | `kernel/panic/` 5 file 912 baris | **ADA** |
| Crash dump | `kernel/crash_dump_64.c` | `kernel/debug/crashdump.c`, `crash_archive.c` | **ADA** |
| Backtrace | `kernel/dumpstack.c` | `panic_explain.c` RBP-chain + scan | **SEBAGIAN** |
| ORC unwinder | `kernel/unwind_orc.c` 20 KB | - | **BELUM** |
| `WARN_ON` / `BUG` / `ASSERT` | `include/asm/bug.h` | tidak ada macro apa pun | **BELUM** |
| lockdep | `kernel/locking/` | - | **BELUM** |
| KASAN | `mm/kasan/` | - | **BELUM** |
| ftrace | `kernel/ftrace.c` | - | **BELUM** |
| kprobes | `kernel/kprobes/` | - | **BELUM** |
| kgdb | `kernel/kgdb.c` | - | **BELUM** |
| Selftest runtime | `kernel/nmi_selftest.c`, `insn_sanity.c` | `tests/host/` (host, bukan in-OS) | **SEBAGIAN** |

### 2.8 Boot, platform, dan sisanya

| Subsistem | Padanan Linux | Kita | Status |
| --- | --- | --- | --- |
| Boot protocol | `boot/` 82 file | Limine (protokol 3) | **ADA** |
| Memory map | `kernel/e820.c` | Limine memmap (`pmm.c:89`) | **ADA** |
| ACPI | `kernel/acpi/` 11 file | `drivers/acpi.c` - FADT saja, untuk S5 | **SEBAGIAN** |
| EFI | `boot/efi/` 10 file, `efi/` | - (Limine menangani) | **BELUM** |
| PCI config | `pci/` 23 file 243 KB | `drivers/pci.c` - legacy `0xCF8`/`0xCFC` | **SEBAGIAN** |
| PCIe ECAM (MCFG) | `pci/` | - | **BELUM** |
| IOMMU / VT-d | `kernel/` | - | **BELUM** |
| DMI / SMBIOS | `kernel/dmi_scan.c` | - | **BELUM** |
| Alternatives | `kernel/alternative.c` 76 KB | - | **BELUM** |
| Retpoline / mitigasi Specre | `kernel/retpoline.S`, `kernel/cpu/bugs.c` | - | **BELUM** |
| RDRAND / RDSEED | `lib/` | `kernel/entropy.c` | **ADA** |
| Power (suspend/hibernate) | `power/` 7 file | S5 shutdown saja (`drivers/acpi.c`) | **SEBAGIAN** |
| Kexec / purgatory | `kernel/kexec*`, `purgatory/` | - | **BELUM** |
| Video / VGA | `kernel/video*.c` | `kernel/display.c` | **ADA** |

---

## 3. Prioritas: yang perlu dikerjakan

Klasifikasi: **P0** = bug kebenaran/keamanan yang ada di kode sekarang -
**P1** = dibutuhkan untuk "production-worthy" pada lingkup yang sudah diklaim -
**P2** = berguna, tidak memblokir.

### P0 - bug, bukan fitur

**P0-1. TLB shootdown lintas-CPU**

- **Celah.** `vmm_tlb_shootdown()` (`kernel/mm/paging.c:68`) hanya menjalankan
  `invlpg` di CPU pemanggil. Kodenya sendiri mencatat ini sebagai utang
  (`paging.c:575`, `paging.c:399`).
- **Dampak.** Ada dua jalur, dan keduanya nyata:

  1. **Halaman user.** `vmm_unmap_page_from()` (`paging.c:437`) meng-invalidate
     hanya di CPU pemanggil. `uheap_unmap_range` (`uheap.c:30`) dan
     `task_stack_free` (`task_stack.c:138`) membebaskan frame tepat setelahnya.
     Bila task bermigrasi ke CPU lain sebelum unmap, CPU itu masih punya entri
     TLB basi - dan frame itu bisa langsung dialokasikan ulang oleh proses lain.
  2. **Halaman kernel.** Stack task di-map ke **kernel PML4** (`task_stack.c:99`
     memakai `vmm_alloc_page_kernel`), dan kernel PML4 di-*shallow copy* ke
     setiap address space (`paging.c:101`). Jadi mapping itu terlihat oleh semua
     AS, dan `task_stack_free` melepasnya dari tabel bersama.

- **Yang meredam sekarang.** `core.c:214` hanya menulis CR3 bila AS **berbeda**
  (`cpu->current_cr3 != new_cr3`), jadi switch antar-task dalam AS yang sama
  tidak mem-flush apa pun. Tidak ada bit GLOBAL di PTE mana pun (diverifikasi:
  tidak ada `PAGE_GLOBAL` di repo), jadi setiap tulis CR3 memang mem-flush
  seluruh TLB non-global - tapi itu hanya menolong bila CR3-nya benar-benar
  berubah.
- **Kenapa belum meledak.** Belum ada COW, belum ada thread yang berbagi
  `PML4` (fork = salinan penuh), dan `next_stack_vaddr` monotonik
  (`task_stack.c:117`) sehingga VA stack tidak dipakai ulang. Jendelanya sempit,
  tapi bukan nol - `heap.c:152` **memang** memakai ulang VA saat ekspansi gagal
  di tengah jalan (`current_heap_end = start_expansion_addr`).
- **Prasyarat.** IPI sudah ada (`lapic.c:369`), infrastruktur per-CPU sudah ada
  (`percpu_t.current_cr3` di `smp.h:18`). Butuh vektor IPI khusus + pelacakan AS
  aktif di tiap CPU + acknowledgment.

**P0-2. Backtrace tidak andal pada `-O2`**

- **Celah.** `panic_bt_collect()` (`panic_explain.c:234`) mencoba rantai RBP
  lebih dulu; komentarnya sendiri mengakui "-O2 sering menghilangkan frame
  pointer". Build kernel memakai `-O2` (`cmake/KyuzenCompiler.cmake:64`) tanpa
  `-fno-omit-frame-pointer`, dan tidak ada flag itu di seluruh `cmake/`.
- **Dampak.** Jalur pertama gagal, dan satu-satunya fallback adalah pemindaian
  768 qword pada stack yang menyaring `KERNEL_VMA..KERNEL_VMA+0x2000000` -
  heuristik yang bisa melewatkan frame atau melaporkan nilai yang kebetulan
  tampak seperti alamat kode. Untuk kernel yang punya crash dump
  (`kernel/debug/`: `crashdump.c`, `crash_archive.c`, `panic_log.c`) dan BSOD
  yang menampilkan backtrace (`panic_draw.c`), ini melemahkan justru bagian yang
  paling dibutuhkan saat ada bug.
- **Usaha.** Kecil - satu flag. **Tapi wajib boot-test ulang 17/17**, karena ini
  mengubah codegen dan baseline terverifikasi dibangun tanpa flag ini.

**P0-3. Hapus `paging_map_region()` - dead code dengan flag salah**

- **Celah.** `paging.c:470`, dideklarasikan di `paging.h:47`, tidak dipanggil di
  mana pun. Isinya `vmm_alloc_page(vaddr, 7)` - pemetaan halaman kernel dengan
  bit USER.
- **Dampak.** Tidak ada dampak runtime (tidak dipanggil). Risikonya adalah
  *contoh*: siapa pun yang mencari cara memetakan halaman kernel akan menemukan
  fungsi ini, melihat flag `7`, dan menyalinnya. Itu persis pola yang membuat
  pertahanan berlapis di 4.2 hilang.
- **Usaha.** Sangat kecil - hapus fungsi dan deklarasinya.

### P0 - ringkasan

| ID | Item | Usaha | Dampak |
| --- | --- | --- | --- |
| P0-1 | TLB shootdown lintas-CPU | Besar | Korupsi memori lintas-proses (tertunda) |
| P0-2 | `-fno-omit-frame-pointer` | Kecil | Backtrace andal pada `-O2` |
| P0-3 | Hapus `paging_map_region()` | Sangat kecil | Menghapus contoh flag yang salah |

### P1 - kelengkapan produksi

**P1-1. Framework deteksi fitur CPU.** *Leverage tertinggi di daftar ini.*
Saat ini setiap fitur dicek ad-hoc: `cpu_enable_nx()` memanggil CPUID inline
(`kernel/cpu.c:40`), `smap.h` mengecek sendiri. Setiap item di bawah butuh
feature gating. Satu bitmap `cpu_features` + `cpu_has(X)` + probe CPUID terpusat
akan dipakai ulang oleh UMIP, PAT, PCID, x2APIC, XSAVE, MSI, dan RDRAND.

**P1-2. IOAPIC + MADT.** *Prioritasnya lebih rendah dari yang terlihat.*

Hanya tiga IRQ eksternal yang dipakai (PIT, keyboard, mouse), dan ketiganya
perangkat lambat yang tidak masalah terpusat di BSP. ATA, e1000, dan virtio-gpu
semuanya polling. Jadi IOAPIC **bukan** penghambat hari ini.

Yang membuatnya tetap P1 adalah apa yang dibukanya: IRQ affinity (mendistribusi
beban ke AP), jumlah IRQ tidak lagi dibatasi 15, dan - yang paling penting -
menjadi **prasyarat MSI/MSI-X**. Begitu ada driver yang butuh interrupt
sungguhan (NIC throughput tinggi, ATA DMA, virtio dengan notification), atau
begitu perangkat mulai memakai MSI (yang tidak lewat PIC sama sekali), IOAPIC
menjadi wajib. Urutkan setelah P0-1 dan P1-1 selesai, bukan sebelum.

**P1-3. Signal.** Tanpa signal, proses tidak punya cara menerima pemberitahuan
asinkron - `kill` saat ini langsung mengeksekusi `proc_exit_kill()`. Ini juga
yang membuat `SIGSEGV` tidak bisa dipakai untuk handler user-space.

**P1-4. `SYSCALL`/`SYSRET`.** `int 0x80` melewati IDT gate; `SYSCALL` melewati
itu dan menghemat ~100 siklus per panggilan. Untuk OS dengan 89 syscall dan
aplikasi GUI yang memanggilnya tiap frame, ini terasa langsung. Prasyarat:
MSR `STAR`/`LSTAR`/`SFMASK`, set `EFER.SCE`, dan jalur entry asm baru.

**P1-5. vDSO.** Sekali `SYSCALL` ada, jam/waktu masih perlu syscall per
pembacaan. Aplikasi `clock`, taskbar, dan animasi membacanya terus-menerus.
vDSO menghilangkan syscall itu sepenuhnya.

**P1-6. PAT.** Diperlukan untuk write-combining pada framebuffer. Mapping
framebuffer sekarang milik Limine (`paging.c:704` menegaskan itu bukan milik
kita), jadi tipe memori tidak terkontrol. Untuk grafis, ini perbedaan performa
yang besar.

**P1-7. TSC.** Sumber waktu resolusi tinggi yang murah, dan TSC-deadline
sebagai timer per-CPU yang tidak butuh kalibrasi sama sekali - menghapus seluruh
kerumitan ARCH-12.

**P1-8. `include/atomic.h` + refcount.** Atomik sekarang tersebar sebagai
`lock xchg`/`lock xadd` inline di `spinlock.h` dan `smp.c`. Refcount dengan
proteksi overflow adalah prasyarat COW.

**P1-9. UMIP.** Satu bit CR4. Mematikan `SGDT`/`SIDT`/`SLDT`/`SMSW`/`STR` dari
ring 3, yang membocorkan alamat tabel kernel. Perbandingan manfaat/usaha terbaik
di dokumen ini.

**P1-10. `WARN_ON` / `BUG_ON` / `ASSERT`.** Tidak ada satu pun macro diagnostik
di kernel. Sebuah kernel produksi butuh jalur "lapor dan lanjut" yang tidak
mematikan mesin.

**P1-11. SSE/AVX di userspace.** **SELESAI (Phase 3).**

`CR4.OSFXSR` + `OSXMMEXCPT` diset, `CR0` (`EM=0`, `MP=1`, `NE=1`) sekarang juga
diset (`fpu.c`), dan save/restore `FXSAVE64` per task sudah terpasang. Flag
userspace tidak lagi memuat `-mno-sse -mno-sse2 -mno-mmx -msoft-float`, jadi
`double`/`float` bisa dikompilasi dan libc bisa mengembalikan `double`
(`difftime` aktif), dan JPEG diaktifkan kembali di `libs/media/png.c`.

**KERNEL TETAP `-mno-sse`, dan itu keputusan sadar — bukan sisa kebijakan.**

Roadmap FASE 3 langkah 4 meminta flag itu dicabut dari kernel "secara
bertahap". **Langkah itu tidak dikerjakan, karena salah.** Alasannya:

- Interrupt entry TIDAK menyimpan XMM. `arch/x86/isr_macro.inc` hanya mendorong
  register umum, dan `fpu_switch()` hanya berjalan saat context switch.
- Jadi kode kernel yang memakai SSE akan menimpa XMM task ring-3 yang sedang
  berjalan setiap kali interrupt menyala di tengah kode itu, dan task tersebut
  lanjut dengan nilai FP yang salah.
- Itu bukan hipotesis: `kernel/string.c` tanpa `-mno-sse` menghasilkan
  `movups %xmm0` di `memcpy`, dan `memcpy` dipanggil dari jalur scheduler dan
  filesystem yang berjalan di dalam interrupt.

Karena itu flag kernel dipindahkan ke `KYUZEN_KERNEL_FP_BAN`
(`cmake/KyuzenCompiler.cmake`) dan dijaga setelah link oleh
`cmake/verify_kernel_elf.cmake` — build GAGAL bila image kernel memuat SSE/x87.
Sisi userspace dijaga terpisah oleh `cmake/verify_user_elf.cmake`, yang
sekarang MENGIZINKAN xmm/x87 dan hanya menolak `ymm`/`zmm`.

Ketergantungan untuk AVX: XSAVE (bukan hanya FXSAVE) menjadi wajib begitu AVX
dibuka, karena `FXSAVE` tidak menyimpan bagian atas YMM/ZMM. Itulah sebabnya
penjaga userspace menolak `ymm`/`zmm` hari ini.

### P2 - berguna, tidak memblokir

| Item | Catatan |
| --- | --- |
| x2APIC | Butuh hanya untuk >255 CPU; MMIO masih cukup |
| MSI / MSI-X | Butuh IOAPIC dulu; NIC dan virtio-gpu sekarang polling |
| HPET | Sumber kalibrasi lebih baik dari PIT |
| PCID / INVPCID | Menghemat flush TLB saat context switch; tanpa INVPCID, PCID berbahaya |
| KASLR | Limine mendukung; nilai keamanan nyata, bukan kosmetik |
| Huge page 2 MB | Mengurangi tekanan TLB untuk heap besar |
| ORC unwinder | Menghapus ketergantungan pada RBP; usaha besar |
| lockdep | Deteksi deadlock; ada nilai karena banyak lock, tapi berat |
| KASAN | Mahal, tapi menangkap kelas bug yang paling mahal |
| ftrace / kprobes | Tracing; bermanfaat untuk performa GUI |
| ptrace | Prasyarat debugger user-space |
| DMI / SMBIOS | Mengetahui model mesin; QEMU menyediakannya |
| PCIe ECAM | Butuh untuk >256 bus |
| Alternatives | Patching kode saat boot; berguna setelah mitigasi Specre ada |
| NMI watchdog | Deteksi CPU hang; butuh timestamp per-CPU |
| Retpoline / mitigasi Specre | Hanya relevan bila kode tak tepercaya berjalan di mesin nyata |
| LDT | Hanya bila 32-bit compat / Wine masuk lingkup |
| Machine Check (#MC) | Di QEMU praktis tidak pernah fire; di hardware nyata wajib |
| Microcode | Memerlukan pemuatan sangat awal; Limine tidak menyediakannya |
| Power (S3/hibernate) | Di luar lingkup desktop saat ini |
| RCU / workqueue | Dua-duanya alternatif untuk primitif yang sudah ada; hanya perlu bila skalanya menuntut |
| COW + refcount | Ditambah setelah P0-1 selesai, bukan sebelumnya |
| Exception fixup table (ARCH-7) | Menunggu jalur verifikasi; lihat `arch-hardening.md` |
| KPTI (ARCH-14) | Menunggu jalur verifikasi; lihat `arch-hardening.md` |

---

## 4. Catatan keamanan yang ditemukan saat pemetaan

### 4.1 Ketergantungan tersembunyi pada `FSGSBASE` yang mati

`kernel/smp/smp.c:190` (`smp_set_gs_base`) membiarkan `IA32_GS_BASE` menunjuk ke
`smp_percpu[cpu_id]` - alamat **kernel** - dan nilai itu tetap terpasang selama
ring 3 berjalan. Tidak ada `swapgs`; `include/msr.h:18` mencatat
`IA32_KERNEL_GS_BASE` didefinisikan tapi belum dipakai. `smp_current_cpu_index()`
(`smp.c:165`) lalu membaca `gs:0` dan mempercayai hasilnya bila
`< SMP_MAX_CPUS`.

Aman **sekarang**, dan alasannya berlapis:

- Tidak ada satu pun jalur syscall yang menyetel GS/FS base dari ring 3
  (tidak ada `arch_prctl`, tidak ada `wrgsbase`). Diverifikasi dengan memindai
  seluruh `kernel/`.
- `wrgsbase` dari ring 3 menghasilkan `#UD` selama `CR4.FSGSBASE` (bit 16) tidak
  diset - dan bit itu tidak pernah diset di mana pun.
- Kalaupun GS base diubah, alamat kernel tidak dapat di-*deref* dari ring 3
  karena leaf PTE higher-half dibuat tanpa bit USER (`heap.c:141` memakai flag
  `3`; `task_stack.c:99` memakai `PRESENT | WRITABLE`).

Yang tidak ada adalah **pencatatan ketergantungannya**. **Bila
`CR4.FSGSBASE` diaktifkan suatu saat - misalnya untuk mempercepat TLS - ring 3
bisa menyetel GS base ke alamat miliknya, dan `smp_current_cpu_index()` akan
membaca indeks CPU yang dikendalikan penyerang.** Perbaikan yang benar saat itu
adalah `swapgs` di setiap entry, bukan sekadar mengandalkan bit CR4.

### 4.2 Bit USER pada tabel perantara higher-half

`vmm_create_address_space()` (`paging.c:118`) dan `vmm_map_page()` (`paging.c:232`,
`248`, `264`) membuat entri tabel perantara PML4/PDPT/PD dengan flag `| 7` -
yaitu `PRESENT | WRITABLE | USER`. Bit USER di level perantara **tidak** dengan
sendirinya membuka akses ring 3: bit USER harus diset di **semua** level
termasuk leaf PTE, dan leaf PTE kernel memang dibuat tanpa bit itu.

Jadi ini bukan lubang, tapi **pertahanan berlapisnya hilang**. Satu bug di masa
depan yang membuat sebuah leaf PTE kernel keliru memakai flag `7` alih-alih `3`
akan langsung memberi ring 3 akses ke halaman itu, karena seluruh jalur di
atasnya sudah bertanda USER. Flag yang benar untuk tabel perantara higher-half
adalah `3` (`PRESENT | WRITABLE`).

Ditemukan juga: `paging_map_region()` (`paging.c:470`) adalah **dead code** -
dideklarasikan di `include/paging.h:47` tapi tidak punya satu pun pemanggil di
seluruh repo. Isinya `vmm_alloc_page(vaddr, 7)`, yaitu pemetaan kernel dengan
bit USER. Menghapusnya (beserta deklarasinya) menghilangkan satu contoh pola
yang salah sekaligus satu fungsi mati.

### 4.3 Dokumentasi FPU/SSE yang sempat basi

`ARCH-10` menyetel `CR4.OSFXSR` + `CR4.OSXMMEXCPT` (`kernel/sched/fpu.c:106-107`)
dan memasang save/restore `FXSAVE64` per task. Tapi **lima tempat sempat
menyatakan kernel tidak pernah menyetel `CR4.OSFXSR`**:

| File | Baris | Status |
| --- | --- | --- |
| `cmake/KyuzenCompiler.cmake` | 32 | sudah dikoreksi |
| `cmake/KyuzenApplication.cmake` | 453 | sudah dikoreksi |
| `cmake/verify_user_elf.cmake` | 85, 103 | sudah dikoreksi |
| `docs/development/build-system-audit.md` | 182 | sudah dikoreksi |
| `docs/ROADMAP-OSDEV.md` | 351-364 (FASE 3.1) | sudah dikoreksi |

Flag `-mno-sse -mno-sse2 -mno-mmx -msoft-float` **masih benar untuk KERNEL** dan
masih harus dipertahankan — tapi **alasannya bukan lagi `#UD`**, dan bukan pula
sekadar "disiplin ABI".

Alasan yang benar: **interrupt entry tidak menyimpan XMM.**
`arch/x86/isr_macro.inc` hanya mendorong register umum (`PUSHA64`), dan
`fpu_switch()` hanya berjalan saat context switch. Kode kernel yang memakai SSE
akan menimpa XMM task ring-3 yang sedang berjalan setiap kali interrupt menyala
di tengah kode itu — dan task tersebut lanjut dengan nilai FP yang salah.
Terverifikasi bukan hipotetis: `kernel/string.c` tanpa `-mno-sse` menghasilkan
`movups %xmm0` di `memcpy`, dan `memcpy` dipanggil dari jalur scheduler dan
filesystem yang berjalan di dalam interrupt.

Itulah juga sebabnya **userspace justru dibuka** (Phase 3): `fpu_switch()`
memang berjalan di antara task, jadi user code boleh memakai FP. Asimetri ini
adalah keseluruhan desainnya.

Penegakannya sekarang mekanis, bukan komentar:
`cmake/verify_kernel_elf.cmake` menggagalkan build bila image kernel memuat
SSE/x87, dan flag itu sendiri hidup di `KYUZEN_KERNEL_FP_BAN`
(`cmake/KyuzenCompiler.cmake`).

**Langkah 4 ROADMAP FASE 3.1 ("cabut `-mno-sse` dari kernel secara bertahap")
DIBATALKAN** — lihat P1-11 di atas untuk alasannya.

Yang tersisa dari fase itu hanya langkah 3 (`#NM` / lazy FPU), yang eksplisit
ditandai opsional di `fpu.c`. `CR0` (clear `EM`/`TS`, set `MP`/`NE`) sudah
diset sekarang, jadi langkah 1 selesai seluruhnya.

---

## 5. Yang TIDAK perlu, dan alasannya

Daftar ini sepenting daftar kerja. Setiap baris pernah saya pertimbangkan dan
tolak.

| Tidak perlu | Alasan |
| --- | --- |
| `kvm/`, `xen/`, `hyperv/`, `coco/`, `virt/` | Kita OS yang *dijalankan*, bukan hypervisor. 167 file di Linux. Tidak akan pernah masuk lingkup. |
| `ia32/`, `realmode/`, `entry_32.S`, `um/` | Kita tidak mendukung 32-bit maupun UML. Kalau nanti butuh menjalankan binary 32-bit, itu proyek tersendiri. |
| `platform/` (OLPC, Geode, Intel MID, CE4100, TS5500, SCX200) | Perangkat yang sudah tidak diproduksi. Kita jalan di QEMU generik. |
| `kernel/cpu/` vendor (cyrix, centaur, transmeta, hygon, zhaoxin, vortex, acrn, bhyve, vmware) | Quirk vendor. Satu jalur kode generik sudah benar untuk kita. |
| `crypto/` (AES-NI, Camellia, Serpent, Twofish, SM4, ARIA asm) | Sudah ada di `third_party/bearssl`. Tidak ada alasan menulis ulang. |
| `net/` (BPF JIT) | Tidak ada eBPF di kita. |
| e820 | Limine sudah memberi memory map (`pmm.c:89`). Menambah e820 hanya duplikasi. |
| Multiboot | Limine sudah jadi boot protocol kita. |
| MP table (bukan ACPI) | Usang; MADT menggantikannya. Bila IRQ routing dikerjakan, MADT yang benar. |
| `purgatory/`, kexec | Tidak ada skenario kita yang butuh reboot ke kernel lain. |
| CPU hotplug | Mesin kita tidak menambah/melepas CPU saat berjalan. |
| Cpufreq, thermal, RAPL | Butuh hardware nyata dan kebijakan daya. Tidak ada di QEMU. |
| NUMA, IOMMU | Satu socket, satu node. IOMMU baru relevan bila ada passthrough. |
| vm86 | 16-bit mode. Tidak relevan di long mode. |
| vsyscall | Digantikan vDSO; hanya ada untuk kompatibilitas ABI lama. |
| AMX, PKU | Belum ada use case di kita. |
| `dump_pagetables.c` | `heap_watch.c` sudah mem-*walk* page table; yang kurang hanya presentasi. |

**Kesimpulan penting:** bila 12 baris di atas dikeluarkan dari `arch/x86/`
Linux, sisanya sekitar 100-150 file. Itu 6-9x kita - berbeda orde besaran,
bukan 83x. Dan selisih itu dijelaskan oleh item P1/P2 di Bagian 3, bukan oleh
sesuatu yang tak terjangkau.

---

## 6. Urutan yang disarankan

Ketergantungannya nyata, jadi urutannya bukan selera:

```
P0-1 TLB shootdown  ──> prasyarat COW dan thread
P0-2 frame pointer  ──> membuat P0-1 bisa didebug
P0-3 hapus dead code  (mandiri, sangat murah)

P1-1 feature framework ──┬──> P1-6 PAT
                         ├──> P1-7 TSC / TSC-deadline
                         ├──> P1-9 UMIP
                         └──> (x2APIC, PCID, XSAVE, MSI di P2)

P1-2 IOAPIC + MADT ──> MSI/MSI-X, IRQ affinity
P1-4 SYSCALL/SYSRET ──> P1-5 vDSO
P1-8 atomic.h ──> COW
P1-3 signal  (mandiri)
P1-10 WARN_ON/BUG_ON  (mandiri, murah)
P1-11 SSE/AVX ──> XSAVE wajib
         └──> 4.3 dulu: perbaiki 5 komentar basi + CR0
```

Tiga hal yang mandiri dan bisa dikerjakan lebih dulu tanpa menunggu apa pun:
**P0-2** (satu flag), **P0-3** (hapus dua baris), dan **P1-9** (satu bit CR4,
setelah P1-1 ada). Ketiganya kecil, dan P0-2 langsung memperbaiki kemampuan
mendiagnosa seluruh item lain.

Satu hal lagi yang mandiri dan murah: **4.3** - perbaiki lima komentar yang
menyatakan SSE selalu `#UD`. Tidak ada perubahan perilaku, tapi alasannya salah
dan akan menyesatkan siapa pun yang mengerjakan FASE 3 berikutnya.
*(Sudah dikerjakan bersama Phase 3 — lihat P1-11.)*

---

## 7. Cara mengukur kemajuan

Kelengkapan bukan jumlah file. Empat ukuran yang benar:

1. **Setiap item P0/P1 punya bukti boot-test.** Repo ini sudah punya polanya:
   17/17 subsistem OK, 0 FAIL, 0 PANIC. Setiap item di atas harus lulus
   kriteria yang sama sebelum dianggap selesai.
2. **Tidak ada fitur yang diaktifkan tanpa feature gating.** Setelah P1-1,
   setiap `CR4`/`EFER`/`XCR0` baru harus dicek lewat `cpu_has()`.
3. **Setiap utang tercatat di kode.** `paging.c:575` adalah contoh yang baik:
   keterbatasan ditulis di tempat pembacanya akan menemukannya. Utang yang tidak
   tercatat adalah bug yang menunggu waktu.
4. **Dokumen ini diperbarui saat status berubah.** Bila sebuah item dikerjakan
   atau ditolak, barisnya di Bagian 2 berubah - bukan dihapus.

---

## 8. Rujukan

- [Arch Hardening](../kernel/arch-hardening.md) - pertahanan yang sudah ada
- [Interrupts & Timers](../kernel/interrupts.md) - mekanika IDT/ISR/PIT/LAPIC
- [Memory Management](../kernel/memory.md) - PMM, VMM, heap
- [OSDev Portfolio Roadmap](../ROADMAP-OSDEV.md) - rencana tingkat proyek
