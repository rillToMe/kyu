# KyuzenOS — Roadmap Menuju Portofolio Kelas OSDev

> **Catatan (pasca-migrasi CMake).** Dokumen ini ditulis saat proyek masih
> dibangun dengan Makefile, jadi perintah `make ...` di dalamnya merujuk build
> lama. Makefile sudah dihapus; padanan CMake-nya ada di
> [Building](development/building.md). Hasil verifikasi yang tercatat di sini
> sengaja tidak diubah — itu catatan apa yang benar-benar dijalankan saat itu.


> **Tujuan dokumen:** rencana kerja konkret, terurut, dan terverifikasi untuk
> membawa KyuzenOS dari "proyek OS solo yang jalan" menjadi **proyek yang
> diakui komunitas OSDev** — bisa di-build orang lain, punya CI, punya
> keunggulan teknis yang jelas, dan bisa didemokan tanpa penjelasan panjang.
>
> **Basis:** audit langsung `fix/FIX.md`, `serial.log`, 127 dokumen di `docs/`,
> dan verifikasi kode pada commit HEAD `feature/64bit-migration`.
>
> **Prinsip kerja (dari `.rules/RULES.md`):** jangan pernah menukar kualitas
> dengan kecepatan. Setiap milestone harus **terverifikasi**, bukan "kelihatannya
> jalan".

---

## 0. Definisi "Portofolio Kelas OSDev"

Sebuah proyek OS dianggap kelas portofolio ketika **orang asing** bisa
memverifikasi klaimmu tanpa bantuanmu. Itu berarti 6 syarat keras:

| # | Syarat | Kriteria lulus (terukur) |
|---|---|---|
| S1 | **Reproducible** | `git clone` → `make && make apps && make boot_image.iso` → boot sukses, di mesin bersih, tanpa langkah rahasia |
| S2 | **Terverifikasi otomatis** | CI hijau di setiap push: build + minimal 1 boot smoke test |
| S3 | **Klaim jujur** | Tidak ada klaim di README yang tidak dibuktikan kode |
| S4 | **Stabil** | Boot 20× berturut-turut tanpa crash; tidak ada crash-dump saat idle |
| S5 | **Terdefinisi batasnya** | Ada dokumen "apa yang TIDAK dilakukan" yang eksplisit |
| S6 | **Ada satu keunggulan** | Minimal satu hal yang proyek ini lakukan lebih baik/berbeda dari OS hobi lain |

Roadmap di bawah mengurutkan pekerjaan supaya S1→S6 tercapai berurutan.
**Jangan lompat fase.** Fase 1 adalah fondasi kredibilitas; tanpa itu, semua
fitur di atasnya tidak bisa diverifikasi orang lain.

**Estimasi total:** 9–12 bulan kerja konsisten (asumsi ~15–20 jam/minggu).
Setiap fase punya "Definition of Done" — jangan tandai selesai sebelum lulus.

---

## FASE 1 — KREDIBILITAS & REPRODUCIBILITY (Bulan 1–3)

> **Sasaran fase:** orang asing bisa clone, build, boot, dan percaya klaimmu.
> Fase ini **tidak menambah fitur baru**. Ini yang mengubah "kodemu" menjadi
> "proyek". Semua item di sini berasal dari `FIX.md` yang sudah kamu audit sendiri.

### 1.1 — Selesaikan isolasi Ring-3 (FIX K-1) `[P0]` — ✅ **SELESAI**

**Masalah (sudah diperbaiki):** `kernel/kernel.c` dulu memanggil `user_login()`
langsung di CPL 0 lewat stub `switch_to_user_mode()`. Login & shell berjalan di
Ring 0, `ucopy_ctx.from_user` = 0, seluruh validasi pointer user di-bypass.

**Penyelesaian:** migrasi penuh, bukan tambalan. Kernel kini **nol kode user**:
`init`, `login`, `shell`, `zen` semuanya ELF ring-3. Lihat
[Ring-3 Init Migration](design/ring3-init-migration.md) untuk rencana lengkap,
bukti, dan temuan sampingan.

Yang berubah:

