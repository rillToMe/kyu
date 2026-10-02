# Desain: Migrasi login/shell/zen ke Ring 3 (Fase 3)

> **Catatan (pasca-migrasi CMake).** Dokumen ini ditulis saat proyek masih
> dibangun dengan Makefile, jadi perintah `make ...` di dalamnya merujuk build
> lama. Makefile sudah dihapus; padanan CMake-nya ada di
> [Building](../development/building.md). Hasil verifikasi yang tercatat di sini
> sengaja tidak diubah — itu catatan apa yang benar-benar dijalankan saat itu.


> **Status**: `SELESAI` — Fase 1-4 selesai & terverifikasi (2026-XX).
> **Prasyarat**: FIX_005 Tahap 1-4 (SELESAI).
> **Terkait**: `fix/FIX.md` K-1 (FIXED), PM-2 (FIXED), UP-18.
>
> Kernel kini **nol kode user**: `system/` (init/login/shell/zen) seluruhnya ELF
> ring-3, `libs/core/kernel_userlib.c` dihapus, longjmp exec-chain dihapus.

## Masalah

`kernel/kernel.c` memanggil `user_login()` **langsung di CPL 0**. Konsekuensinya:

| Efek | Bukti |
|---|---|
| Validasi pointer user di-bypass total | `kernel/proc/usercopy.c`: `if (!ctx->from_user) return 1;` — `from_user` = `(r->cs & 3) == 3` = 0 |
| `sys_alloc` memberi heap **kernel**, bukan uheap | `kernel/syscall/sys_mem.c` dispatch berdasarkan `uc->from_user` |
| login/shell/zen punya privilege penuh | `cli`/`hlt`/`outb`/page table semua terjangkau |

README mengklaim *"memory-protected Ring-3 applications"*. Klaim itu **benar**
untuk app hasil `sys_spawn`/`sys_exec` (yang memalsukan frame `CS=0x1B/SS=0x23`),
tetapi **salah** untuk shell/login — jalur yang justru paling banyak dipakai.

## Jebakan: menambal `switch_to_user_mode()` TIDAK cukup

Menambahkan `iretq` ke `CS=0x1B` di titik hand-off **akan langsung `#PF`**:

- `kernel/mm/heap.c:139` — `vmm_alloc_page_kernel(current_heap_end, 3)`, komentar:
  *"US=0 (flags 3, dulu 7) — heap kernel tidak lagi terlihat ring 3"*
- `kernel/proc/elf.c:145` — *"kmalloc higher-half kini US=0, ring 3 yang push ke
  sana langsung #PF"*

**Kode yang di-link ke kernel tidak bisa "dinaikkan" ke Ring 3.** Ia harus
**dipindah keluar** menjadi ELF terpisah, lalu dijalankan sebagai task ring-3.

## Pola yang sudah terbukti

Migrasi ini bukan wilayah baru — tiga preseden sudah ada di repo:

| Preseden | Bukti |
|---|---|
| `system/cat.c` + `system/echo.c` | Sudah `filter-out` dari `SRC_DIRS`, dibangun sebagai ELF (`apps/Makefile` `SYS_APPS`) |
| `system/zen.c` | **Fase 2 dokumen ini** — sekarang ELF ring-3, di-spawn `cmd_zen` |
| `apps/terminal.c` | Sudah menjalankan engine `system/shell_core.c` **di Ring 3** (`apps/Makefile:293` `TERMINAL_LIBS` memuat `SHELLCORE_OBJ`) |

`include/shell.h` hanya butuh `<stdint.h>` + `shell_io_t` (callback) — API-nya
sudah bersih dari tipe kernel. Engine shell **terbukti portable**.

## Rencana

### Langkah 1 — `system/login.c` + `system/shell.c` jadi ELF

Ikuti pola `zen.c` (Fase 2):
- `filter-out` dari `SRC_DIRS`
- Tambahkan ke `SYS_APPS` di `apps/Makefile` (rule link generik sudah ada)
- `login.c`: `user_login()` → `main()`; `user_shell()` → `sys_spawn("/apps/shell.elf")`
- `shell.c`: `user_shell()` → `main()`; buang `extern fs_node_t tty_node` +
  `read_fs` (dua-duanya simbol kernel — ganti `console_poll_input` dengan API
  userlib)

### Langkah 2 — `init.elf` sebagai supervisor

Kernel **tidak** memanggil login langsung. Sebagai gantinya:

```c
// kernel/kernel.c
static void boot_handoff_to_init(void) {
    uint64_t stack_top = 0;
    phys_addr_t pml4 = vmm_create_address_space();
    vmm_switch_pml4(pml4);
    uint64_t entry = elf_load_file("/apps/init.elf", &stack_top, pml4);
    if (entry == 0) panic_boot("init.elf tidak bisa dimuat");
    int tid = create_user_task(entry, stack_top, pml4, as_cookie_next(),
                               "init", 0, 0, NULL);
    if (tid < 0) panic_boot("create_user_task(init) gagal");
    // task 0 jadi idle supervisor
}
```

`create_user_task` (`kernel/sched/lifecycle.c:371`) **sudah benar** — ia
memalsukan frame `CS=0x1B`/`SS=0x23` + TSS RSP0 per-CPU. Tidak perlu diubah.

`init.elf` bertugas:
1. `sys_spawn("/apps/desktop.elf")`
2. `sys_spawn("/apps/login.elf")`
3. Mengawasi: kalau anak exit dengan kode non-nol → restart (dengan backoff),
   catat ke serial. Ini yang membuat sistem tidak bisa "mati" karena satu app.

### Langkah 3 — Ganti longjmp `sys_exec` dengan iretq benar

`kernel/syscall/sys_proc.c:310-328` melakukan:

```c
__asm__ volatile("mov %0, %%rsp\n xor %%rbp, %%rbp\n sti\n jmp *%1\n" ...);
```

Ini **melewati `iretq` sepenuhnya** (PM-2). Hanya valid karena `user_shell` ada
di kernel (segmen `0x08`/`0x10`). Setelah shell jadi ELF, jalur ini harus:

```c
r->rip = entry;
r->rsp = new_stack_top;
r->cs  = 0x1B;   // user code | RPL3
r->ss  = 0x23;   // user data | RPL3
// iretq di isr128_stub yang menurunkan CPL — sama seperti sys_exec sukses
```

### Langkah 4 — Bersihkan warisan

| Item | Alasan |
|---|---|
| `libs/core/kernel_userlib.c` (13 KB) | Duplikat `userlib.c` untuk jalur Ring 0. Setelah 0 pemakai → hapus |
| `TASK_KIND_KERNEL` exec-chain (`include/task.h:94`) | Model "task 0 jalankan app lalu longjmp" tidak berlaku lagi |
| `g_shell_return_rsp` (`system/shell.c:19`) | Hanya dipakai jalur longjmp |
| `sys_exec` longjmp | Diganti Langkah 3 |

### Langkah 5 — Verifikasi

- `badptr.elf` — test negatif pointer kernel (harus 27 PASS, sekarang dari init ring-3)
- `make conc` — 5/5 PASS di `-smp 4`
- Boot → login root/1 → shell → 20 app
- Konfirmasi `CS & 3 == 3` dari dalam shell (butuh syscall getter atau cetak di
  `sys_get_task_id`)

## Progres

### Fase 1 — Bersihkan `kernel/kernel.c` (SELESAI)

| Perubahan | Hasil |
|---|---|
| `boot_*` helper (78 baris) → `kernel/boot_console.c` + `include/boot_console.h` | Presentasi terpisah dari urutan boot |
| `kernel_main` (360 baris) → 9 `boot_phase_*()` bernama | Terbaca sebagai urutan tahap |
| `string_length()` (dead, 0 caller) | Dihapus |
| `print_hex()` | Pindah ke `kernel/kprint.c` (pemilik jalur konsol) |
| `extern fs_node_t tty_node` (tak terpakai) | Dihapus |
| `switch_to_user_mode()` stub → `boot_handoff_to_init()` | Komentar jujur + dokumentasi jebakan US=0 |
| **Bonus:** `include/kprint.h` kanonis | 14 TU yang menulis `extern void kprint(...)` manual dikonversi; memperbaiki drift nyata (`kernel/proc/elf.c:9` mendeklarasikan `kprint_num(uint32_t)` padahal aslinya `uint64_t`) |

### Fase 2 — Buka jalan perintah shell (SELESAI)

Perintah di `system/shell.c` dulu memanggil simbol kernel langsung, sehingga
terkunci ke Ring 0. Sekarang semuanya lewat API userlib:

| Perintah | Dulu | Sekarang |
|---|---|---|
| `gpu` | `ghal_stats_dump()` (kernel-only) | `sys_gpu_stats()` — syscall 65 di Ring 3, shim di Ring 0 |
| `sleep` | `timer_sleep_ms()` | `sys_sleep()` — syscall 46 |
| `refresh` | `timer_get/set_refresh_rate()` | `sys_get/set_refresh_rate()` — **syscall 87/88 (baru)** |
| `zen` | `zen_main()` (in-process) | `sys_spawn("/apps/zen.elf")` — ELF ring-3 |

`system/zen.c` sekarang ELF: `zen_main` → `main`, `timer_sleep_ms` → `sys_sleep`,
`include/zen.h` dihapus. Bonus: zen berjalan sebagai task sendiri, jadi shell
tidak lagi terblokir saat editor terbuka.

`ghal_stats_dump()` (0 caller setelah migrasi) dihapus dari `graphics/ghal.{c,h}`.

### Fase 2 — Catatan efek samping

`system/shell.c` sekarang hanya boleh memakai `include/userlib.h`. Aturan ini
ditulis sebagai komentar di header file supaya tidak dilanggar lagi.

## Yang TIDAK berubah (sengaja)### Fase 3 — Migrasi login/shell + init.elf (SELESAI)

| Perubahan | Hasil |
|---|---|
| `system/init.c` (BARU) | Supervisor PID 1: spawn desktop + login, `waitpid` loop, restart dengan backoff (1s→30s cap). Login mati → desktop ikut di-restart agar sesi konsisten |
| `system/login.c` | `user_login()` → `main()`; `timer_sleep_ms` → `sys_sleep`; `user_shell()` (loop kernel) → `sys_spawn_argv("shell.elf", uid)` + `waitpid` |
| `system/shell.c` | `user_shell()` → `main()`; buang `extern fs_node_t tty_node` + `read_fs`; `console_poll_input` lewat `sys_read_keyboard` (syscall 3, sudah non-blocking); `g_shell_return_rsp` dihapus; weak socket wrappers dihapus (kini `netutil.o` yang menyediakan); `try_implicit_exec` pakai `sys_spawn`+`waitpid` (bukan `sys_exec` yang mengganti image shell); `logout` → `sys_exit()` |
| `kernel/kernel.c` | `boot_handoff_to_init()` memuat `/apps/init.elf` ke AS baru + `create_user_task` (frame `CS=0x1B/SS=0x23`). Gagal = `boot_halt` dengan pesan jelas |
| `kernel/syscall/sys_proc.c` | **PM-2 FIXED**: longjmp `mov rsp; jmp *user_shell` dihapus. `sys_exit` (34) untuk task non-SPAWNED kini lapor bug + halt (kode kernel tidak punya frame user). Fallback `r->rsp = g_shell_return_rsp` dihapus — stack gagal = exec gagal, bukan lanjut dengan RSP kernel |
| `include/shell.h` | Deklarasi `user_shell`/`g_shell_return_rsp` dihapus |
| `include/task.h` | Komentar `TASK_KIND_KERNEL` diperbarui: tidak boleh memanggil syscall 34 |

**Urutan uid (penting).** Shell di-spawn **sebelum** login menurunkan uid, lalu
uid target diteruskan sebagai `argv[1]` dan shell sendiri yang memanggil
`sys_set_uid()`. Kalau login menurunkan uid lebih dulu, `cred_inherit()`
membuat shell mewarisi uid non-root dan perintah root-only
(`format`/`shutdown`/`reboot` — `sys_fs.c:15`, `sys_system.c:63,70,86`) ditolak
selamanya. Bonus: login tetap root, jadi login kedua di loop yang sama tidak
lagi gagal `sys_set_uid` (di kode lama login menurunkan uid-nya sendiri sekali).

### Fase 4 — Bersihkan warisan (SELESAI)

| Item | Aksi |
|---|---|
| `libs/core/kernel_userlib.c` (13 KB) | **Dihapus** — 0 pemakai setelah semua kode user keluar dari kernel |
| `libs/core/userutil.c` | `filter-out` dari kernel (kini ELF-only); komentar header diperbarui |
| `system/shell_core.c` | `filter-out` dari kernel (dipakai `shell.elf` + `terminal.elf`) |
| `TASK_KIND_KERNEL` exec-chain | Semantik dihapus (makro tetap ada untuk task kernel murni) |
| `g_shell_return_rsp` | Dihapus dari `system/shell.c` + `include/shell.h` |
| Komentar usang `kernel_userlib` | Dibersihkan di 6 file (`kyuzenfs.h`, `kfs_shim.c`, `elf.c`, `sys_misc.c`, `syscall.h`, `shell.c`) |

