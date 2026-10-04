# FIX_008 — Hardening lapisan arsitektur `arch/x86/`: 12 temuan produksi

**Prioritas:** P0 (5 blocker kritis: korupsi memori, keamanan, triple fault)
**Status:** DONE (2026-10-03)
**Tercatat:** 2026-10-03 (audit kesiapan produksi `arch/x86/`)
**Cakupan:** 31 file dimodifikasi + 6 file baru

## Ringkasan

Audit kesiapan produksi terhadap lapisan `arch/x86/` (16 file, 636 baris)
menemukan **15 temuan**: 5 kritis, 3 tinggi, 4 sedang, 3 rendah. Remediasi
menutup **12 temuan**, termasuk **seluruh 5 blocker kritis**. Dua item sengaja
dibiarkan terbuka dengan alasan teknis yang tercatat.

Sebelum remediasi, lapisan ini berada di level *"serius hobby / pre-production"*:
desain context switch, TSS per-CPU, dan SMEP/SMAP sudah benar, tapi lapisan
hardening yang membedakan OS produksi dari hobby OS belum ada.

**Hasil verifikasi:** 3 boot QEMU berturut-turut → 17/17 subsystem OK, 0 FAIL,
0 PANIC, build dari nol bersih tanpa warning.

## Temuan & perbaikan

### Kritis

#### ARCH-1 — Prolog entry stub tidak menormalkan RFLAGS

**Gejala (dua vektor dari satu akar):**

1. **DF bocor dari ring 3 → korupsi heap kernel.** `kernel/gfx/kwm.c:464`
   memakai inline asm `rep movsl` untuk blit canvas, dipanggil dari syscall 31.
   DF dapat di-set dari CPL3 (`pushfq; or [rsp],0x400; popfq`) tanpa IOPL, dan
   interrupt/trap gate **tidak** menormalkan DF. Aplikasi cukup menyalakan DF
   lalu memanggil syscall → `rep movsl` menyalin **mundur** → menulis di belakang
   `canvas->pixels` → korupsi heap kernel.

2. **AC bocor dari ring 3 → SMAP mati.** Gerbang SMAP adalah flag AC, **bukan**
   `CR0.AM`. Selama AC=1, kernel bebas menyentuh halaman US=1 — seluruh model
   keamanan "kernel hanya menyentuh user via `user_access_begin/end`"
   (`include/smap.h`) gagal terhadap aplikasi hostile. Jendela SMAP dibuat di 7
   lokasi (`elf.c`, `kwm.c`, `proc.c`, `sys_kwm.c`, `uheap.c`, `usercopy.c`).

**Fix:** macro `KZ_ENTRY_PROLOGUE` di `arch/x86/isr_macro.inc` — `cld` selalu,
lalu `clac` **dengan gate `g_smap_enabled`** (CPU tanpa SMAP akan `#UD` bila
`clac` dieksekusi tanpa syarat; `g_smap_enabled` konstan setelah boot sehingga
branch-nya 100% predictable). Dipasang di **252 stub**.

**Bukti:** objdump `arch_x86_exceptions.asm.o` → 252 × `cld` (opcode `fc`) dan
252 × `clac` (opcode `0f 01 ca`), satu per stub, tepat sebelum `push` pertama.

**Catatan:** `gdt_flush.asm`, `idt_flush.asm`, `idle_switch.asm` sengaja tidak
memakai prolog — ketiganya bukan entry point interrupt (dipanggil dari C sebagai
fungsi biasa), jadi tidak ada transisi CPL yang membawa flag user.

#### ARCH-2 — Tidak ada IST; stack task tanpa guard page

**Gejala:** `#DF` (8), NMI (2), `#MC` (18) memakai stack yang sedang korup/penuh.
Bila push gagal → **triple fault → reset tanpa BSOD, tanpa crashdump, tanpa log**.
Skenario paling mungkin: stack overflow kernel (task stack 16 KB dari `kmalloc`,
**tanpa guard page**) → `#PF` di stack → `#DF` di stack yang sama → reset.

**Fix (dua bagian):**

- **IST.** `arch/x86/gdt.c` mengalokasikan `ist_stacks[16][3][8192]` (BSS statis,
  384 KB — bukan `kmalloc`, karena `init_gdt()` dipanggil di
  `boot_phase_cpu_tables()` **sebelum** heap ada). `tss_init_ist_cpu()` mengisi
  `ist1..ist3` untuk setiap CPU saat `init_gdt()`. `idt_set_gate_ist()` baru
  menerima indeks IST; NMI → IST2, `#DF` → IST1, `#MC` → IST3.