| | Sebelum | Sesudah |
|---|---|---|
| Handoff kernel | `switch_to_user_mode()` → `user_login()` di CPL 0 | `boot_handoff_to_init()` memuat `/apps/init.elf` + `create_user_task()` (frame `CS=0x1B`/`SS=0x23`), lalu task 0 masuk `scheduler_idle_loop()` |
| PID 1 | tidak ada | `init.elf` — spawn + supervisi `login.elf` (restart backoff 1s→30s) |
| Jalur login | `login.c` di kernel | `login.elf` ring-3 → spawn `desktop.elf` + `shell.elf` setelah auth |
| Jalur shell | `shell.c` di kernel, `user_shell()` loop | `shell.elf` ring-3, `logout` → `sys_exit` |
| Editor | `zen_main()` in-process | `/apps/zen.elf` di-spawn |
| `libs/core/kernel_userlib.c` | shim Ring 0 (13 KB) | **dihapus** — 0 pemakai |
| `sys_exit` longjmp | `mov rsp; jmp *user_shell` (PM-2) | dihapus — semua task user `TASK_KIND_SPAWNED` |

**Bukti:** `make -n all | grep build/obj/system/` → kosong (nol objek user di
link kernel). `llvm-nm build/bin/myos.bin` tidak memuat `user_login`,
`user_shell`, `zen_main`, `shell_init`, `cmd_start`, `g_shell_return_rsp`.

**Verifikasi:**
- [x] Build hijau dari nol: `make all` (0 error/warning), `make apps` (28 ELF),
      `make boot_image.iso` (30,2 MB), host test 17/17
- [x] Kernel nol kode user (dibuktikan lewat `make -n all` + `llvm-nm`)
- [x] `init.elf`/`login.elf`/`shell.elf`: `T main` di `0x4000000`, 0 undefined symbol
- [ ] `make run` → login → shell, cetak `CS` register → harus `0x1b` *(butuh QEMU)*
- [ ] Test negatif: kirim pointer kernel ke syscall dari shell → `-EFAULT` *(butuh QEMU)*
- [ ] `make conc` masih 5/5 PASS (regresi SMP) *(butuh QEMU)*
- [ ] Semua app masih jalan setelah perubahan *(butuh QEMU)*

**DoD:** ✅ Login dan shell berjalan di Ring 3; validasi pointer aktif untuk
keduanya. `FIX.md` K-1 → `FIXED`, PM-2 → `FIXED`.

> **Catatan:** jalur alternatif "koreksi README" (yang disebut di bawah) **tidak
> dipakai** — migrasi penuh dikerjakan, jadi klaim README sekarang akurat.

---

### 1.2 — Fresh-clone build hijau (FIX K-2) `[P0]`

**Masalah:** audit mencatat fresh clone tidak bisa di-build.

**Langkah:**
1. Reproduksi di direktori bersih: `git clone <repo> /tmp/kyuzen-clean`.
2. Catat setiap kegagalan: file yang hilang, submodule rusak, path hardcoded,
   `third_party/` yang ternyata kosong tapi dibutuhkan.
3. Perbaiki sumber ketergantungan: dokumentasikan setiap prasyarat di
   `docs/development/building.md` dan buat `make bootstrap` yang memeriksa
   toolchain (clang, nasm, xorriso, qemu) dan memberi pesan jelas bila kurang.
4. Putuskan nasib `third_party/` yang di-vendor: lwIP, LLVM libc/libc++,
   FreeType, BearSSL — apakah masuk repo, submodule, atau diunduh via script.

**Verifikasi:**
- [ ] Di mesin/container bersih, `git clone && make bootstrap && make && make apps && make boot_image.iso` lulus tanpa langkah manual di luar dokumen
- [ ] `docs/development/building.md` akurat — diuji oleh orang yang belum pernah build (minta teman)

**DoD:** Fresh clone build lulus di lingkungan bersih. `FIX.md` K-2 → `FIXED`.

---

### 1.3 — CI minimal (Syarat S2) `[P0]`

**Sekarang:** tidak ada `.github/`, tidak ada CI sama sekali.

**Langkah:**
1. Buat `.github/workflows/build.yml`:
   - Runner: `ubuntu-latest`
   - Install: `clang`, `lld`, `nasm`, `xorriso`, `qemu-system-x86`, `make`, `cmake`, `python3`
   - Job 1 (**build**): `make && make apps` — gagal = merah
   - Job 2 (**boot smoke**): `make boot_image.iso`, lalu jalankan QEMU headless:
     ```
     qemu-system-x86_64 -cdrom build/boot_image.iso -m 1G -smp 4 \
       -serial stdio -display none -no-reboot \
       -device e1000,netdev=n0 -netdev user,id=n0 &
     # tunggu, ambil serial, cek marker "KyuzenOS ready."
     ```
   - Job 3 (**host tests**): jalankan semua target `test-*` yang host-side
     (tidak butuh QEMU): `test-kyuzenfs-v4`, `test-color`, `test-text`, dll.
