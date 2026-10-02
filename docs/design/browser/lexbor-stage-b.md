# Lexbor Stage B — HTML Parser Integration (Production `html::parse()`)

> **Catatan (pasca-migrasi CMake).** Dokumen ini ditulis saat proyek masih
> dibangun dengan Makefile, jadi perintah `make ...` di dalamnya merujuk build
> lama. Makefile sudah dihapus; padanan CMake-nya ada di
> [Building](../../development/building.md). Hasil verifikasi yang tercatat di sini
> sengaja tidak diubah — itu catatan apa yang benar-benar dijalankan saat itu.


> **Status**: Stage B **selesai** (integrasi + verifikasi).
> **Tanggal**: 2026-09-27.
> **Revisi Lexbor**: **3.0.0** (stable terbaru).
> **Prasyarat**: `lexbor-stage-a.md` (vendor + arsip freestanding, sumber kebenaran
> untuk build/port layer Lexbor).
> **Lingkup Stage B**: jadikan Lexbor 3.0.0 parser HTML **produksi** di balik
> `html::parse()` yang sudah ada, lewat adapter khusus. DOM KyuBrowser, CSS,
> layout, renderer, dan API publik **tidak berubah**.
> **DI LUAR lingkup**: migrasi DOM native Lexbor, CSS/selectors Lexbor, Duktape,
> perubahan kernel/ABI/FPU, Stage C+.

> **Stage B mengintegrasikan Lexbor sebagai parser HTML produksi tetapi
> dengan sengaja mempertahankan representasi DOM KyuBrowser yang sudah ada.**

---

## 1. Tujuan

Mengganti implementasi parser HTML tulis-tangan dengan HTML parser/tree builder
Lexbor, tanpa mendesain ulang engine browser. Satu jalur parser produksi,
tanpa flag runtime `USE_LEXBOR`/`USE_OLD_HTML`.

---

## 2. Arsitektur Parser: Sebelum vs Sesudah

**Sebelum (Stage A dan sebelumnya):** `html::parse()` menjalankan parser
tulis-tangan (10 aturan recovery terdokumentasi di `html.hpp`) yang langsung
membangun `dom::Document`.

**Sesudah (Stage B):**

```text
HTML bytes
    │
    ▼
html::parse()                       apps/browser/engine/html.cpp
    │  (cap MAX_DOC_BYTES)
    ▼
parse_via_lexbor()                  apps/browser/engine/lexbor_html_adapter.cpp
    │
    ▼
Lexbor HTML parser/tree builder     liblexbor_kyuzen.a (Stage A)
    │
    ▼
Lexbor DOM (sementara)
    │
    ▼
Adapter (tree conversion)           ← HANYA salin data, bukan parser
    │
    ▼
dom::Document KyuBrowser            apps/browser/engine/dom.hpp
    │
    ├── css::parse_stylesheet / compute_style
    ├── layout::build_layout
    └── renderer (apps/browser/browser_app.cpp)
```

**Tanggung jawab Lexbor**: tokenisasi HTML, tree construction, recovery HTML
rusak, aturan nesting, decoding entity, pembuatan DOM sementara.

**Tanggung jawab KyuBrowser**: tipe DOM existing, parsing CSS, cascade/computed
style, layout, painting, perilaku dokumen khas browser.

Pemisahan ini ditegakkan oleh header: `dom.hpp` tetap **bebas Lexbor**, dan
`lexbor_html_adapter.hpp` (dipakai engine) **tidak menyertakan** header Lexbor.
Hanya `lexbor_html_adapter.cpp` yang tahu Lexbor.

---

## 3. File

### 3.1 Baru

| Path | Isi |
|---|---|
| `apps/browser/engine/lexbor_html_adapter.hpp` | API adapter (bebas Lexbor): `bool parse_via_lexbor(const std::string&, dom::Document&)` |
| `apps/browser/engine/lexbor_html_adapter.cpp` | Siklus hidup Lexbor + konversi tree → DOM KyuBrowser |

### 3.2 Diubah

| Path | Perubahan |
|---|---|
| `apps/browser/engine/html.cpp` | `html::parse()` kini memanggil adapter; parser tulis-tangan (≈390 baris) **dihapus**. Helper `dom::find_first/find_all/inner_text` **dipertahankan** |
| `apps/browser/engine/html.hpp` | Komentar aturan recovery tulis-tangan diganti deskripsi kebijakan dokumen Stage B |
| `tests/host/unit/browser_html_test.cpp` | +7 test adapter Stage B |
| `Makefile` | `BW_SYS_INC` + include Lexbor; `$(BW_ELF)` link `$(LEXBOR_KYUZEN_A)`; arsip Lexbor host + rule test browser dipindah ke bawah blok Lexbor |