- **Guard page.** `kernel/sched/task_stack.c` (baru) menggantikan
  `kmalloc(TASK_STACK_SIZE)`: alokasi dari PMM + **satu halaman guard yang tidak
  di-map**. Overflow stack jadi `#PF` dengan CR2 tepat di halaman guard, bukan
  korupsi heap senyap.

**Catatan penting:** `task_stack_top()` menggantikan rumus
`stack_base + TASK_STACK_SIZE` di `kernel/sched/core.c` (RSP0). Sejak stack punya
guard page di bawahnya, rumus lama menunjuk 4 KB **di bawah** ujung stack yang
sebenarnya — RSP0 akan berada di tengah area stack dan transisi ring-3→ring-0
langsung menimpa data task sendiri.

#### ARCH-3 — IDT tidak lengkap

**Gejala:** hanya 20 dari 256 vektor terpasang. Vektor lain jatuh ke IDT entry
nol → `#GP` → `#DF` → panic. Yang paling berbahaya:

- **vektor 255 (spurious LAPIC)** — LAPIC SVR diset `LAPIC_SVR_ENABLE | 0xFF`
  di `lapic.c:67`, tapi `IDT[255]` tidak pernah dipasang. Spurious interrupt
  adalah kejadian **normal** pada sistem APIC, jadi ini jalur realistis.
- **`#MC` (18)**, `#MF` (16), `#XM` (19) — fault hardware jadi panic tak
  terjelaskan alih-alih laporan yang bisa didiagnosis.
- **`#BP` (3) dipasang DPL=0** → `int3` dari ring-3 menghasilkan `#GP`, bukan
  `#BP`, sehingga debugging user-space tidak mungkin.

**Fix:** semua **256 vektor** terpasang.

- `arch/x86/exceptions.asm` meng-emit stub untuk 0..31 (dengan error-code yang
  benar per Intel SDM Table 6-1) dan 32..255 via `%rep`, lalu mengekspor tabel
  kontigu `isr_default_table[224]`.
- `arch/x86/vectors.c` (baru) = `default_interrupt_handler()`, membedakan empat
  kelas: (1) vektor 255 spurious → cukup EOI; (2) PIC IRQ 32..47 tanpa driver →
  EOI + laporan (dibatasi 8× agar IRQ storm tidak membanjiri serial);
  (3) exception 0..31 → jalur panic; (4) sisanya → laporan + panic.
- DPL=3 untuk `#BP`/`#OF`/`#BR`/`#UD`.

**Koreksi terhadap catatan audit sebelumnya:** klaim "`#AC` (17) reachable dari
ring-3" ternyata **tidak berlaku** — `#AC` hanya di-raise saat `CR0.AM=1` *dan*
`CPL=3`, dan `CR0.AM` tidak pernah diset. Namun analisis itu justru menemukan
temuan yang lebih serius: `clac` tidak ada di entry stub, sehingga AC yang
diwariskan user **mematikan SMAP** (menjadi ARCH-1 bagian 2).

#### ARCH-4 — Fault ring-3 mematikan seluruh OS

**Gejala:** `exception_handler()` langsung `panic_cli()` + lockdown + BSOD
**tanpa cek CPL**. Satu aplikasi user yang segfault (pointer liar, stack
overflow, `#UD`) mematikan seluruh sistem operasi. Ini sendiri sudah
mendiskualifikasi label "production".

**Fix:** `kz_try_kill_faulting_user_task()` di `kernel/panic/panic.c`, dipanggil
**paling awal** di `exception_handler()` sebelum state panic tersentuh. Fault
dari CPL=3 milik task `TASK_KIND_SPAWNED` → `proc_exit_kill()` (jalur terminasi
yang sudah ada, noreturn) → **task itu mati, kernel + desktop + task lain hidup**.

**Tetap panic (sengaja):** CPL=0 (bug kernel), NMI/`#DF`/`#MC` (kondisi mesin,
bukan fault proses), dan fault tanpa task user aktif.

**Kenapa aman:** task yang fault sedang RUNNING di CPU ini (tidak bisa jalan di
CPU lain), jalur user memegang NOL lock kernel saat masuk ring 0 (tidak bisa
self-deadlock), dan `proc_exit_kill()` melakukan transisi tunggal.