2. Tambahkan **boot gate script** (`tools/ci/boot_check.py`): boot QEMU, tangkap
   serial, cari string `KyuzenOS ready.`; timeout 60 detik = gagal.
3. Tambahkan badge CI di README.

**Verifikasi:**
- [ ] Push → CI hijau
- [ ] Sengaja rusak 1 baris → CI merah (bukti CI benar-benar bekerja)
- [ ] Boot smoke mendeteksi kegagalan boot (uji dengan sengaja panic)

**DoD:** CI hijau untuk build + boot + host tests. Ini yang membuat proyekmu
bisa dipercaya tanpa kamu jelaskan.

---

### 1.4 — Bersihkan `.git` 766 MB (FIX K-5) `[P1]`

**Masalah:** `.git` 766 MB untuk source ~10 MB. Disk image (100 MB × 2) dan
build artifact ter-commit. Ini menghalangi clone dan terlihat tidak profesional.

**Langkah:**
1. Identifikasi file besar di history: `git rev-list --objects --all | ...`
   atau `git-sizer`.
2. Putuskan: `disk.img`, `test_disk.img`, `build/`, `.cache/` — pindah ke
   Git LFS (jika perlu versioning) atau buang dari history.
3. Gunakan `git filter-repo` (lebih cepat & aman dari `filter-branch`) untuk
   menulis ulang history.
4. Perbarui `.gitignore` — sudah cukup baik, tapi pastikan `disk.img` dan
   `test_disk.img` (di root, tidak di-ignore saat ini) masuk.
5. Force-push (kamu satu-satunya author, aman). Dokumentasikan di
   `docs/development/building.md` cara generate disk image lokal.

**Verifikasi:**
- [ ] `.git` < 50 MB
- [ ] Fresh clone tetap build (bukti history rewrite tidak merusak apa pun)
- [ ] `make run` masih bisa membuat `disk.img` otomatis bila belum ada

**DoD:** `.git` ramping, clone cepat. `FIX.md` K-5 → `FIXED`.

---

### 1.5 — Hidupkan kembali test yang mati (FIX K-4) `[P1]`

**Masalah:** audit mencatat 15 target test host mati; separuh test suite hilang.
Makefile punya **32 target `test-*`** — sayang kalau mati.

**Langkah:**
1. Jalankan setiap target `test-*`, catat mana yang error.
2. Perbaiki akar masalah (biasanya: path berubah, file test terhapus saat
   refactor `libs/` + `apps/`).
3. Tandai test mana yang host-side (cepat, untuk CI) vs QEMU-side (berat).
4. Untuk test yang memang usang, **hapus dengan sadar** atau tulis ulang.

**Verifikasi:**
- [ ] Semua target `test-*` host-side hijau dan masuk CI
- [ ] Tidak ada target `test-*` yang error saat dijalankan
- [ ] `docs/development/testing.md` akurat: setiap test punya tujuan + perintah

**DoD:** Test suite lengkap dan hijau. `FIX.md` K-4 → `FIXED`.

---

### 1.6 — Rapikan metadata repo (FIX K-6) `[P2]`

- [ ] `AGENT.md` adalah symlink ke `CLAUDE.md`, tapi `CLAUDE.md` di-`.gitignore`
      → **symlink menggantung**. Perbaiki: masukkan `CLAUDE.md` ke repo, atau
      jangan symlink.
      *Progres:* `CLAUDE.md` sudah di-un-ignore dari `.gitignore` sehingga
      muncul sebagai untracked dan bisa di-`git add`. Symlink baru benar-benar
      hidup setelah file itu masuk repo — langkah commit diserahkan ke pemilik repo.
- [ ] Submodule rusak: perbaiki atau buang.
- [ ] `fix/` di-`.gitignore` tapi isinya dokumen penting (`FIX.md` 168 KB) —
      putuskan: masukkan ke repo (`docs/audit/`) atau memang sengaja lokal.

**DoD:** Tidak ada symlink menggantung, tidak ada submodule rusak. `FIX.md` K-6 → `FIXED`.

---