### 3.3 Dihapus

Tidak ada file dihapus. Kode parser tulis-tangan **di dalam** `html.cpp`
dihapus seluruhnya (bukan file terpisah).

---

## 4. Pemetaan DOM: Lexbor → KyuBrowser

`dom::NodeType { Document, Element, Text }` (tidak ada Comment). Aturan konversi:

| Lexbor | KyuBrowser | Catatan |
|---|---|---|
| `lxb_dom_document_t` | `Document::root` (`NodeType::Document`) | selalu ada; `html/head/body` disintesis Lexbor dan tetap tampil sebagai elemen biasa |
| `LXB_DOM_NODE_TYPE_ELEMENT` | `NodeType::Element` | `tag` dari `lxb_dom_element_qualified_name` (Lexbor sudah lowercase untuk HTML) |
| `LXB_DOM_NODE_TYPE_TEXT` | `NodeType::Text` | byte UTF-8 disalin verbatim; text bersebelahan digabung (node lebih sedikit, semantik sama) |
| Atribut (`lxb_dom_attr_t`) | `dom::Attribute {name,value}` | nama sudah lowercase; urutan sumber dipertahankan; boolean attr (nilai `NULL`) → `""`; duplikat: Lexbor first-wins |
| `LXB_DOM_NODE_TYPE_COMMENT` | **dibuang** | tidak ada tipe Comment di DOM KyuBrowser |
| `LXB_DOM_NODE_TYPE_DOCUMENT_TYPE` | **dibuang** | idem |
| PI / node lain | **dibuang** | idem |

### 4.1 Kebijakan dokumen khas browser (bukan logika parser)

Diterapkan **di atas** tree Lexbor, di adapter:

- **`<script>`/`<style>` tanpa text child.** Body `<script>` dibuang (tanpa JS);
  body `<style>` dikumpulkan **verbatim** (termasuk `<` mentah) ke
  `Document::styles` sesuai urutan dokumen. Ini mempertahankan model
  `doc.styles` yang dipakai `css::parse_stylesheet`.
- **`<title>`** (pertama) → `Document::title`, di-trim. Diambil lewat DOM-walk
  KyuBrowser (bukan `lxb_html_document_title`) agar konsisten dengan model lama.
- **`<link rel=stylesheet href>`** → `Document::style_hrefs` (rel
  case-insensitive, href non-empty, urutan dokumen).
- **Normalisasi `&nbsp;`**: U+00A0 (UTF-8 `C2 A0`) → spasi ASCII, pada text dan
  nilai atribut. **Kebijakan teks browser**, bukan logika parser: parser lama
  memetakan `&nbsp;` → `' '`, dan model teks KyuBrowser serta test existing
  mengasumsikannya. (Decoding entity tetap milik Lexbor; hanya titik kode ini
  yang dinormalisasi.)
- **Cap sumber daya**: `MAX_NODES` (4096) dan `MAX_DEPTH` (128) ditegakkan
  selama konversi. Melebihi salah satunya → `Document::truncated = true`.
  Di kedalaman cap, elemen lebih dalam **diratakan**: seluruh text descendant
  ditempel ke node terdalam yang diizinkan, sehingga text tidak hilang
  (kontrak sama dengan cap parser lama).

### 4.2 Contoh konversi (empiris, host probe)

```html
<div><p>Hello <b>world</b></p></div>
```
→ `div` → `p` → [Text "Hello ", Element `b` → Text "world"]; `parent` benar
di setiap node; `inner_text` = `"Hello world"`.

```html
<p>Hello<div>World
```
→ Lexbor menutup `<p>` sebelum `<div>` (recovery spec); adapter menyalin apa
adanya: `p`→"Hello", `div`→"World". Tidak ada kode perbaikan nesting di adapter.

---

## 5. Kepemilikan / Lifetime

**Lifetime Lexbor** (di dalam `parse_via_lexbor`, satu fungsi):

```text
lxb_html_parser_create → lxb_html_parser_init → lxb_html_parse
   → convert (salin) → lxb_html_document_destroy → lxb_html_parser_destroy
```

**Lifetime KyuBrowser**: `dom::Document` yang keluar dari adapter **sepenuhnya
dimiliki `std::string`/`std::unique_ptr` KyuBrowser**.