### Verifikasi Fase 3+4

| Cek | Hasil |
|---|---|
| `make all` dari nol | exit 0, 0 error, 0 warning |
| `make apps` | **28 ELF** |
| `make boot_image.iso` | 30,1 MB, exit 0 |
| Host test suite | **17/17 hijau** |
| `llvm-nm build/bin/myos.bin` | `user_login`, `user_shell`, `zen_main`, `cmd_start`, `shell_init`, `current_username`, `g_shell_return_rsp` — **semua hilang** |
| `make -n all \| grep build/obj/system/` | **kosong** — tidak ada objek `system/` di link kernel |
| `init.elf` / `login.elf` / `shell.elf` | `T main` di `0x4000000`, **0 undefined symbol** |

### Bug pra-eksisting yang ditemukan (TIDAK diperbaiki — di luar lingkup)

**`sudo` tidak bisa naik ke root.** `run_elevated()` (`shell_core.c:469`)
memanggil `sys_set_uid(0)` untuk elevasi, tetapi kernel hanya mengizinkan
transisi **dari** root (`cred_transition_allowed(uid) = (uid == 0)`,
`include/cred.h:47`). Jadi dari uid non-root, `sudo` selalu ditolak.

Perilaku ini **identik sebelum dan sesudah migrasi** (diverifikasi dengan
menelusuri jalur lama: login menurunkan uid → `user_shell()` di task yang sama
→ `sudo` → `sys_set_uid(0)` → ditolak). Bukan regresi.

Perbaikan yang benar butuh keputusan desain (setuid-bit, sudoers, atau
capability), jadi dicatat di sini alih-alih ditambal. Lihat `fix/FIX.md` UP-4
(password plaintext) yang satu rumpun dengan ini.


### Status akhir Fase 1+2 (sudah disuperseded oleh Fase 3+4)

Catatan di bawah ini berlaku **saat Fase 1+2 selesai**. Setelah Fase 3+4
dikerjakan, ketiganya sudah tidak berlaku lagi — lihat bagian Fase 3 dan 4 di
atas:

- ~~Task 0 tetap `TASK_KIND_KERNEL`~~ — masih berlaku: task 0 memang task kernel
  murni, tetapi sekarang ia masuk `scheduler_idle_loop()` setelah handoff dan
  tidak pernah menjalankan kode user.
- ~~`libs/core/kernel_userlib.c` masih ada~~ — **sudah dihapus** (Fase 4).
- ~~`sys_exec` longjmp masih ada~~ — **sudah dihapus** (Fase 3, PM-2 FIXED).

## Verifikasi Fase 1+2

| Cek | Hasil |
|---|---|
| `make all` dari nol | exit 0, 0 error, 0 warning |
| `make apps` | 25 ELF (sebelumnya 0 — lihat catatan di bawah) |
| `make boot_image.iso` | 30,1 MB, exit 0 |
| Host test suite | 17/17 hijau |
| `llvm-nm build/bin/myos.bin` | `zen_main`, `string_length`, `switch_to_user_mode`, `ghal_stats_dump` **hilang**; `sys_gpu_stats`, `sys_get_refresh_rate`, `sys_set_refresh_rate`, `boot_state` ada |
| `llvm-nm build/apps/zen.elf` | `T main` di `0x4000000`, nol simbol kernel |

### Catatan: `CMAKE` tidak terdefinisi (bug pra-eksisting)

`Makefile` merujuk `"$(CMAKE)"` di rule LLVM libc (baris 762-770) tetapi
**tidak pernah mendefinisikannya** — `git show HEAD:Makefile` mengonfirmasi.
Akibatnya `make apps` gagal `Error 127` (command not found) di **setiap clone**,
dan `build/apps/` selalu kosong. Diperbaiki dengan `CMAKE ?= cmake` di blok
Tools. Tanpa ini, `zen.elf` tidak mungkin dibangun maupun diverifikasi.

Catatan: setelah `make clean`, `make apps` pertama kadang gagal di
`_impure_ptr` karena SDK C++ stage belum lengkap; menjalankannya sekali lagi
berhasil. Urutan dependensi ini rapuh dan layak diperbaiki terpisah.