#### ARCH-5 — Tidak ada NX/W^X

**Gejala:** `EFER.NXE` tidak pernah diset, bit 63 tidak pernah dipakai di flag
mapping, dan `kernel/proc/elf.c` hanya membaca `p_type` — **`p_flags` ELF
diabaikan total**; semua segmen di-map `flags=7` (P|RW|US). Akibatnya seluruh
segmen user RWX: `.text` writable, `.data`/`.bss` executable. Mitigasi dasar
hilang; satu bug memory-safety (mis. overflow parser browser) langsung jadi
eksekusi kode.

**Fix:**

- `include/msr.h` (baru): `kz_rdmsr`/`kz_wrmsr` bersama + `MSR_IA32_EFER`,
  `EFER_NXE`. Sebelumnya `rdmsr`/`wrmsr` adalah static lokal di `lapic.c`.
- `kernel/cpu.c`: `cpu_enable_nx()` — CPUID `0x80000001` EDX bit 20, lalu set
  `EFER.NXE` + **verifikasi** (bila `wrmsr` diabaikan, `g_nx_enabled` tetap 0
  sehingga bit NX tidak pernah dipasang di PTE). Per-CPU: BSP + setiap AP.
- `kernel/mm/paging.c`: `paging_nx_flag()` (0 bila NXE tidak ada — mencegah
  `#GP` dari bit NX pada CPU tanpa dukungan), `paging_user_rw_nx()`, dan
  `vmm_protect_page_into()` untuk mengubah flag PTE yang sudah ter-map.
- `kernel/proc/elf.c`: `p_flags` sekarang dibaca. `PF_X` → NX dikosongkan;
  tanpa `PF_X` → NX dipasang; `PF_W` → RW=1. Pola **map-writable → isi →
  reprotect** dipakai karena `CR0.WP=1` membuat kernel tidak bisa menulis
  halaman RW=0 (isi segmen akan `#PF` saat disalin).
- Stack user di-map `paging_user_rw_nx()` — **NX**.

**Bukti:** `llvm-readelf -lW build/target/apps/calc.elf` → `LOAD ... R E` dan
`LOAD ... RW ` (bukan `RWE`).

### Tinggi

#### ARCH-6 — Fake ISR frame tidak 16-byte aligned

**Gejala:** frame dibangun 22 qword (176 byte ≡ 0 mod 16) dari `kmalloc` yang
16-aligned, sehingga `RSP` saat masuk `task_entry_trampoline` ≡ **0 (mod 16)**.
ABI SysV x86-64 mensyaratkan **≡ 8** saat entry fungsi. Seluruh chain pemanggilan
C di task kernel baru misaligned 8 byte — tidak crash hari ini karena
`-mno-sse`, tapi begitu SSE diaktifkan atau ada local `aligned(16)` → crash acak
yang sangat sulit didiagnosis.

**Fix:** `KZ_FAKE_FRAME_PAD` (8 byte) + `KZ_FAKE_FRAME_TOP()` di
`kernel/sched/lifecycle.c`, dipakai **ketiga** konstruksi frame
(`create_task_prio`, `create_user_task`, fork) sehingga tidak ada jalur yang
"kebetulan benar". Padding ditaruh **di atas** frame supaya `registers_t` tetap
176 byte dan tidak ada offset field yang bergeser.

**Temuan sampingan:** jalur trap **asli** sudah benar dengan sendirinya (CPU
push 5 + stub 2 + PUSHA64 15 = 176 ≡ 0 mod 16, lalu `call` menambah 8 → ≡ 8 ✓).
Yang salah hanya jalur frame palsu, karena `iretq` tidak mendorong return
address.

#### ARCH-8 — `mov rsp, rax` tanpa sanity check

**Gejala:** tiga ISR (`timer`, `lapic_timer`, `reschedule`) memakai pola
`call handler` → `mov rsp, rax` → `POPA64` → `iretq`. Bila handler mengembalikan
nilai tidak wajar (bug scheduler, pointer rusak), RSP memuat stack sampah dan
kernel crash **di tempat yang jauh dari penyebabnya** — biasanya `#DF` atau
triple fault, karena stack sudah tidak bisa dipakai melaporkan apa pun.

