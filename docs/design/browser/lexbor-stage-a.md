# Lexbor Stage A — Vendor + Freestanding Build + Smoke Probe

> **Status**: Stage A **selesai** (vendor + build + verifikasi). Ini adalah
> laporan implementasi, bukan audit. Perubahan terbatas pada build system +
> artefak baru; **tidak ada perubahan perilaku browser**.
> **Tanggal**: 2026-09-27.
> **Revisi Lexbor**: **3.0.0** (stable terbaru).
> **Prasyarat**: `audit-kyubrowser-lexbor-duktape.md` (audit kelayakan).
> **Lingkup Stage A**: vendor Lexbor, bangun `liblexbor_kyuzen.a` freestanding,
> port allocator/`memory.h`/int64-conv, host smoke test, probe QEMU userspace.
> **DI LUAR lingkup**: perubahan `apps/browser/engine/*`, Duktape, CSS/selectors,
> perubahan kernel/ABI/FPU, Stage B+.

---

## 1. Ringkasan Hasil

| Item | Hasil |
|---|---|
| Revisi | Lexbor **3.0.0** (`third_party/lexbor/version`, `base.h` = 3.0.0) |
| Modul di-vendor | `core, dom, html, ns, tag` (5 modul) |
| File sumber dikompilasi | **158** objek (156 upstream + 2 port) |
| `liblexbor_kyuzen.a` | **1.498.818 bytes** |
| Simbol eksternal (undefined) | **11**: `malloc/calloc/realloc/free` + `memchr/memcmp/memcpy/memmove/memset/strlen/strncmp` |
| Instruksi SSE/XMM/x87 | **0** (158/158 objek; `llvm-objdump`) |
| Host smoke test | **ALL PASS** (25/25 check) |
| Probe QEMU userspace | **`[lexbor-a] PASS`** (parse → DOM → serialize → malformed → empty) |
| Regresi browser | `test-netutil`, `test-browser-url-http`, `test-browser-html`, `test-browser-css-layout`, `test-tls` = **ALL PASS** |
| Perubahan browser/kernel | **NOL** (engine & kernel tidak disentuh) |

---

## 2. Mengapa 3.0.0 (bukan 2.4.0)

Lexbor **3.0.0 memisahkan HTML dari CSS secara arsitektural** — ini justru
menguntungkan, karena blocker FP audit lama ada di CSS:

- `html/config.cmake` → `DEPENDENCIES "core dom ns tag"` (CSS **tidak** lagi).
- `html/interfaces/document.{h,c}` **tidak lagi** meng-`#include` / init CSS.
- `html/style.c` **sudah tidak ada** di 3.0.0.
- Verifikasi grep: nol referensi `lxb_css_*`/`lxb_selectors_*` di seluruh
  `.c` modul `core/dom/html/ns/tag`.

Konsekuensi: closure HTML/DOM **bersih dari CSS** → tidak ada `double` yang
ditarik, sehingga Stage A tidak perlu menyentuh ABI/FPU sama sekali.

---

## 3. Batas FP (mengapa file tertentu dikeluarkan)

Empat file di `core/` mengimplementasikan konversi IEEE-754 dan **mengembalikan
`double`** — gagal keras di ABI userspace Kyuzen:

```
third_party/lexbor/source/lexbor/core/conv.c:90: error: SSE register return with SSE disabled
```

File yang **dikeluarkan**: `core/{conv,dtoa,strtod,diyfp}.c`.

Namun closure HTML/DOM tetap butuh **tepat satu** simbol dari `conv.c`:
`lexbor_conv_int64_to_data()` (integer murni), yang dipanggil
`core/bst.c` → `lexbor_bst_serialize_entry()` (bookkeeping cache alokasi
`core/mraw.c`). Simbol ini disediakan oleh port
`kyuzen/src/lexbor_conv_shim.c` (salinan verbatim dari upstream).

**Bukti bahwa exclusion ini wajib** — guard di `kyuzen.mk` + `Makefile`:
tanpa `filter-out`, build langsung mati pada `conv.c` dengan error SSE di atas.
Guard `lexbor_check_no_fp` memastikan file FP tidak bisa bocor ke daftar sumber.

---

## 4. Perubahan File

### 4.1 Baru

| Path | Isi |
|---|---|
| `third_party/lexbor/source/lexbor/{core,dom,html,ns,tag}/**` | Sumber Lexbor 3.0.0 (336 file, 2.26 MB) |
| `third_party/lexbor/LICENSE`, `NOTICE`, `version` | Lisensi Apache-2.0 + penanda revisi |
| `third_party/lexbor/kyuzen.mk` | Variabel build (`LEXBOR_KYUZEN_SRCS/CFLAGS/A/INCS/HDRS`) |
| `third_party/lexbor/kyuzen/include/memory.h` | Shim `<memory.h>` → `<string.h>` SDK |
| `third_party/lexbor/kyuzen/src/lexbor_memory.c` | Allocator hooks → libc Kyuzen |
| `third_party/lexbor/kyuzen/src/lexbor_conv_shim.c` | `lexbor_conv_int64_to_data` (integer saja) |
| `tests/host/unit/lexbor_html_test.c` | Host smoke test (25 check) |
| `tools/lexbor-stage-a/lexbor_phase_a.c` | App probe QEMU userspace |
| `tools/lexbor-stage-a/lexbor_app.ld` | Linker script (entry `_start`) |
| `tools/lexbor-stage-a/run-qemu.sh` | Harness QEMU otomatis (pola libc-phase1) |