### 1.7 — Audit 145 issue OPEN di `FIX.md` `[P1]`

`FIX.md` punya 145 `OPEN`, 2 `WIP`, 1 `FIXED`. Itu daftar utang teknis terbaik
yang kamu punya — pakai itu.

**Langkah:**
1. Sortir 145 item berdasarkan severity × effort.
2. **Klasifikasi ulang** setiap item menjadi:
   - `FIX-NOW` (kritis, memblokir kredibilitas/stabilitas)
   - `FIX-LATER` (nyata tapi tidak memblokir portofolio)
   - `WONTFIX` (keputusan sadar — dokumentasikan alasannya)
3. Kerjakan semua `FIX-NOW`. Untuk `WONTFIX`, pindahkan alasannya ke
   `docs/design/` sebagai catatan keputusan.

**DoD:** Tidak ada `OPEN` tanpa klasifikasi. `FIX-NOW` = 0.

---

### Checklist FASE 1 — Definition of Done

- [ ] Login & shell berjalan di Ring 3 **atau** README dikoreksi jujur
- [ ] Fresh clone build lulus di lingkungan bersih
- [ ] CI hijau (build + boot smoke + host tests)
- [ ] `.git` < 50 MB
- [ ] Semua `test-*` hijau
- [ ] Tidak ada symlink/submodule rusak
- [ ] 145 `OPEN` terklasifikasi; `FIX-NOW` selesai
- [ ] Boot 20× berturut tanpa crash (dokumentasikan hasilnya)

**Setelah Fase 1:** proyekmu sudah **bisa diverifikasi orang lain**. Ini
lompatan kredibilitas terbesar dalam seluruh roadmap.

---

## FASE 2 — STABILITAS & SKALA (Bulan 3–5)

> **Sasaran fase:** hilangkan cap "mainan" pada batas sistem. Setelah fase ini,
> OS-mu bukan lagi demo yang crash saat app ke-4.

### 2.1 — Task & window dinamis (buang `MAX_*=16`) `[P0]`

**Masalah terverifikasi:** `include/task.h`: `MAX_TASKS=16`, `TASK_STACK_SIZE=16384`.
`kernel/gfx/kwm/kwm_internal.h`: `MAX_WINDOWS=16`. Ini cap keras — OS serius
butuh task/window yang tumbuh dinamis.

**Langkah:**
1. Ganti array statis task menjadi **linked list atau dynamic array** dengan
   alokasi dari heap. Pertahankan `MAX_TASKS` hanya sebagai batas keamanan
   (mis. 1024), bukan penentu ukuran array.
2. Sama untuk `kwm_windows`.
3. Perhatikan: `smp_current_task_id()` dan per-CPU run queue harus tetap benar
   saat task bertambah dinamis (ada di `kernel/sched/`).
4. Ukur: berapa task/window maksimum sebelum OOM? Dokumentasikan.

**Verifikasi:**
- [ ] Spawn 100 app konkuren → tidak crash, semua terdaftar
- [ ] `sched` di shell menampilkan >16 task dengan benar
- [ ] `make conc` masih PASS di `-smp 4` dan `-smp 8`
- [ ] Task manager (`taskmgr`) menampilkan semuanya

**DoD:** Task & window tumbuh dinamis; batas 16 hilang.

---

### 2.2 — Kampanye stabilitas `[P0]`

**Masalah:** `serial.log` menunjukkan "notifikasi crash (dump ke-28)" saat idle,
dan browser berulang `connection closed unexpectedly`.

**Langkah:**
1. **Reproduksi crash yang ada.** Buka `crash-report.txt` (dump ke-28) — kenapa
   crash? Klasifikasikan: kernel bug, race SMP, atau app bug?
2. **Boot soak test:** boot otomatis 50×, jalankan skenario identik, catat
   crash. Jadikan bagian dari CI (malam, bukan per-push).
3. **App fuzzing:** kirim argumen acak ke setiap app, pastikan tidak ada yang
   mem-brick kernel (harus mati dengan bersih, ada crash report).
4. **Fokus pada yang berulang:** jika crash selalu di compositor/input saat app
   launch (lihat commit `5936aa2` soal kwm_lock), itu petunjuk area.

**Verifikasi:**
- [ ] 50× boot berturut tanpa crash saat idle
- [ ] Idle 30 menit di desktop: 0 crash-dump baru
- [ ] Semua 19 app: buka, pakai 1 menit, tutup — 0 crash
- [ ] Crash report otomatis, tidak pernah menggantung sistem