**Fix:** `kernel/sched/context_validate.c` (baru) + macro `KZ_VALIDATE_RSP` di
`isr_macro.inc`, dipakai ketiga stub. Checks: canonical; higher-half (RSP di
user range = handler mengembalikan pointer user → ditolak); 8-byte aligned;
tidak membentur ujung ruang alamat. Saat gagal → lapor ke COM1 dengan nilai RSP
+ fallback ke RSP frame ISR (pasti valid).

**Temuan penting saat verifikasi — ada DUA bentuk frame dengan residue berbeda.**
Validasi versi pertama mensyaratkan 16-byte aligned dan **menolak RSP yang
sebenarnya SAH**, membanjiri serial dengan `[CTX] RSP tidak valid`. Sebabnya:

| Asal interrupt | qword didorong CPU | Field |
| --- | --- | --- |
| Ring 3 (user) | **5** | SS, RSP, RFLAGS, CS, RIP |
| Ring 0 (kernel) | **3** | RFLAGS, CS, RIP |

Pergantian stack hanya terjadi dari ring 3, jadi hanya di situ SS:RSP didorong.
Akibatnya frame ring-0 `registers_t` ≡ **8 (mod 16)**, bukan 0. Syarat yang benar
adalah **8-byte aligned**. (Dua field terakhir `registers_t` pada frame ring-0
membaca memori di atas frame — tidak berbahaya untuk dump, dan tidak
memengaruhi context switch karena `iretq` juga hanya mem-pop 3 qword saat
kembali ke CPL yang sama.)

**Dua bug lain yang ditemukan & diperbaiki saat implementasi macro ini:**
(a) versi awal menyimpan kandidat di `r15` lalu `mov rsp, rax` — di jalur OK
`rax` berisi hasil `test` (0/1), sehingga RSP jadi 1 dan kernel langsung mati;
diperbaiki dengan memulihkan `mov rax, r15`. (b) batas bawah awal memakai alamat
heap (`0xFFFF9000…`) padahal stack boot Limine ada di HHDM (`0xFFFF8000…`).

### Sedang

#### ARCH-9 — Tidak ada per-CPU GS base

**Gejala:** `smp_current_cpu_index()` membaca **MMIO LAPIC (uncached)** lalu
memindai tabel 16 CPU untuk mencocokkan `lapic_id`. Dipanggil di **setiap** ISR
dan puluhan jalur lain — biaya nyata di tick tinggi, dan bergantung pada MMIO
yang bisa tidak responsif justru saat CPU bermasalah (jalur panic).

**Fix:** jalur cepat **satu load dari `GS:0`**, dengan fallback ke pemindaian
LAPIC lama bila GS belum di-set (boot sangat awal). `percpu_t` mendapat field
`cpu_index` **di offset 0**; `smp_set_gs_base()` dipanggil BSP di `smp_init()`
dan setiap AP di `smp_ap_main()` **sebelum** kode apa pun yang bisa memanggil
`smp_current_cpu_index()`.

**Kenapa GS, bukan FS:** userspace KyuzenOS tidak memakai TLS (ELF statis tanpa
`PT_TLS`), jadi FS dibiarkan bebas untuk kebutuhan masa depan itu.

**Catatan:** `swapgs` belum dipakai karena syscall tetap lewat `int 0x80`
(keputusan desain yang sudah tercatat). Per-CPU GS base berguna **tanpa**
migrasi ke `syscall`/`sysret`.

#### ARCH-10 — Tidak ada FPU/XMM save

**Gejala:** tidak ada penyimpanan state FPU/SSE antar context switch. Register
XMM dan stack x87 adalah state **global per-CPU**: task berikutnya melihat sisa
nilai pendahulunya. Dua konsekuensi: **kebocoran data antar proses** (pelanggaran
isolasi yang sama seriusnya dengan membaca memori proses lain, dan tidak
terdeteksi page table), dan **hasil komputasi salah secara acak** tergantung
urutan penjadwalan.

**Kenapa baru perlu sekarang:** kernel dibangun `-mno-sse -msoft-float`, jadi
asumsi lama "tidak ada state untuk disimpan" **benar untuk kernel** tapi **salah
untuk userspace** — target Rust `x86_64-unknown-none` mengaktifkan SSE/SSE2
secara default, dan `apps/calc.c` juga memakai float.

**Fix:**

