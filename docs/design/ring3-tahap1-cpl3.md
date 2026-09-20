# Desain: Ring 3 Tahap 1 — CPL3 untuk ELF Apps (FIX_005)

> **Status**: **SELESAI & TERVERIFIKASI** (2026-07-26) — app jalan di CPL 3,
> 0 panic, jalur PNG end-to-end sukses di `-smp 4`
> **Scope**: Hanya ELF apps (`user_apps/*.elf`). App kernel-side
> (`apps/login.c`, `apps/shell.c`, `apps/zen.c`) tetap Ring 0.

## Tujuan

App ELF berjalan di **CPL=3**. Syscall tetap `int 0x80` (API userlib tidak
berubah). Isolasi memori penuh (US=0 untuk higher-half) BUKAN scope tahap ini —
itu Tahap 3. Tahap 1 memisahkan **privilege instruksi**: app tidak bisa lagi
`cli`/`hlt`/`outb`/utak-atik page table; satu-satunya jalan masuk kernel adalah
`int 0x80`.

## Yang sudah ada (tidak diubah)

| Komponen | Kondisi |
|----------|---------|
| GDT[3] user code `0x18`, GDT[4] user data `0x20` | Sudah DPL=3 |
| IDT gate 128 (`int 0x80`) | Sudah `0xEE` (trap, DPL=3) |
| AS per-proses + CR3 switch saat exec | Kernel ter-clone di higher-half |
| `registers_t` memuat `cs`, `ss` | Frame iretq bisa diedit dari C |

## Keputusan desain

### 1. TSS per-CPU + syscall stack permanen per-CPU

- `tss_entries[SMP_MAX_CPUS]`, deskriptor di GDT index `5 + 2*cpu`
  (selector `(5 + 2*cpu) << 3`). GDT tumbuh 7 → 37 entri.
- `rsp0` = **syscall stack permanen per-CPU** (16 KB, kmalloc di
  `tasking_init`, tidak pernah di-free — pola sama seperti idle stack FIX_001).
- **Kenapa statis (bukan update RSP0 per context switch)**: hanya task 0 yang
  menjalankan app ring-3 (model exec longjmp). Saat app mid-syscall lalu
  di-preempt, frame-nya membeku di syscall stack dan tidak disentuh siapa pun
  sampai task 0 resume — kernel task lain ring-0 dan tidak memakai RSP0.
  Aman tanpa update TSS di scheduler.
- `ltr` wajib **per-CPU**: BSP di `init_gdt` (selector 0x28), AP di
  `smp_ap_main` via `tss_flush_sel((5 + 2*cpu) << 3)` yang baru.

### 2. Masuk ring 3 (sys_exec, syscall 33)

Handler menambahkan pada frame yang sudah ada:

```c
r->rip = entry;
r->rsp = new_stack_top;
r->cs  = 0x1B;   // user code | RPL3   ← BARU
r->ss  = 0x23;   // user data | RPL3   ← BARU
```

`iretq` di `isr128_stub` menurunkan CPL ke 3. Stack app adalah heap pointer
(US=1) — sah untuk Tahap 1.

### 3. Kembali ke shell (sys_exit, syscall 34)

Longjmp ke `user_shell` (kode Ring 0) wajib mengembalikan segmen kernel:

```c
r->rip = (uint64_t)user_shell;
r->rsp = g_shell_return_rsp;
r->cs  = 0x08;   // kernel code      ← BARU
r->ss  = 0x10;   // kernel data      ← BARU
```

### 4. Interrupt/exception dari ring 3

CPU otomatis: ganti ke `RSP0` (syscall stack CPU ini) → push SS/RSP user →
ISR jalan normal → `iretq` balik ke CPL 3. Semua ISR existing tidak berubah.

## Risiko & mitigasi

| Risiko | Mitigasi |
|--------|----------|
| `rsp0=0` → triple fault | RSP0 diisi SEBELUM exec pertama (tasking_init jauh sebelum shell) |
| AP tanpa `ltr` → #TS saat interrupt ring-3 di AP | `tss_flush_sel` di `smp_ap_main` |
| Syscall stack overflow (rantai exec dalam) | 16 KB (2× task stack) + exec chain dangkal |
| App ring-3 mengakses memori kernel | DITERIMA tahap ini (higher-half masih US=1); ditutup Tahap 3 |

## Verifikasi

1. Boot normal, login, shell jalan (semua Ring 0). ✓
2. `fileman` jalan → app di CPL 3 (serial log exec: `cs=0x1B`). ✓
3. Interrupt timer/keyboard/mouse saat app ring-3 → sistem stabil (bukan triple fault). ✓
4. Jalur PNG end-to-end; exit kembali ke shell (CPL 0 lagi). ✓
5. Build `HEAP_WATCH_DEBUG`: 0 panic, 0 corruption. ✓

## Temuan saat implementasi (bug yang diperbaiki)

1. **`hlt` di userland** — `apps/userlib.c` `sys_yield()` mengeksekusi `hlt`
   langsung: legal di CPL 0, `#GP` di CPL 3 → BOSD di SEMUA app. Fix: `hlt`
   pindah ke sisi kernel syscall 4.
2. **Gate `int 0x80` = interrupt gate, bukan trap** — `0xEE` mematikan IF saat
   entry (komentar lama di `idt.c` salah label). `hlt` kernel tanpa `sti` →
   CPU tidur selamanya (freeze, timer ikut mati). Fix: `sti; hlt`.
3. **Jalur shell bypass ring 3** — shell meluncurkan app via `sys_load_elf` +
   CALL langsung di CPL 0. Fix: shell memakai `sys_exec` (33) → semua app
   (shell-launch maupun chained) masuk CPL 3 lewat satu jalur.

## Perubahan akhir

| File | Isi |
|------|-----|
| `arch/x86/gdt.c` | TSS per-CPU (deskriptor GDT `5+2*cpu`, 37 entri), `tss_set_rsp0`, `tss_load_cpu` |
| `arch/x86/gdt_flush.asm` | `tss_flush_sel(selector)` |
| `kernel/task.c` | Syscall stack permanen 16 KB × `SMP_MAX_CPUS` di `tasking_init`; RSP0 BSP |
| `kernel/kernel.c` | `smp_ap_main`: `tss_set_rsp0` + `tss_load_cpu` per AP |
| `kernel/syscall.c` | `sys_exec`: frame `cs=0x1B, ss=0x23`; syscall 4: `sti; hlt` di kernel |
| `apps/userlib.c` | `sys_yield` tanpa `hlt` userland |
| `apps/kernel_userlib.c` | wrapper `sys_exec` (33) untuk sisi kernel |
| `apps/shell.c` | launch app via `sys_exec` (bukan CALL langsung) |
