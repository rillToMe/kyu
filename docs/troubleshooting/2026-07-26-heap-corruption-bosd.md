# Post-mortem: BOSD "heap_block_t magic mismatch" — Halaman Heap Terpetakan ke ROM BIOS

> **Tanggal insiden**: 25–26 Juli 2026
> **Branch**: `feature/64bit-migration`
> **Commit fix**: `7f89077` — *FIX: Pseudo heap corruption - heap page mapped to BIOS ROM*
> **Status**: **TERPECAHKAN** — terverifikasi di QEMU (`-smp 4` + NIC e1000)
> **Artefak debugging**: folder [`Resolve/26-07-26/`](Resolve/(2026-07-26) - heap-corruption/) (log serial, skrip, disassembly)

---

## TL;DR

Tidak ada heap corruptor. Tidak ada race SMP. Tidak ada DMA liar.

Halaman fisik di belakang heap virtual `0xFFFF9000002B3000` adalah **ROM SeaBIOS**
(`phys 0xF8000`), yang diberikan oleh **PMM sendiri** karena free list-nya tercemar.
Store `magic = 0xDEADC0DE` ke ROM menguap; load mengembalikan byte BIOS — persis
menyerupai "header heap ditimpa penulis misterius".

Pencemaran free list: cabang `else { vmm_unmap_user_space(); }` di syscall 33
(exec) dan 34 (exit) menjalankan `vmm_destroy_address_space()` pada **boot/kernel
PML4**, membebaskan mapping user-range milik **Limine** (halaman fisik < 72 MB,
reserved) ke free list PMM.

---

## 1. Gejala

Setelah boot, jalur berikut memicu BOSD:

1. Login.
2. Jalankan `fileman`.
3. Pilih `kyuzen.png`.
4. Klik `BUKA`.

Panic yang konsisten pada build `HEAP_WATCH_DEBUG`:

```text
REASON: HEAP CORRUPTION
DETAILS: heap_block_t magic mismatch - header corrupt
CODE: 0xFFFF9000002B3A50
```

Blok yang "korup" selalu remainder hasil split untuk buffer PNG
(request `0x2CA30` = 182.832 byte = ukuran `kyuzen.png`), dengan metadata:

```text
block   = 0xFFFF9000002B3A50
size    = 0xA026C68966F1CDA0
free    = 0xCC
next    = 0x75E866C0B60F6623
magic   = 0x00DA80BB        (seharusnya 0xDEADC0DE)
```

## 2. Mengapa kasus ini menyesatkan

Empat sifatnya mengarahkan investigasi ke "corruptor aktif":

1. **Garbage byte-identik di dua build berbeda dan dua run berbeda** — padahal
   dua patch (scheduler ordering + deferred stack) sudah masuk ISO.
2. **Read-back langsung garbage** — log `[AFTER SPLIT]` tepat setelah `kmalloc`
   menulis header menunjukkan nilai garbage, di CPU yang sama, IRQ off,
   `heap_lock` dipegang.
3. Semua field header (magic, size, free, next) berubah — pola "timpa multi-byte".
4. Crash selalu di jalur PNG — mudah disalahkan pada STB/KFS/viewer.

Penjelasan sebenarnya untuk semua sifat itu: **isi ROM bersifat tetap**. Store ke
ROM diabaikan hardware; load mengembalikan byte SeaBIOS yang sama setiap boot,
di build mana pun. Byte "garbage" itu (`66 0F B6 C0` = `movzx eax,al` ber-prefix,
`89 C6` = `mov esi,eax`) memang potongan kode BIOS.

## 3. Hipotesis yang gugur (dan bukti penggugurnya)