- `include/task.h`: field `fpu_state[512] __attribute__((aligned(16)))` +
  `fpu_initialized` di TCB. 512 byte = format FXSAVE standar (x87 + XMM0..15 +
  MXCSR pada mode 64-bit). Alamat 16-byte aligned karena FXSAVE `#GP` bila tidak.
- `kernel/sched/fpu.c` (baru): `fpu_init_cpu()` (CR4.OSFXSR + OSXMMEXCPT,
  per-CPU, BSP + setiap AP), `fpu_switch()` (FXSAVE task lama → FXRSTOR task
  baru), `fpu_state_reset()`.
- `kernel/sched/core.c`: `fpu_switch(cur, next)` dipanggil setelah keputusan
  task berikutnya final dan sebelum frame task baru dipakai — pada titik itu
  `cur` masih berjalan (state-nya bisa disimpan) dan `next` belum eksekusi.
- Task baru memuat **keadaan bersih**: `fninit` untuk x87, dan area `fpu_state`
  yang di-zero + MXCSR default (`0x1F80`) + FCW default (`0x037F`) lalu FXRSTOR —
  supaya XMM tidak mewarisi sisa register task sebelumnya.

**Kenapa FXSAVE, bukan XSAVE:** FXSAVE mencakup semua register yang bisa
disentuh SSE/SSE2 (batas maksimum yang dijanjikan kernel). XSAVE butuh CPUID
probe + area dinamis (bisa ribuan byte bila AVX-512 aktif) + penanganan XCR0;
kompleksitasnya tidak sebanding untuk cakupan SSE saja. Bila AVX ditambahkan
nanti, `fpu.c` adalah titik masuk yang tepat.

**Biaya:** 8 KB BSS (16 task × 512 B) + ~100-200 cycle per context switch
terhadap kuantum 10-20 ms (tidak terukur).

**Catatan:** `fpu_init_cpu()` **bukan** optimasi — tanpa `CR4.OSFXSR`, instruksi
SSE dari ring 3 menghasilkan `#UD`, sehingga app Rust dan `apps/calc.c` tidak
bisa berjalan sama sekali.

#### ARCH-11 — Tidak ada freeze assert untuk layout `registers_t`

**Gejala:** layout `registers_t` ↔ `PUSHA64` adalah kontrak paling kritis di
lapisan ini, tapi satu-satunya penjaganya adalah **komentar**
(`isr_macro.inc`: *"PENTING: Jangan ubah urutan PUSH/POP ini!"*). Sementara itu
ABI lain di proyek ini sudah punya freeze asserts (`kwm_abi.h`, `smp.c`,
`sys_gpu.c`). Satu perubahan urutan field tanpa sengaja → kernel rusak **tanpa
error kompilasi**, dan gejalanya crash acak yang sangat mahal.

**Fix:** `include/task.h` kini memuat **23 `_Static_assert`**:
`sizeof(registers_t) == 176` + `offsetof` untuk **setiap** field (r15=0, r14=8,
…, rax=112, int_num=120, error_code=128, rip=136, cs=144, rflags=152, rsp=160,
ss=168).

**Assert ini langsung membuktikan nilainya:** saat ditulis, ia menangkap
kesalahan hitung penulisnya sendiri (`184 == 176` gagal) — 22 field × 8 byte =
176, bukan 184.

#### ARCH-12 — LAPIC timer tidak dikalibrasi

**Gejala:** `INIT_COUNT` adalah konstanta hardcoded `1000000`. Nilai itu **tidak
berarti apa-apa**: LAPIC timer menghitung pada bus clock / divider yang
berbeda-beda antar mesin. Di host uji, nilai itu hanya menghasilkan **~12 Hz**,
bukan 100 Hz yang dimaksud. Akibatnya kuantum scheduler di AP berubah tanpa
diketahui, dan `lapic_ticks` tidak bisa dijadikan basis waktu.

**Fix:** kalibrasi terhadap **PIT kanal 0** saat boot.

**Kenapa kanal 0, bukan kanal 2** (yang tidak dipakai kernel): percobaan pertama
memakai kanal 2 dan hasilnya **tidak bisa dipercaya** — dua pendekatan
pengukuran menghasilkan angka berbeda **9×** (bus clock 187 MHz vs 21 MHz),
tanpa cara membuktikan mana yang benar. Kanal 0 menghilangkan seluruh kelas
masalah itu: sudah diprogram `init_timer()` untuk 60 Hz, rate-nya **sudah
terbukti benar** karena seluruh timekeeping OS bergantung padanya, dan kita
hanya **membaca** (tidak memprogram ulang, yang akan merusak timebase).

