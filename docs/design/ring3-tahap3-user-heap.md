# Desain: Ring 3 Tahap 3 — Heap User per Proses (FIX_005)

> **Status**: **SELESAI & TERVERIFIKASI** (2026-07-26) — boot `-smp 4` bersih,
> `badptr.elf` 27 PASS / 0 FAIL (ring 3), fileman GUI jalan, viewer decode
> `kyuzen.png` sukses (stbi = churn alloc/realloc/free terberat di uheap).
> **Scope**: `kernel/uheap.c` (BARU), `kernel/elf.c` (stack user range),
> `kernel/heap.c` (US=0), `kernel/usercopy.c` (predikat final),
> `kernel/paging.c` (`vmm_unmap_page_from`), `kernel/syscall.c` (9/10/19 +
> teardown), `kernel/sched/lifecycle.c`, `include/{uheap,task,elf,paging,usercopy}.h`.

## Tujuan

Titik penutup isu sistemik FIX_005: pointer yang lewat boundary syscall bukan
lagi alamat heap kernel. Bug app (pointer liar/stale) tidak bisa lagi
menyentuh metadata kernel — paling jauh #PF di app itu sendiri.

1. `sys_alloc`/`sys_free`/`sys_realloc` (9/10/19) dari **ring 3** dilayani
   allocator region user range milik AS caller (`kernel/uheap.c`), bukan
   `kmalloc`. Caller **ring 0** (shell/login/zen via int 0x80) tetap kmalloc.
2. Stack app pindah dari `kmalloc` ke user range AS
   (`[USER_STACK_TOP-256KB, USER_STACK_TOP)`, elf.c).
3. Higher-half heap kernel di-flip **US=0** (expand_heap flags 7 → 3) — app
   ring 3 yang menyentuh `0xFFFF9000...` langsung #PF.
4. Predikat `user_range_ok`/`strncpy_from_user` diperketat ke **user range
   saja** (`uaddr < 0x0000800000000000`, PML4 idx < 256) — pointer higher-half
   ditolak walau mapped di AS caller. Prasyaratnya (poin 1 & 2) kini terpenuhi.

## Layout user range per AS

| Range | Isi |
|---|---|
| `0x4000000` | ELF segments (kode/data/bss app) |
| `0x0BFC0000..0x0C000000` | Stack app 256KB (`USER_STACK_TOP`, elf.h) |
| `0x10000000..0x40000000` | Heap user (`UHEAP_BASE..UHEAP_END`, uheap.h) |

## Keputusan desain

### 1. Allocator region page-granular, metadata di heap KERNEL