| # | Hipotesis | Bukti penggugur |
|---|-----------|-----------------|
| 1 | STB image / KFS menimpa buffer | Header sudah "rusak" **sebelum** buffer PNG dibaca (log `[AFTER SPLIT]`) |
| 2 | Buffer overflow alokasi tetangga | Prev block `0x287000` berakhir tepat di `0x2B3A50`; dan store→load garbage dalam ~50 cycle |
| 3 | Race allocator (`heap_lock` bocor) | `kmalloc` memegang `heap_lock` + IRQ off saat split |
| 4 | Corruptor dari CPU lain (SMP) | **Watchpoint DR0 di semua CPU hanya menangkap `kmalloc` sendiri** — nol hit lain |
| 5 | DMA e1000 | DMA tak mungkin sinkron dengan window store→load di dua build berbeda; ring e1000 dari PMM+HHDM (benar) |
| 6 | Stack viewer menimpa header | Rentang stack `0xC9040+0x40000` berakhir di `0x109040` — tidak mencakup target |
| 7 | Double-run task (scheduler bug) | Patch ordering scheduler diterapkan → crash **tetap** terjadi identik |

Catatan: hipotesis #7 dan stack UAF (P0 `task_exit()`) adalah **bug nyata** dan
patch-nya tetap berharga — hanya saja bukan penyebab BOSD ini.

## 4. Bukti kunci (kronologi)

### 4.1 Disassembly `kmalloc` — store & load di CPU yang sama

```text
ffffffff800034e8:  movl $0xdeadc0de, 0x20(%r14,%rbx)   ; new_block->magic = MAGIC
ffffffff800034f1:  movb $0x1, 0x30(%r14,%rbx)          ; ← RIP watchpoint
...
ffffffff8000354c:  movq 0x28(%r14,%rbx), %r12          ; load kembali size → garbage
```

Store→load alamat sama, CPU sama, IRQ off → secara arsitektur load **harus**
melihat hasil store. Kecuali halaman fisiknya bukan RAM.

### 4.2 Watchpoint DR0 per-CPU — satu-satunya penulis adalah allocator

Debug register x86 bersifat **per-CPU** — dipasang di BSP (`kernel_main`) dan
setiap AP (`smp_ap_main`), mengawasi 4 byte field magic di `0xFFFF9000002B3A50`.
Handler `#DB` bersifat *log-and-continue* (tidak freeze), mencetak CPU/task/RIP/
nilai + hasil page-walk.

Satu-satunya hit di seluruh run:

```text
[W0] n=0 cpu=0 task=0 rip=0xFFFFFFFF800034F1 val=0x0000000000DA80BB  <== SUSPECT
     cr3=0x0000000000001000 rsp=0xFFFF900000108CF0
     pte=0x00000000000F8067 pa=0x00000000000F8A50 kpml4=0x000000001FF7E000
```

- `rip` = instruksi tepat setelah store magic milik `kmalloc` → **yang menulis
  hanyalah allocator itu sendiri**. Kalau ada corruptor CPU lain, DR0-nya pasti
  menyala — nol hit.
- `val` = `0xDA80BB` — dibaca **langsung setelah store `0xDEADC0DE`** → store
  tidak pernah mendarat.
- `pa = 0xF8A50` — alamat fisik target = **area ROM BIOS (0xF0000–0xFFFFF)**.
- `cr3 = 0x1000` — app address space hasil daur ulang halaman low (lihat §5).

### 4.3 Jejak map-time — PMM sendiri yang memberi halaman ROM

Instrumentasi `vmm_alloc_page_kernel` (dipakai `expand_heap`) mencatat:

```text
[VMM] kmap va=0xFFFF9000002B3000 pa=0x00000000000F8000
```

Dan 233 mapping heap ber-backing halaman rendah dimulai dari:

```text
[VMM] kmap va=0xFFFF9000001CB000 pa=0x000000000000F000
[VMM] kmap va=0xFFFF9000001CC000 pa=0x0000000000010000
... (berurutan naik, pola hint allocator)
```

Titik transisi (pa tinggi → pa rendah) terjadi **tepat setelah app pertama
exit** — momen ketika cabang `else vmm_unmap_user_space()` pertama kali berjalan.