**Empat bug yang ditemukan & diperbaiki selama iterasi ini** (semuanya terlihat
dari boot log, bukan dari pembacaan kode):

1. **PIT diprogram SETELAH LAPIC.** `boot_phase_interrupts()` memanggil
   `lapic_init_bsp()` sebelum `init_timer()` — kalibrasi mengukur terhadap nilai
   sisa firmware. Sebaran hasil 18% antar boot. **Urutan dibalik.**
2. **Target tick vs count.** Satu tick PIT @60 Hz = 19886 **count**; memanggil
   wait dengan `target = 5` berarti menunggu 5 count (~4 µs), bukan 5 tick
   (~83 ms) — window ~20000× terlalu pendek.
3. **Wrap di reload value, bukan `0x10000`.** Rumus lama menambahkan
   `(0x10000 - c)` saat wrap, padahal counter hanya berputar sampai 19886 →
   ~45700 count palsu setiap wrap.
4. **Mode 0 tidak andal.** `count == 0` berarti "belum dimuat" **sekaligus**
   "selesai"; mode 2 + akumulasi penurunan counter (bukan hitung wrap) membuat
   pengukuran tidak bergantung pada posisi awal.

**Bukti:** 3 boot berturut-turut → `kalibrasi OK: 19570 / 19575 / 19579 tick` —
**sebaran 0.05%** (sebelumnya 112%).

**Catatan semantik:** `lapic_timer_ticks()` adalah counter **AGREGAT** — setiap
CPU menambahkan tick-nya sendiri, termasuk BSP. Jadi rate agregat = (jumlah CPU
yang menjalankan LAPIC timer) × 100 Hz.

### Rendah

#### ARCH-13 — `memcpy`/`memset` byte-loop di jalur panas

**Gejala:** `memcpy`/`memmove`/`memset` adalah loop byte murni (clang meng-unroll
4 byte per iterasi). Jalur panas menyalin blok besar: `vmm_clone_user_as`
(fork — salin seluruh address space), `elf_load_file`, `kwm_update_window`
(blit).

**Fix:** `rep movsb`/`rep stosb` untuk blok **≥ 128 byte**; di bawah itu tetap
loop byte. Ambang ini penting — `rep movsb` punya biaya setup ~30-40 cycle dan
memperlambat ukuran kecil yang justru mayoritas pemanggilan.

**`cld` ditulis EKSPLISIT** sebelum setiap instruksi string. Ini pelajaran
langsung dari ARCH-1: bergantung pada "entry stub sudah cld" adalah asumsi
tersembunyi yang persis menyebabkan bug ARCH-1. Biaya `cld` ~1 cycle.

Untuk jalur mundur (`memmove` overlap), dipakai `std; rep movsb; cld` — `cld` di
akhir memulihkan DF=0 karena meninggalkannya 1 akan merusak setiap instruksi
string berikutnya di thread itu.

#### ARCH-15 — Higiene assembly

