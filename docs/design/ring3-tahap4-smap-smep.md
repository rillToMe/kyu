# Desain: Ring 3 Tahap 4 — SMEP/SMAP + Verifikasi WP (FIX_005, penutup)

> **Status**: **SELESAI & TERVERIFIKASI** (2026-07-26) — CR4=0x300020 di
> keempat vCPU (SMEP+SMAP on), CR0.WP on; badptr 27 PASS / 0 FAIL, viewer
> decode PNG, fileman GUI — semua dengan SMAP hidup.
> **Scope**: `include/smap.h` (BARU), `kernel/cpu.c`, `kernel/kernel.c`,
> `kernel/smp/smp.c`, dan jendela STAC/CLAC di `usercopy.c`, `uheap.c`,
> `elf.c`, `gfx/kwm.c`, `syscall.c`.

## Tujuan

Lapisan pertahanan terakhir FIX_005: setelah Tahap 1–3 memisahkan privilege,
boundary, dan heap, Tahap 4 membuat KERNEL-nya sendiri tidak bisa menyentuh
memori user secara tidak sengaja:

- **CR4.SMEP** (bit 20): kernel tidak bisa mengeksekusi halaman US=1 —
  pointer fungsi kernel yang dibelokkan ke shellcode user langsung #PF.
- **CR4.SMAP** (bit 21): kernel tidak bisa baca/tulis halaman US=1 kecuali
  EFLAGS.AC di-set — deref pointer user yang lolos tanpa validasi (bug
  kernel) langsung #PF, bukan diam-diam dieksekusi.
- **CR0.WP** (bit 16): kernel tidak menembus halaman read-only.

## Implementasi

### 1. Aktivasi per-core (`cpu.c`, `smap.h`)

`cpu_enable_smap_smep()`: cek CPUID leaf 7 subleaf 0 (EBX bit 7 = SMEP, bit
20 = SMAP), set bit CR4 yang didukung. `cpu_verify_wp()`: pastikan CR0.WP,
set bila belum. CR4/CR0 **per-core** — dipanggil di:

- BSP: `kernel_main` setelah `init_heap` (+ log boot `[CPU] SMEP=.. SMAP=.. WP=..`);
- Tiap AP: `smp_ap_main` (sebelum online).

`g_smap_enabled` (global, ditulis nilai identik oleh semua core) menjaga
helper: CPU tanpa SMAP tidak pernah mengeksekusi stac/clac (#UD).

### 2. Jendela akses user (`smap.h`)

```c
user_access_begin();   // stac — buka jendela (encode .byte, bebas gating asm)
...deref halaman user yang disengaja...
user_access_end();     // clac — tutup
```

Semua deref user yang disengaja sudah terkonsentrasi (by design Tahap 2/3):

| Titik | File | Catatan |
|---|---|---|
| `copy_from_user` / `copy_to_user` | `usercopy.c` | jendela hanya saat `ctx->from_user` |
| `strncpy_from_user` (loop ring 3) | `usercopy.c` | satu jendela untuk seluruh loop; early-return diubah jadi `fail+break` agar clac selalu jalan |
| copy realloc (kedua sisi user) | `uheap.c` | CR3 = AS caller |
| copy segmen ELF + zero BSS | `elf.c` | halaman tujuan US=1 (ELF64 & ELF32) |
| blit canvas (`rep movsl`) | `gfx/kwm.c` | pengecualian shared #1 (syscall 31) |
| `draw_image` call-site (syscall 23) | `syscall.c` | pengecualian shared #2 — jendela selebar blit |

Zeroing halaman uheap/stack via **HHDM** (US=0) — tidak butuh jendela.

### 3. SMEP: audit jalur eksekusi

Kernel tidak pernah lompat ke halaman user: launch app = `sys_exec` (33)
iretq ke CPL 3; jalur direct-launch ring 0 lama (`sys_load_elf` kernel_userlib
→ jump entry) hanya tersisa di kode ter-komentar `shell.c`. `sys_exit`
longjmp menuju `user_shell` (kode kernel higher-half). Aman tanpa perubahan.

## Batasan diketahui

- **Entry ISR tidak meng-clac**: interrupt yang datang di tengah jendela
  (mis. preemption saat blit canvas 16MB) mewarisi AC=1 sampai iretq —
  pengurangan proteksi sesaat, bukan bug correctness (RFLAGS per-task
  disimpan/dipulihkan oleh ISR frame). Linux menutup ini dengan CLAC di
  semua entry stub — bisa ditiru nanti bila diperlukan.
- Jendela syscall 23/31 selebar blit (≤ 16–64MB) — konsekuensi pengecualian
  shared yang sudah terdokumentasi sejak Tahap 2.

## Verifikasi (QEMU `-cpu max -smp 4`)

1. Build kernel + ISO bersih. ✓
2. Monitor QEMU `info registers -a`: **CR4=0x00300020 (SMEP+SMAP) dan
   CR0=0x80010011 (WP) di keempat vCPU**. ✓
3. `badptr.elf`: 27 PASS / 0 FAIL — jalur copy tervalidasi tetap hidup di
   dalam jendela SMAP (tanpa stac, tiap copy_to_user pasti #PF). ✓
4. `viewer` decode `kyuzen.png` (uheap churn + copy-out file 8MB-cap +
   canvas blit) → gambar tampil, Back + close rapi. ✓
5. `fileman` setelah viewer exit: window, file list, event loop hidup;
   kernel sehat di uptime 3m12s. ✓

## FIX_005 selesai

Keempat tahap rampung: CPL3 (1) → boundary copy (2) → heap user per proses +
US=0 (3) → SMEP/SMAP/WP (4). Bug app kini terkurung penuh di AS-nya;
kesalahan deref di kernel-nya sendiri pun berubah dari korupsi diam-diam
menjadi #PF yang terlihat.