## 5. Root cause — rantai lengkap

```text
sys_exit (34) men-reset pml4_phys = 0
        │
        ▼
app berikutnya exec (33) dengan pml4_phys == 0
        │
        ▼
else { vmm_unmap_user_space(); }                ← kernel/syscall.c
        │  = vmm_destroy_address_space(BOOT PML4, free_pml4=0)
        ▼
membebaskan mapping user-range [0..255] milik LIMINE di boot PML4
(pmm_owns_page menjawab "used" untuk halaman reserved → ikut ter-free)
        │
        ▼
free list PMM tercemar halaman fisik < 72 MB (reserved + ROM 0xF0000–0xFFFFF)
        │
        ├──► vmm_create_address_space → PML4 app di phys 0x1000  (CR3=0x1000!)
        ├──► user pages app        → sebagian ROM-backed → crash acak (BOSD bervariasi)
        └──► expand_heap va 0x2B3000 → phys 0xF8000 (ROM SeaBIOS)
                        │
                        ▼
        split kmalloc: store magic=0xDEADC0DE → menguap di ROM
        load kembali → byte BIOS (0xDA80BB, 0xA026C68966F1CDA0, ...)
                        │
                        ▼
        panic "heap_block_t magic mismatch" di 0xFFFF9000002B3A50
```

Alamat-alamatnya deterministik karena heap base virtual tetap
(`0xFFFF900000000000`), urutan alokasi boot sama, dan isi ROM tetap — sehingga
seluruh "politik" crash tampak konsisten padahal bukan disebabkan penulis aktif.

## 6. Fix (commit `7f89077`)

| File | Perubahan |
|------|-----------|
| [`kernel/syscall.c`](../../kernel/syscall.c) | Hapus cabang `else { vmm_unmap_user_space(); }` di syscall 33 & 34. `pml4_phys == 0` berarti task memakai boot/kernel AS — tidak ada yang boleh di-unmap |
| [`kernel/paging.c`](../../kernel/paging.c), [`include/paging.h`](../../include/paging.h) | Hapus fungsi landmine `vmm_unmap_user_space` (tidak ada caller sah) |
| [`kernel/pmm.c`](../../kernel/pmm.c) | Defense in depth: `pmm_free_page` **menolak** free halaman < `0x4800000` (72 MB — zona reserved permanen: kernel, Limine, BIOS/ROM, framebuffer) dengan warning `[PMM] free low-reserved (BUG, ditolak)` |

Catatan: `vmm_destroy_task_as(self->pml4_phys)` untuk app AS tetap sah — user
range app AS hanya berisi halaman PMM yang legit.

## 7. Verifikasi

Build `HEAP_WATCH_DEBUG` + QEMU `-smp 4 -nic user,model=e1000`, sesi ~25 detik
meliputi jalur exec/exit berulang dan 6 alokasi PNG (`0x2CA30`):

| Metrik | Sebelum fix | Sesudah fix |
|--------|-------------|-------------|
| Waktu sampai crash | < 10 detik | tidak crash (sesi penuh) |
| Mapping heap ber-backing < 72 MB | 233 | **0** (dari 3173 total) |
| Panic `HEAP CORRUPTION` | 1 (fatal) | **0** |
| Warning `free low-reserved` | — | **0** |
| Alokasi PNG sukses | 0 | **6** |

## 8. Teknik debugging yang bisa dipakai ulang

Semua artefak ada di [`Resolve/26-07-26/`](Resolve/(2026-07-26) - heap-corruption/). Untuk masalah serupa
("memori berubah sendiri"):

1. **Build instrumentasi**: `make heap-watch` (atau `make boot_image.iso
   CFLAGS="<flags Makefile> -g -DHEAP_WATCH_DEBUG"` agar QEMU tidak auto-run).
2. **Serial log**: jalankan QEMU dengan `-serial file:<log>.log`. Semua jejak
   `[heap]`, `[VMM]`, `[W0]` terekam di situ.