`kernel/uheap.c`: model brk — `uheap_brk` per-task maju terus; tiap alokasi =
`ceil(size/4096)` halaman + **1 guard page tidak di-map** (overflow lintas
region → #PF, bukan korupsi diam-diam). Region dicatat sebagai linked list
`uheap_region_t {base, size, pages}` di **heap kernel** (`task_t.uheap_regions`)
— app tidak pernah bisa menyentuh metadata allocator-nya sendiri, jadi
`sys_free` liar paling jauh gagal validasi (diabaikan), tidak pernah merusak
kernel.

- `uheap_free` memvalidasi kepemilikan: hanya `base` region milik task caller
  yang diterima; unmap via `vmm_unmap_page_from` + frame kembali ke PMM.
- `uheap_realloc`: ukuran lama dilacak kernel — argumen `old_size` dari app
  **tidak dipercaya lagi**. Muat di kapasitas halaman → in-place; selain itu
  alloc-copy-free (copy deref alamat user langsung: CR3 = AS caller selama
  syscall, int 0x80 tidak mengganti CR3).
- Halaman baru **di-zero via HHDM** sebelum diberikan (frame bekas kernel
  tidak boleh bocor ke ring 3). Stack app di elf.c juga di-zero.
- **Tanpa lock**: state per-task, hanya task pemilik di dalam syscall-nya
  sendiri yang menyentuh field `uheap_*`.
- Batas: satu alokasi ≤ 64MB (= `UC_MAX_RANGE`); VA region tidak di-reuse
  (brk maju terus) — plafon 768MB VA per AS, reset setiap exec.

### 2. Lifecycle: metadata vs frame

Frame fisik region & stack dibebaskan oleh `vmm_destroy_address_space` saat
AS mati — `uheap_reset()` hanya membuang node metadata + reset brk, dipanggil
berdampingan dengan teardown AS di:

- syscall 25 (`sys_load_elf`) — sebelum AS baru dipasang;
- syscall 33 (`sys_exec`) — di blok destroy AS lama;
- syscall 34 (`sys_exit`) — sebelum destroy;
- `task_exit` (lifecycle.c) — di dalam `scheduler_lock` SEBELUM slot terlihat
  DEAD (reaper `create_task` tidak bisa double-free; pola yang sama dengan
  zero-ownership `stack_base` FIX_001) + fallback reset di slot reuse.

`task_t.user_stack_base`/`deferred_user_stack_base` dan
`reclaim_deferred_user_stack()` DIHAPUS — stack tidak lagi kmalloc, tracking
kfree tidak diperlukan. Signature `elf_load_file` kehilangan `out_stack_base`;
minta stack (`out_stack_top != NULL`) kini wajib `target_pml4 != PHYS_NULL`.

### 3. Pengecualian shared Tahap 2 TETAP kompatibel

Canvas KWM (31) & `sys_draw_image` (23): `kwm_update_window` menyalin buffer
app → canvas kernel secara **sinkron di dalam syscall** (`rep movsl`, CR3 =
AS caller), dan `draw_image` membaca langsung di konteks yang sama. Compositor
hanya membaca canvas kernel — pointer user per-AS tidak pernah menyeberang
konteks. Tidak ada perubahan di kwm/compositor.

### 4. `vmm_unmap_page_from` (paging.c)

Unmap satu halaman dari PML4 target spesifik, return phys addr agar caller
bisa `pmm_free_page`. invlpg hanya CPU ini — batasan yang sama dengan
`vmm_tlb_shootdown` existing (TLB CPU lain: TODO IPI, pre-existing).

## Kontrak syscall 9/10/19 (final)

| # | Ring 3 | Ring 0 |
|---|---|---|
| 9 sys_alloc | `uheap_alloc` → pointer user range, halaman zeroed | `kmalloc` |
| 10 sys_free | `uheap_free` — validasi kepemilikan, pointer asing diabaikan | `kfree` |
| 19 sys_realloc | `uheap_realloc` — old_size app diabaikan (dilacak kernel) | `krealloc` |

Sisi app: **tidak ada perubahan API** — userlib tetap int 0x80 yang sama.

## Verifikasi (QEMU `-smp 4`, headless + screendump)

1. `make` + `make boot_image.iso` bersih. ✓
2. Boot → login `root` → shell (jalur ring 0 bypass hidup). ✓
3. `badptr.elf` ring 3: **27 PASS / 0 FAIL**, exit rapi ke shell. ✓
4. `fileman` GUI: window render (canvas uheap → copy 31), file list
   (copy-out 24), event loop (29). ✓
5. `viewer` → decode `kyuzen.png` via stb_image: ratusan alloc/realloc/free
   di uheap + buffer pixel besar — gambar tampil benar, kernel hidup. ✓
6. Higher-half `#PF` dari ring 3: tercakup oleh US=0 (leaf PTE heap tidak
   punya bit US); predikat usercopy menolak sebelum deref. ✓ (by construction)

## Batasan diketahui

- VA heap user tidak di-reuse (brk maju terus): app yang alloc/free terus-
  menerus dalam SATU sesi bisa menghabiskan 768MB VA — frame fisiknya bebas,
  hanya VA yang bocor; reset total setiap exec. Cukup untuk profil app saat
  ini; free-list VA bisa ditambah nanti tanpa mengubah API.
- TLB CPU lain tidak di-shootdown saat `uheap_free` (pre-existing, sama
  dengan semua unmap lain). Ditutup saat IPI shootdown masuk.
- Syscall 25 (`sys_load_elf`) mengganti AS caller tanpa mengatur RIP/RSP —
  semantik legacy pra-Tahap 1; jalur launch nyata adalah syscall 33.
  Dibiarkan kompilasi & berperilaku sama (stack ter-map ikut bebas bersama AS).

## Lanjut: Tahap 4

SMAP/SMEP + verifikasi CR0.WP — semua deref user terkonsentrasi di
`usercopy.c` + `uheap_realloc` + dua pengecualian shared (23/31), jadi
STAC/CLAC hanya perlu di titik-titik itu.