### 4.2 Diubah

| Path | Perubahan |
|---|---|
| `Makefile` | Blok Lexbor: archive `liblexbor_kyuzen.a`, host test, target `lexbor-stage-a`/`lexbor-stage-a-qemu`, 1 baris copy ELF ke ISO |

**Tidak diubah**: `apps/browser/engine/{html,dom,css,layout}.cpp`,
`apps/browser/page.cpp`, seluruh `kernel/`, `apps/Makefile`, sumber upstream Lexbor.

---

## 5. Port Layer Kyuzen

### 5.1 Allocator (`lexbor_memory.c`)

Rantai sesuai spesifikasi:

```
Lexbor → lexbor_malloc/realloc/calloc/free
       → libc malloc/realloc/calloc/free   (Kyuzen C SDK)
       → sys_alloc / sys_free               (syscall kernel)
```

Mekanisme `lexbor_memory_setup()` dipertahankan verbatim dari port POSIX
upstream sehingga perilaku identik dengan build hosted. Tanpa POSIX/threads/FS.

### 5.2 `memory.h` shim

`core/base.h` meng-`#include <memory.h>` yang **bukan** bagian SDK
freestanding. Tanpa shim, preprocessor diam-diam jatuh ke header libc **host**
(`/usr/include/memory.h`) — pelanggaran aturan "no hosted libc" meski tetap
terkompilasi. Shim memetakannya ke `<string.h>` SDK. Direktori port di-`-I`
**paling awal** agar menang atas host.

### 5.3 `lexbor_assert`

Di `core/base.h`, `#define lexbor_assert(val)` sudah **no-op** — tidak butuh
`assert.h` sama sekali.

---

## 6. Verifikasi

### 6.1 Host smoke test (`make test-lexbor-host`) — ALL PASS

Sumber yang sama dengan arsip freestanding (file FP tetap dikecualikan, shim
int64 dipakai). Mencakup: create/init parser, parse HTML well-formed,
DOM lookup (`by_tag_name` + `by_id`), baca teks + atribut, serialisasi
round-trip, parse input rusak, parse kosong, destroy bersih.

```
serialized: <!DOCTYPE html><html><head><title>Kyuzen</title></head><body>...
ALL PASS
```

### 6.2 Probe QEMU userspace (`make lexbor-stage-a-qemu`) — PASS

App freestanding statis di-link dengan `liblexbor_kyuzen.a` + libc Kyuzen,
dijalankan via `start lexbor_phase_a` di KyuzenOS nyata. Bukti serial COM1:

```
[lexbor-a] start
[lexbor-a] ok parse
[lexbor-a] ok dom-lookup (title text)
[lexbor-a] ok dom-lookup (by_id + attr)
[lexbor-a] ok serialize (139 bytes)
[lexbor-a] ok malformed
[lexbor-a] ok empty
[lexbor-a] PASS
```

Artinya Lexbor **benar-benar berjalan di userspace KyuzenOS** (allocator =
libc Kyuzen → syscall), bukan hanya di host.

### 6.3 Link hygiene

- `llvm-nm --undefined-only` pada arsip → **11 simbol eksternal**, semuanya
  tersedia di `libc.a` Kyuzen. Nol POSIX/pthread/FS/dynamic-load.
- `llvm-objdump -d` pada **158/158 objek** → **0** SSE/XMM, **0** x87.
- ELF app Stage A → `0 undefined`, `0 SSE/x87` (guard di Makefile).

### 6.4 Regresi

Semua test browser existing **tidak berubah dan tetap PASS**:
`test-netutil`, `test-browser-url-http`, `test-browser-html`,
`test-browser-css-layout`, `test-tls`. `browser.elf` tetap ter-link.

---

## 7. Cara Pakai

```sh
make test-lexbor-host       # host smoke test (cepat, tanpa QEMU)
make build/lib/liblexbor_kyuzen.a   # arsip freestanding
make lexbor-stage-a         # ELF probe userspace
make lexbor-stage-a-qemu    # boot QEMU + jalankan probe (butuh qemu, xorriso, mkfs)
```

---

## 8. Batasan & Pekerjaan Lanjutan (Stage B+)

1. **Belum terintegrasi ke browser.** Stage A hanya membangun & memverifikasi
   arsip. Mengganti `html::parse` (Stage B) akan menyentuh
   `apps/browser/engine/html.cpp` — **di luar lingkup ini**.
2. **CSS/selectors belum dibangun.** Tokenizer/parser/selectors Lexbor
   (`css/syntax/state.c`, `selectors/selectors.c`) mengembalikan `double` →
   terblokir FP. Perlu keputusan ABI/FPU (Stage D) atau tetap memakai CSS
   Kyuzen existing.
3. **Encoding lengkap belum di-vendor.** `html/encoding.c` (nama→UTF-8)
   self-contained; modul `encoding/` (11 MB, 28 dekoder) belum diperlukan untuk
   closure ini. Ditambahkan saat butuh konversi charset nyata.
4. **Duktape tetap tidak layak** tanpa perubahan kernel (lihat audit).
5. **`-DLEXBOR_STATIC`**: menghindari dekorasi `dllexport/import` saat
   preprocessing di host Windows; harmless di ELF. Bukan patch source.