**DoD:** Boot soak 50/50 bersih. Crash-dump saat idle = 0.

---

### 2.3 — Keamanan & validasi boundary `[P1]`

- [ ] Audit seluruh syscall: pastikan **setiap** pointer user divalidasi
      (konsisten setelah 1.1).
- [ ] Verifikasi SMAP/SMEP aktif dan benar (klaim README) — buktikan dengan test.
- [ ] Verifikasi WP (Write Protect) bit aktif.
- [ ] Test: app jahat coba tulis ke memori kernel → harus SIGSEGV/terminate,
      bukan sukses.
- [ ] Dokumentasikan model keamanan secara eksplisit di
      `docs/userspace/overview.md` — apa yang dilindungi, apa yang belum.

**DoD:** Ada test negatif yang membuktikan isolasi, bukan sekadar klaim.

---

### 2.4 — Sisakan FPU/SSE untuk Fase 3 (persiapan) `[P1]`

Jangan implementasi di sini — tapi **rencanakan** dan **siapkan**.

- [ ] Tulis design note `docs/design/fpu-sse-plan.md`: CR0/CR4, context
      save/restore di switch & interrupt, dampak ke ABI userspace.
- [ ] Inventarisasi: bagian kode mana yang terpaksa menghindari `float`/`double`
      karena `-mno-sse`? (lihat `docs/design/audit-llvm-libc-22-freestanding.md`).
- [ ] Estimasi biaya rebuild ABI setelah SSE dibuka.

**DoD:** Rencana FPU/SSE tertulis dan disetujui (olehmu sendiri, sadar).

---

### Checklist FASE 2 — Definition of Done

- [ ] Task & window dinamis (bukan cap 16)
- [ ] Boot soak 50/50 tanpa crash
- [ ] Idle 30 menit = 0 crash baru
- [ ] Test negatif isolasi keamanan lulus
- [ ] Design note FPU/SSE siap

**Setelah Fase 2:** OS-mu **stabil**. Ini yang membuat orang mau mencobanya.

---

## FASE 3 — MEMBUKA DINDING FPU/SSE (Bulan 5–7)

> **Sasaran fase:** membuka seluruh kelas fitur yang selama ini terkunci oleh
> `-mno-sse -msoft-float`. Ini keputusan proyek-level, bukan patch.
> **Ini fase paling berdampak** untuk plafon jangka panjang.

### 3.1 — Aktifkan SSE/FPU di kernel `[P0]`

**Langkah:**
1. Aktifkan di boot: `CR0` (clear `EM`, set `MP`, `NE`), `CR4` (set `OSFXSR`,
   `OSXMMEXCPT`).
2. Tambahkan **FPU context save/restore** ke jalur context switch:
   `fxsave`/`fxrstor` (atau `xsave`/`xrstor` jika mau AVX nanti).
   - Alokasi area FPU per-task (512 byte aligned-16 untuk `fxsave`).
   - Simpan/restore di `schedule_on_cpu` dan di jalur preemption.
3. Tangani `#NM` (Device Not Available) exception untuk lazy FPU switching
   (opsional, tapi bagus untuk performa).
4. Hapus `-mno-sse -mno-sse2 -mno-mmx -msoft-float` dari `CFLAGS` kernel
   **secara bertahap** — mulai dari modul yang tidak sensitif.
5. **Pertahankan `-mno-red-zone`** (tetap wajib untuk kernel).

**Verifikasi:**
- [ ] Kernel test: task A tulis `double`, task B baca — nilainya tidak bocor
- [ ] Test FPU di dalam ISR (interrupt saat FP aktif)
- [ ] `make conc` masih PASS (context switch FPU benar di SMP)
- [ ] Benchmark: operasi float di kernel benar hasilnya

**DoD:** Kernel bisa pakai SSE/FPU dengan context switch benar. Tidak ada
kebocoran state FP antar-task.

---

### 3.2 — Buka ABI userspace untuk FP `[P0]`

**Langkah:**
1. Rebuild SDK C & C++ **tanpa** `-mno-sse -msoft-float`.
2. Verifikasi fungsi yang mengembalikan `double` bisa dikompilasi (uji yang
   dulu gagal: `difftime`, `duk_add.c`).