3. **Watchpoint per-CPU** (`kernel/heap_watch.c`): set `heap_watch_target_addr`
   ke alamat header korup dari panic terakhir, rebuild. DR0–DR7 bersifat
   **per-CPU** — wajib dipasang di BSP *dan* setiap AP (`smp_ap_main`).
4. **Handler log-and-continue**: jangan freeze di hit pertama. Log
   CPU/task/RIP/nilai, lalu return — `exception_handler` harus `return` untuk
   `int_num == 1` agar stub `iretq` melanjutkan eksekusi. Penulis sah allocator
   (nilai == `HEAP_MAGIC`) dan corruptor langsung terpisah di log.
5. **Page-table walk di handler**: cetak `pte` + `pa` target. Inilah yang
   membuktikan halaman ROM — tanpa ini kita masih menebak-nebak "penulis".
6. **Resolve RIP → fungsi**: `llvm-nm build/myos.bin | sort` (myos.bin adalah ELF64)
   lalu cari simbol terdekat ≤ RIP. Disassembly: `llvm-objdump -d
   --start-address=... --stop-address=... build/myos.bin`.
7. **Matriks eliminasi**: `-smp 1` vs `-smp 4` (race SMP?), tanpa `-nic`
   (DMA?), dua build berbeda (garbage identik → data tetap → curigai ROM/mapping
   ketimbang penulis).

## 9. Isu laten yang tersisa (bukan penyebab BOSD ini)

Ditemukan selama investigasi; masih terbuka, dicatat agar tidak hilang:

- **P0 — `task_exit()` idle loop di stack task DEAD** (`kernel/task.c`): CPU
  tetap berjalan di stack task yang sudah `TASK_DEAD`; slot reuse dapat
  `kfree(stack_base)` stack yang masih dipakai. Butuh per-CPU idle stack atau
  deferred reclamation dengan jaminan CPU sudah pindah stack.
- **P1 — global mutable `vmm_user_pml4`** (`kernel/paging.c`, `kernel/elf.c`):
  dua CPU ELF-load bersamaan dapat memetakan page ke PML4 yang salah. Solusi:
  pass target PML4 eksplisit (`vmm_alloc_page_into`) atau lock seluruh load.
- **P1 — `current_as_cookie` global** (`kernel/syscall.c`): syscall 45 membaca
  global, race di SMP.
- **P1 — ownership `kwm_windows` global**: `sys_exec`/`sys_exit` memanggil
  `kwm_destroy_all_windows()` — dapat menghancurkan window task lain; pointer
  canvas tidak selalu di-NULL-kan setelah free.
- **P1 — app berjalan di Ring 0**: `sys_alloc` mengembalikan pointer `kmalloc`
  mentah; CPL3 belum diterapkan.

## 10. Referensi

| Artefak | Lokasi |
|---------|--------|
| Log pre-fix (build lama & patched, garbage identik) | `Resolve/26-07-26/heap-watch.log`, `Resolve/26-07-26/heap-watch-new.log` |
| Log watchpoint (hit `[W0]` pertama) | `Resolve/26-07-26/heap-watch-v3.log` |
| Log PTE walk + kmap phys (bukti ROM) | `Resolve/26-07-26/heap-watch-v4.log` |
| Log post-fix (bersih) | `Resolve/26-07-26/heap-watch-v5.log` |
| Disassembly & simbol (`kmalloc` store→load) | `Resolve/26-07-26/myos-disasm.txt`, `Resolve/26-07-26/nm-symbols.txt` |
| Skrip resolve return-address → simbol | `Resolve/26-07-26/resolve-ra.ps1` |
| Instrumentasi watchpoint | [`kernel/heap_watch.c`](../../kernel/heap_watch.c), [`include/heap_watch.h`](../../include/heap_watch.h) |
| Ringkasan investigasi awal (handoff lama) | `Resolve/26-07-26/problem.txt` |