**Strategi salin** — setiap field disalin; tidak ada pointer Lexbor yang lolos:

| Field | Sumber Lexbor | Tujuan |
|---|---|---|
| tag | `lxb_dom_element_qualified_name` (`lexbor_hash_entry`) | `std::string` |
| text | `text->char_data.data` (`lexbor_str_t`) | `std::string` |
| nama attr | `lxb_dom_attr_qualified_name` | `std::string` |
| nilai attr | `lxb_dom_attr_value` | `std::string` |

Tidak ada komentar "ownership non-obvious": semua salinan eksplisit. Setelah
`lxb_html_document_destroy`, DOM KyuBrowser tetap valid — dibuktikan oleh test
yang memakai DOM setelah `parse()` kembali.

---

## 6. Penanganan Error

| Kasus | Perilaku |
|---|---|
| Input kosong | Lexbor tetap membangun `html/head/body` kosong; `Document` valid, `truncated=false` |
| HTML rusak | **Bukan error fatal**: Lexbor membangun tree via recovery; adapter menyalin normal |
| `lxb_html_parser_create` gagal | kembalikan `false`; `out.root` tetap Document kosong yang valid |
| `lxb_html_parser_init` gagal | destroy parser, kembalikan `false` |
| `lxb_html_parse` gagal (alokasi) | destroy parser, kembalikan `false`; `out.root` tetap valid |
| `MAX_DOC_BYTES` lewat | `html::parse()` memotong input sebelum Lexbor, set `truncated=true` |

`html::parse()` tetap `void` (kontrak lama: sukses struktural, tidak crash).
Tidak ada sistem error browser baru.

---

## 7. Test

### 7.1 Test adapter Stage B (baru, `browser_html_test.cpp`)

| Test | Cakupan |
|---|---|
| `b_lexbor_basic_document` | struktur document/title/body/p + parent-child |
| `b_lexbor_attributes` | tag, id, class, atribut arbitrer, nilai, text |
| `b_lexbor_nesting` | urutan child eksak + relasi parent/child |
| `b_lexbor_entities` | entity bernama + numerik → UTF-8 sesuai ekspektasi browser |
| `b_lexbor_malformed` | `<p>Hello<div>World` → tree Lexbor, text utuh, tanpa crash |
| `b_lexbor_empty` | kontrak `html::parse()` untuk input kosong |
| `b_lexbor_large_bounded` | 400 `<p>` + atribut (deteksi perilaku kuadratik) |

### 7.2 Hasil (semua hijau)

| Perintah | Hasil |
|---|---|
| `make test-browser-html` | **ALL PASS** (23 test: 16 lama + 7 baru) |
| `make test-browser-css-layout` | **ALL PASS** |
| `make test-browser-url-http` | **ALL PASS** |
| `make test-netutil` | **ALL PASS** |
| `make test-tls` | **ALL TLS TESTS PASS** |
| `make test-lexbor-host` | **ALL PASS** (25/25) |
| `make lexbor-stage-a-qemu` | **`[lexbor-a] PASS`** |
| `tests/host/probes/_browser_probe.py` | **21/21 PASS, fails=0** |

Test existing yang sebelumnya dianggap berisiko (`t_entities`,
`t_script_style`, `t_nesting_cap`) **lulus tanpa perubahan** — berkat kebijakan
dokumen §4.1 (normalisasi NBSP, drop text script, kumpulkan style, cap+flatten).

---

## 8. Build Integration

- **Tidak ada sistem build Lexbor kedua.** Reuse `third_party/lexbor/kyuzen.mk`
  dan arsip Stage A `build/lib/liblexbor_kyuzen.a` (1.498.818 bytes).
- **Browser ELF**: `BW_SYS_INC` ditambah
  `-Ithird_party/lexbor/kyuzen/include -Ithird_party/lexbor/source -DLEXBOR_STATIC`;
  `$(BW_ELF)` men-depend & link `$(LEXBOR_KYUZEN_A)`.
- **Test host browser**: host = Windows PE (mingw), jadi arsip freestanding ELF
  tidak bisa di-link ke test host. Test `#include html.cpp` (amalgamasi), maka
  rule `test-browser-html`/`test-browser-css-layout` mengompilasi adapter (C++)
  dan men-link **arsip Lexbor host** yang dibangun dari sumber yang **sama**
  (`LEXBOR_HOST_SRCS`, C, `HOSTCC`, file FP tetap dikecualikan, shim int64
  dipakai). Arsip host: `build/lexbor-host/liblexbor_host.a`.