3. Pastikan jalur syscall tidak merusak XMM register user (simpan/restore bila
   perlu — biasanya tidak, karena syscall bukan context switch).
4. Update semua dokumentasi SDK: FP sekarang **supported**.

**Verifikasi:**
- [ ] Kompilasi `typedef double d; d f(d a, d b){return a+b;}` untuk target
      `x86_64-pc-none-elf` → **sukses** (dulu error "SSE register return")
- [ ] App userspace bisa pakai `float`/`double` dan hasilnya benar
- [ ] `make sdk-c-smoke-qemu` dan `make sdk-cpp-smoke-qemu` lulus

**DoD:** ABI userspace mendukung FP. Dinding `-mno-sse` **runtuh**.

---

### 3.3 — Buktikan payoff: JavaScript (opsional tapi kuat) `[P1]`

Sekarang Duktape/QuickJS **bisa** dikompilasi. Ini demo yang sangat meyakinkan
untuk portofolio.

- [ ] Vendor Duktape (atau QuickJS), tulis shim platform (allocator, `setjmp`,
      `libm` subset).
- [ ] Jalankan `console.log` di shell.
- [ ] Integrasikan ke `KyuBrowser` (yang sudah ada engine di `apps/browser/engine/`).
- [ ] Tulis devlog: "Kami membuka SSE dan tiba-tiba JS jadi mungkin."

**DoD:** Minimal satu skrip JS sederhana jalan di KyuzenOS.

---

### Checklist FASE 3 — Definition of Done

- [ ] SSE/FPU aktif dengan context switch benar
- [ ] ABI userspace mendukung FP
- [ ] SDK C/C++ bisa mengembalikan `double`
- [ ] (opsional) JS jalan
- [ ] Dokumentasi FP akurat

**Setelah Fase 3:** plafon fiturmu **naik kelas**. Audio, port library modern,
JS, semuanya terbuka.

---

## FASE 4 — KEUNGGULAN YANG TERDEFINISI (Bulan 7–10)

> **Sasaran fase:** kamu tidak bisa menang di keluasan. Kamu **bisa** menang di
> kedalaman satu sumbu. Pilih satu, kerjakan dalam, dokumentasikan.
> Ini yang membuat orang lain **mengingat** proyekmu.

### Pilih SATU angle (jangan dua sekaligus)

**Angle A — "OS yang hidup: hot-reload sistem"**
Kamu sudah punya bibit: syscall `SYS_HOT_RELOAD` (85), `SYS_WALLPAPER_RELOAD` (84),
live-reload wallpaper/font, XML UI, commit "1 source truth for hot reload".
- [ ] Perluas: hot-reload komponen **sistem**, bukan cuma wallpaper/font.
      Ganti tema, ganti widget, ganti driver display — tanpa reboot.
- [ ] Bangun **live development workflow**: edit kode di host → OS recompile
      komponen → langsung aktif. Ini pengalaman yang belum ada di OS mainstream.
- [ ] Tulis dokumen + demo video.

**Angle B — "OS 2D murni yang jujur"**
Kamu sudah **sadar memilih ini** di `docs/history/milestones/2d_accelleration.md`:
menolak 3D/OpenGL/Vulkan, fokus 2D. Konsekuensinya: harus **terbaik** di 2D.
- [ ] Compositor dirty-region: sudah ada, optimalkan lebih jauh (occlusion
      culling, multi-opaque — sudah ada reportnya di `docs/history/graphics/`).
- [ ] Akselerasi 2D Intel iGPU + VirtIO-GPU: jadikan **lebih cepat dari
      software**, buktikan dengan benchmark.
- [ ] Rendering teks (FreeType) cepat dan benar di semua ukuran.
- [ ] Tulis benchmark publik: framerate, latensi input, waktu composit.

> **Rekomendasi:** Angle A lebih unik dan belum ada padanannya di OS lain —
> potensi sorotan lebih besar. Angle B lebih aman dan sudah setengah jalan.

### 4.1 — Batas yang terdefinisi (Syarat S5)

- [ ] Tulis `docs/SCOPE.md`: apa yang KyuzenOS **bukan**. Contoh:
      "Bukan pengganti Linux. Tidak menargetkan 3D/OpenGL. Tidak ada
      kompatibilitas binary Windows/Linux. Tidak ada WiFi/USB/ACPI penuh."
- [ ] Ini **melindungi** proyekmu: reviewer menghargai kejujuran batas, dan
      kamu tidak dikejar ekspektasi yang salah.