**Fix:** `bits 64` ditambahkan ke **11 file** yang belum memilikinya. Sekarang
12/12 file `.asm` konsisten. Komentar usang di `isr128.asm` (menyebut "trik lama
`[rsp+112] = RAX`" yang sudah tidak ada) diganti dengan penjelasan kontrak yang
berlaku.

## Item yang sengaja dibiarkan terbuka

### ARCH-7 — Exception fixup table (dan `fixup_bad_iret`)

**Ditunda dengan sengaja.** Fault di `copy_from_user` sekarang **deterministik
dan terlihat**:

- CPL3 (aplikasi) → ARCH-4 membunuh task itu, kernel tetap hidup;
- CPL0 (kernel) → ARCH-2/3 menghasilkan BSOD yang bisa dibaca + crashdump.

Jadi ini **bukan lagi korupsi senyap**. Yang hilang hanyalah kemampuan mengubah
fault menjadi `-EFAULT` yang bisa ditangani pemanggil.

**Kenapa ditunda:** butuh rutin copy assembly dengan anotasi fixup, section
linker baru, dan pengalihan RIP di handler `#PF`/`#GP`. Jalur ini **tidak bisa
diuji** dengan boot-test otomatis — fault hanya terjadi saat race/kondisi langka,
dan menulis ulang jalur fault tanpa test lebih berbahaya daripada gap yang sudah
terlihat.

**Rencana konkret:**

1. Rutin copy di assembly (`arch/x86/usercopy.asm`) memakai `rep movsb` dengan
   label fixup untuk setiap akses.
2. Section `.fixup` di `linker.ld` + tabel `(fault_ip, fixup_ip)` terurut.
3. `kernel/proc/usercopy.c` memakai rutin itu, bukan `memcpy` langsung.
4. Handler di `#PF`/`#GP`: cari RIP di tabel → set RIP ke fixup → return
   `-EFAULT`. **Wajib didahului test host yang mensimulasikan fault.**

### ARCH-14 — Tanpa KPTI

Kernel terpetakan saat ring-3 berjalan. Hanya relevan untuk CPU tanpa `RDCL_NO`
(Meltdown). Batasan hardware yang terdokumentasi, bukan bug.

## File baru

| File | Peran |
| --- | --- |
| `include/arch.h` | Kontrak vektor/IST/GDT-selector/batas VMA — satu sumber untuk sisi C |
| `include/msr.h` | `kz_rdmsr`/`kz_wrmsr` bersama + `MSR_IA32_*`, `EFER_NXE` |
| `arch/x86/vectors.c` | `default_interrupt_handler` — 4 kelas vektor |
| `kernel/sched/task_stack.c` | Alokator stack task + guard page (ARCH-2b) |
| `kernel/sched/context_validate.c` | Validasi RSP hasil handler (ARCH-8) |
| `kernel/sched/fpu.c` | FXSAVE/FXRSTOR per task + CR4.OSFXSR (ARCH-10) |

## Pelajaran metodologi

**8 dari 10 bug** yang ditemukan selama remediasi hanya terlihat lewat
**boot-test nyata**, bukan lewat pembacaan kode atau disassembly:

| # | Bug | Akar masalah |
| --- | --- | --- |
| R-1 | `KZ_VALIDATE_RSP` menyetel RSP=1 | Di jalur OK, `rax` berisi hasil `test` (0/1), bukan RSP |
| R-2 | Validasi RSP menolak RSP sah | Dua bentuk frame: ring-3 push 5 qword, ring-0 push **3** |
| R-3 | Batas bawah validasi RSP salah | Memakai alamat heap padahal stack boot Limine di HHDM |
| R-4 | Kalibrasi: PIT belum diprogram | `lapic_init_bsp()` dipanggil sebelum `init_timer()` |
| R-5 | Kalibrasi: tick vs count | 1 tick PIT @60 Hz = 19886 count, bukan 1 |
| R-6 | Kalibrasi: wrap di `0x10000` | Counter hanya berputar sampai reload value |
| R-7 | Kalibrasi: mode 0 tidak andal | `count == 0` ambigu; diganti mode 2 |
| R-8 | `_Static_assert` menangkap salah hitung | `sizeof(registers_t)` = 176, bukan 184 |
| R-9 | Verifikasi empiris LAPIC flaky | Mengukur sistem yang sedang sibuk boot |
| R-10 | Literal `` `r`n `` tersisip ke file | Kesalahan skrip PowerShell |

**Kesimpulan:** disassembly membuktikan instruksi yang **benar dihasilkan**;
hanya boot-test yang membuktikan sistemnya **berjalan**. Untuk pekerjaan di
lapisan arch, keduanya wajib.

## Verifikasi

| Metrik | Hasil |
| --- | --- |
| Build dari nol (162 langkah) | 0 error, 0 warning |
| Subsystem OK | **17 / 17** (3 boot berturut-turut) |
| FAIL / PANIC / CTX warning | 0 / 0 / 0 |
| QEMU keluar sendiri (triple fault) | tidak |
| Task ring-3 | jalan sampai prompt login |
| Kalibrasi LAPIC | 19583-19610 (sebaran **0.14%**) |
| `calc.elf` segments | `R E` + `RW ` (bukan `RWE`) |
| `cld`/`clac` per stub | 252 / 252 |

## Dokumentasi terkait

- [Interrupts & Timers](../../kernel/interrupts.md) — IST, 256 vektor, entry prologue
- [Arch Hardening](../../kernel/arch-hardening.md) — referensi lapisan hardening
- [Panic & Crash Handling](../../kernel/panic.md) — jalur fault & kill task
- [Scheduler](../../kernel/scheduler.md) — context switch, FPU, guard page