- **Rebuild bersih terbukti**: setelah menghapus arsip + objek browser + test,
  `make browser-app` mereproduksi `liblexbor_kyuzen.a` **tepat 1.498.818 bytes**
  dan `browser.elf` (1.770.600 bytes).

---

## 9. Higiene Link / ABI (browser.elf)

| Pemeriksaan | Hasil |
|---|---|
| `llvm-nm --undefined-only browser.elf` | **0** simbol undefined |
| SSE/XMM/YMM/ZMM (`llvm-objdump -d`) | **0** |
| x87 (`fld/fst/fxch/fucom/fadd/fmul/fdiv/fsub/fild/fist/fcom`) | **0** |
| MMX (`%mm0-7`) | **0** |
| libc hosted (glibc `__libc_*`, `_IO_*`, `__gmon_start__`) | **0** |
| POSIX/pthread/dlopen/dlsym | **0** |
| Simbol Lexbor di ELF | ada (`lxb_html_parse`, `lxb_dom_element_qualified_name`, `lexbor_malloc`, …) |
| Simbol adapter di ELF | ada (`browser::html::parse_via_lexbor(...)`) |
| Satu-satunya simbol "runtime" C++ | `__cxa_atexit` (dari libcxxrt SDK, sudah ada sejak sebelum Stage B) |

Kehadiran simbol Lexbor + adapter di `browser.elf` membuktikan jalur parse
produksi benar-benar memakai Lexbor (bukan fallback).

---

## 10. Runtime QEMU (browser)

`_browser_probe.py` (21/21 PASS) memverifikasi end-to-end di KyuzenOS nyata:
muat fixture, parse via Lexbor, cascade CSS (link merah #EE0000 + h1 biru
#0033AA dari CSS eksternal), layout, render, klik link, Back/Reload, scroll
drag, panel error (koneksi/DNS), HTTPS nyata (example.com, info.cern.ch),
google.com 91 KB (regresi `parse_px`), redirect loop, tanpa panic.

---

## 11. Kinerja / Alokasi

Input → Lexbor DOM → DOM KyuBrowser = **duplikasi sementara** (dapat diterima
untuk Stage B; integrasi zero-copy = pekerjaan lanjut). Tidak ada arena/pooling/
refcount/allocator baru/thread. Alokator Lexbor = libc Kyuzen (Stage A, tak
berubah). Test `b_lexbor_large_bounded` (400 paragraf) tidak menunjukkan
perilaku kuadratik.

---

## 12. Batasan Diketahui

1. **Representasi ganda sementara** (Lexbor + KyuBrowser) — boros memori,
   disengaja untuk Stage B.
2. **Text `<script>` dibuang** (tanpa JS) dan **`<style>` tanpa text child**
   (dikumpulkan ke `doc.styles`) — kebijakan browser, bukan semantik DOM penuh.
3. **`<head>`/`<body>` sintetis Lexbor selalu ada** — mengubah bentuk tree relatif
   terhadap parser lama; konsumen (`find_first`/`find_all`, `layout` memilih
   `body`) tetap kompatibel dan test lulus.
4. **NBSP → spasi** (normalisasi sengaja; lihat §4.1).
5. **Batas `MAX_DOC_BYTES` dipotong sebelum parse** — dapat memotong di tengah
   tag; perilaku sama dengan sebelumnya (truncation aman, `truncated=true`).
6. **Encoding/charset non-UTF-8** belum dikonversi (modul `encoding/` belum
   di-vendor; sama seperti Stage A).

---

## 13. Yang Tersisa untuk Stage C

- **Migrasi DOM native Lexbor** (atau integrasi lebih dalam) untuk menghapus
  duplikasi representasi.
- **CSS/selectors Lexbor** — terblokir FP (butuh keputusan ABI/FPU).
- **Duktape/JS** — butuh perubahan kernel (lihat audit).
- Konversi charset (`encoding/`).

---

## 14. Cara Pakai

```sh
make test-browser-html         # test HTML + adapter Stage B (butuh arsip host Lexbor)
make test-browser-css-layout   # CSS + layout + hit test
make test-lexbor-host          # smoke test Lexbor Stage A (25/25)
make browser-app               # link browser.elf dengan liblexbor_kyuzen.a
make lexbor-stage-a-qemu       # probe userspace Lexbor (Stage A)
python tests/host/probes/_browser_probe.py   # runtime browser di QEMU (21/21)
```