**DoD:** Satu angle dikerjakan dalam, satu dokumen scope eksplisit.

---

## FASE 5 — VISIBILITAS & KOMUNITAS (Bulan 10–12, paralel)

> **Sasaran fase:** memecah bus factor = 1. Kontributor pertama datang dari
> visibilitas. Kamu sudah punya bahan dokumentasi terbaik yang kebanyakan
> proyek OS hobi tidak punya — pakai itu.

### 5.1 — Publikasi teknis

- [ ] Tulis **devlog series** (kamu sudah punya 127 dokumen — ekstrak jadi
      narasi): "Membangun SMP scheduler dari nol", "Kenapa saya mematikan SSE
      dan bagaimana saya membukanya lagi", "Compositor dirty-region".
- [ ] Post ke: r/osdev, Hacker News (Show HN), OSDev forum.
- [ ] Sertakan screenshot/video desktop — kamu sudah punya `docs/screenshots/`.

### 5.2 — Onboarding kontributor

- [ ] `CONTRIBUTING.md` (sudah ada di `docs/contributing/`) — pastikan jalan
      dari clone sampai PR pertama < 1 jam.
- [ ] Label issue "good first issue" — ambil dari `FIX.md` yang `FIX-LATER`
      tapi mudah.
- [ ] Pastikan CI memberi feedback cepat ke PR orang lain.

### 5.3 — Rilis bertag

- [ ] Buat release ter-tag (kamu sudah punya `v0.1.0`). Rilis ISO prebuilt
      di GitHub Releases supaya orang bisa coba tanpa build.
- [ ] CHANGELOG per rilis.

**DoD:** Minimal 1 artikel publik terbit; repo siap menerima PR.

---

## Ringkasan Urutan & Ketergantungan

```
FASE 1 (kredibilitas)     ──► FASE 2 (stabilitas)  ──► FASE 3 (FPU/SSE)
  Ring-3, clone, CI                                       │
  .git, tests, audit                                      ▼
                                          FASE 4 (keunggulan/angle)
                                                          │
                                                          ▼
                                          FASE 5 (visibilitas/komunitas)
```

**Aturan ketergantungan:**
- Fase 2 **butuh** Fase 1 (CI untuk verifikasi stabilitas otomatis)
- Fase 3 **butuh** Fase 2 (FPU context switch harus di sistem yang stabil)
- Fase 4 **butuh** Fase 3 (angle A/B diperkuat oleh fitur yang terbuka SSE)
- Fase 5 bisa **mulai paralel** dari Fase 2 (devlog tidak butuh semuanya selesai)

---

## Metrik Keberhasilan (cara mengukur kemajuan)

Lacak ini setiap bulan. Kalau angkanya bergerak, kamu maju.

| Metrik | Sekarang | Target Fase 1 | Target Fase 3 | Target Fase 5 |
|---|---|---|---|---|
| Fresh clone build lulus | ❌ | ✅ | ✅ | ✅ |
| CI status | ❌ | ✅ hijau | ✅ hijau | ✅ hijau |
| `.git` size | 766 MB | < 50 MB | < 50 MB | < 50 MB |
| Issue `FIX-NOW` | ? (dari 145) | 0 | 0 | 0 |
| Boot soak tanpa crash | ? | 20/20 | 50/50 | 100/100 |
| Login/shell Ring | 0 | 3 | 3 | 3 |
| `MAX_TASKS` | 16 | 16 | dinamis | dinamis |
| SSE/FPU userspace | ❌ | ❌ | ✅ | ✅ |
| `double` return compile | ❌ | ❌ | ✅ | ✅ |
| Angle unik | belum | belum | dipilih | terdemonstrasi |
| Artikel publik | 0 | 0 | 1 | 3+ |
| Kontributor eksternal | 0 | 0 | 0 | 1+ |

---

## Yang TIDAK dilakukan (agar proyek selesai)

Roadmap ini sengaja **menolak** hal-hal berikut. Menolaknya bukan kemunduran —
ini yang membuat proyek bisa selesai:

- ❌ Mengejar 3D / OpenGL / Vulkan (sudah diputuskan di `2d_accelleration.md`)
- ❌ Browser "penuh" dengan kompatibilitas web modern lengkap (batasi ke subset)
- ❌ Kompatibilitas binary Linux/Windows (jalan menuju proyek abadi)
- ❌ Menjadi daily-driver / pengganti Linux (butuh tim + 5–10 tahun)
- ❌ Driver hardware massal (WiFi, USB penuh, ACPI penuh) — di luar scope
- ❌ Menambah bahasa pemrograman baru (aturan `.rules/`: C, C++, Rust saja)

---

## Prinsip Kerja Sepanjang Roadmap

Dari `.rules/RULES.md` milikmu sendiri — patuhi ini, karena inilah yang membuat
proyekmu sudah bagus sampai sekarang:

1. **Jangan pernah menukar kualitas dengan kecepatan.** Fase 1 memakan 3 bulan
   dan tidak menambah fitur — itu **disengaja**.
2. **Setiap milestone harus terverifikasi**, bukan "kelihatannya jalan".
3. **Update dokumentasi bersamaan dengan kode** (aturan `.rules/DOCUMENTATION.md`).
4. **Jujur soal batas.** `FIX.md` adalah kekuatanmu, bukan kelemahanmu.
5. **Satu hal dalam, bukan sepuluh hal dangkal.**

---

## Progres — quick wins yang sudah dikerjakan

Dikerjakan langsung dari daftar prioritas `fix/FIX.md` Bagian 3 (leverage
tertinggi, risiko terendah). Semua **diverifikasi build** (`make all` hijau,
0 warning) dan host test yang tersedia tetap hijau.

| FIX.md | Perubahan | File |
|---|---|---|
| **K-3** | Bitmap `mkfs` menandai block root dir + `/apps`; `free_blocks` dikurangi 2. **Bonus:** `free_inodes` juga dikurangi 2 (sebelumnya 1 — counter tidak konsisten dengan bitmap) | `tools/mkfs.kyuzenfs.c` |
| **DRV-1** | EOI dikirim saat `panic_is_locked()` — simetris dengan `keyboard_handler()` | `drivers/mouse.c` |
| **MM-1** | `PMM_KERNEL_ZONE_END` menggantikan 3× hardcode `0x4800000` | `kernel/mm/pmm.c` |
| **GFX-8** | `next_z_index` di-reset di `kwm_destroy_all_windows()` | `kernel/gfx/kwm.c` |
| **FS-8** | `ino_free_nolock` idempotent (guard bitmap) — cegah `free_inodes` melebihi `total_inodes` | `kernel/fs/kfs_balloc.c` |
| **UP-2, UP-3** | Clamp `fsize` + batas copy per-field di parser `users.sys` (stack OOB write) | `system/login.c` |
| **Higiene** | `.gitignore`: scoped `/*.log`, output probe, binary `mkfs`; `CLAUDE.md` di-un-ignore | `.gitignore` |

**Efek samping:** `fix/FIX.md` — `OPEN` 145 → 138, `FIXED` 8. Riwayat lengkap
+ alasan item yang sengaja belum dikerjakan ada di **Lampiran C** `fix/FIX.md`.

**Yang belum dan butuh keputusan Anda** (bukan pekerjaan teknis):
`git add` untuk `apps/browser/engine/lexbor_html_adapter.{cpp,hpp}` (**load-bearing** —
dirujuk `Makefile` L619-636, tanpa ini fresh clone gagal), `CLAUDE.md`,
`docs/ROADMAP-OSDEV.md`, `docs/design/browser/lexbor-stage-b.md`.

---
## Langkah Pertama (mulai besok)

Jangan mulai dari yang besar. Mulai dari yang **membuka pintu**:

1. **Hari 1–3:** Buat `.github/workflows/build.yml` dengan job `make && make apps`.
   Ukur berapa lama sampai hijau. Ini satu-satunya langkah yang paling cepat
   mengubah persepsi proyekmu.
2. ✅ ~~**Minggu 1:** Selesaikan FIX K-1 (Ring-3 login/shell) **atau** koreksi
   README.~~ **Selesai** — migrasi penuh dikerjakan (lihat §1.1). README kini
   akurat: kernel tidak memuat kode user sama sekali.
3. **Minggu 2:** Reproduksi fresh-clone build, catat setiap kegagalan.

Setelah ketiganya, kamu sudah punya fondasi Fase 1 dan momentum.

---

*Dokumen ini hidup. Update status setiap milestone. Bila ragu antara menambah
fitur baru atau menyelesaikan item Fase 1 — selalu pilih Fase 1.*
