# FIX 005 - App berjalan di Ring 0 (pointer heap mentah lintas boundary)

> **Prioritas**: P1 - **epic** (perubahan arsitektur, pecah jadi tahapan)
> **Status**: ✅ **SELESAI — SEMUA 4 TAHAP** (2026-07-26). Tahap 4 (SMEP/SMAP/WP) terverifikasi: CR4=0x300020 di 4 vCPU, badptr 27 PASS, viewer + fileman jalan; desain: `DOCUMENTATION/design/ring3-tahap4-smap-smep.md`
> **Lokasi**: `kernel/syscall.c` (`sys_alloc`, `sys_free`, dispatch), `kernel/elf.c`, GDT/TSS & iretq frame user

## Masalah

App dieksekusi dalam konteks kernel (Ring 0, kernel AS): `sys_alloc()`
mengembalikan pointer `kmalloc()` mentah dan `sys_free()` menerima pointer
mentah. Stale/liar pointer app dapat menulis metadata kernel heap langsung -
satu bug app = korupsi kernel penuh. (Inilah kenapa bug seperti pencemaran PMM
kemarin berdampak sistemik.)

## Arah solusi (bertahap, jangan sekaligus)

1. **Tahap 1 - CPL3 sungguhan** ✅ **DONE (2026-07-26)**: segmen user di GDT,
   TSS per-CPU (RSP0 = syscall stack permanen per-CPU), iretq ke ring 3 di
   `sys_exec`; shell + chained apps satu jalur. Bug verifikasi: `hlt` userland
   (#GP), gate int 0x80 mematikan IF (freeze), jalur shell bypass ring 3.
2. **Tahap 2 - boundary copy** ✅ **DONE (2026-07-26)**: modul `kernel/usercopy.c`
   (validasi TRANSISIONAL "mapped di AS caller" — stack app & sys_alloc masih
   heap kernel, predikat diperketat di Tahap 3); semua syscall pointer
   dikonversi copy-in/copy-out; pengecualian shared: canvas KWM (31) +
   `sys_draw_image` (23); fix UAF `sys_load_elf`. Terverifikasi: `badptr.elf`
   27 PASS / 0 FAIL di ring 3, fileman GUI jalan.
   Detail: `DOCUMENTATION/design/ring3-tahap2-boundary-copy.md`.
3. **Tahap 3 - heap user per proses** ✅ **DONE (2026-07-26)**: modul
   `kernel/uheap.c` — `sys_alloc`/`sys_free`/`sys_realloc` ring 3 = region
   user range per AS (brk + guard page, metadata di heap kernel, halaman
   zeroed); `sys_free` memvalidasi kepemilikan; stack app pindah ke user range
   (`elf.c`); higher-half US=0 (heap flags 7 → 3); predikat usercopy final
   (user range saja). Terverifikasi: badptr 27 PASS / 0 FAIL, fileman GUI,
   viewer decode PNG. Detail: `DOCUMENTATION/design/ring3-tahap3-user-heap.md`.
4. **Tahap 4 - SMAP/SMEP + WP** ✅ **DONE (2026-07-26)**: CR4.SMEP+SMAP
   aktif per-core (BSP `kernel_main` + AP `smp_ap_main`, cek CPUID leaf 7),
   CR0.WP diverifikasi; semua deref user disengaja dibungkus jendela
   STAC/CLAC (`include/smap.h`): usercopy, uheap realloc, copy segmen ELF,
   blit canvas 31, draw_image 23. Terverifikasi QEMU: CR4=0x300020 di 4
   vCPU, badptr 27 PASS / 0 FAIL, viewer + fileman jalan.
   Detail: `DOCUMENTATION/design/ring3-tahap4-smap-smep.md`.


Tahap 1 - CPL3 sungguhan (app jalan di Ring 3)

    Bagian paling mekanis & paling berisiko (salah sedikit → triple fault):

     - GDT (arch/x86/gdt*): tambah segmen user code/data (mis. 0x20|3, 0x18|3)
     - TSS per-CPU + RSP0: begitu app ring 3, setiap interrupt (timer, keyboard, int 0x80) memaksa CPU pindah ke kernel
       stack via TSS. Pilihan desain: per-CPU syscall/interrupt stack permanen (mirip idle stack FIX_001 - kita sudah
       punya polanya), atau update `TSS.rsp0` di `schedule_on_cpu`. Rekomendasi: per-CPU stack permanen dulu, lebih
       sedikit moving part.
     - IDT gate `int 0x80`: DPL harus di-set ke 3 - kalau tidak, int 0x80 dari ring 3 malah #GP
     - iretq frame di sys_exec (syscall.c): r->cs/r->ss diisi segmen user (|3), RFLAGS IF=1 - sekarang hanya
       r->rip/r->rsp yang diubah
     - Flag halaman: user range tetap US=1; higher-half (heap, dsb.) tetap US=1 dulu di tahap ini supaya sys_alloc model
        lama tidak pecah - pemisahan privilege instruksi saja dulu (app tak bisa lagi cli, outb, baca/tulis page table,
       load ELF sendiri)

    Tahap 2 - Boundary copy (syscall tidak lagi memakan pointer kernel)

    Audit semua syscall yang menerima pointer (sys_draw_string, sys_open/vfs paths, kwm_update_window, sys_get_event
    out-pointer, dll.):

     - Validasi pointer: user range (PML4 idx < 256) + paging_is_mapped_into(ptr, caller->pml4_phys)
     - Copy-in ke buffer kernel sebelum dipakai, copy-out sesudahnya
     - Pengecualian terdokumentasi: canvas window (sampai 16MB/frame - copy per frame mahal; tetap shared tapi
       tervalidasi pemiliknya - sudah ada dari FIX_004)
     - Tulis kontrak syscall di DOCUMENTATION/ (mana yang copy, mana yang shared)

    Tahap 3 - Heap user per proses (isolasi heap penuh)

     - sys_alloc/sys_free jadi allocator region user range milik AS pemanggil (model brk/region), bukan kmalloc -
       pointer yang lewat boundary bukan lagi alamat heap kernel
     - Flip flag higher-half ke US=0 (heap expand sekarang kirim flags 7 → jadi 3): app ring 3 yang menyentuh
       0xFFFF9000... langsung #PF
     - Ini titik di mana bug app benar-benar tidak bisa menyentuh metadata kernel - isu sistemik tertutup

    Tahap 4 - SMAP/SMEP + verifikasi WP

     - CR4.SMEP (kernel tak bisa eksekusi user page), CR4.SMAP (kernel tak bisa baca/tulis user page tanpa STAC/CLAC) -
       boundary copy Tahap 2 disesuaikan
     - Verifikasi CR0.WP aktif
     - QEMU -cpu max mendukung keduanya

    Yang perlu diubah di sisi app

    Praktis tidak ada - userlib tetap int 0x80 dengan API sama. Yang berubah hanya perilaku kernel di baliknya
    (kecuali app yang bergantung pada detail sys_alloc - itu di Tahap 3).

    Risiko & urutan

## Catatan

- Setiap tahap harus tetap boot-able; jangan gabung dalam satu commit besar.
- Dokumentasikan kontrak syscall baru di `DOCUMENTATION/` saat Tahap 2 mulai.

## Verifikasi

- App jahat/uji (tulis ke pointer bebas) → `#PF` di app, kernel tetap hidup.
- Suite app existing (login, shell, fileman, viewer) jalan tanpa perubahan perilaku.
