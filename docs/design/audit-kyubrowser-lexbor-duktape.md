# Audit: Lexbor + Duktape Integration untuk KyuBrowser (KyuzenOS)

> **Catatan (pasca-migrasi CMake).** Dokumen ini ditulis saat proyek masih
> dibangun dengan Makefile, jadi perintah `make ...` di dalamnya merujuk build
> lama. Makefile sudah dihapus; padanan CMake-nya ada di
> [Building](../development/building.md). Hasil verifikasi yang tercatat di sini
> sengaja tidak diubah — itu catatan apa yang benar-benar dijalankan saat itu.


> **Status**: audit only — **tidak ada kode yang diubah, tidak ada dependency yang
> di-vendor, tidak ada implementasi**. Dokumen ini murni `audit → understand →
> document → recommend`.
> **Tanggal**: 2026-09-27.
> **Lingkup**: menilai kelayakan mengganti parser HTML/CSS/DOM/JS KyuBrowser
> (`apps/browser/engine/`) dengan **Lexbor** (HTML/DOM/CSS/selectors) dan
> **Duktape** (JavaScript), di dalam constraint freestanding/bare-metal KyuzenOS.
> **Basis bukti**: seluruh pernyataan tentang KyuzenOS **diverifikasi langsung**
> dari source repo. Pernyataan tentang Lexbor/Duktape bersifat **pengetahuan
> upstream** (kedua library TIDAK ada di repo dan TIDAK boleh di-vendor untuk
> audit ini) — ditandai eksplisit `[UPSTREAM]`. Semua kesimpulan yang bergantung
> pada FP ABI **direproduksi empiris** dengan toolchain repo.

---

## 1. Executive Summary

**Verdict singkat**

| Pertanyaan | Jawaban |
|---|---|
| Lexbor menggantikan parser HTML saat ini? | **Ya** — realistis, pure userspace, tanpa perubahan kernel. |
| Lexbor CSS/DOM dipakai independen? | **Ya, dengan catatan** — DOM + selectors + CSS tokenizer/parser bisa; **CSS *value* numerik Lexbor memakai `double`** → kena blocker FP (lihat §4/§13). |
| Duktape menyediakan eksekusi JS? | **TIDAK tanpa perubahan kernel.** Blocker keras & terverifikasi: `double` return ABI tidak bisa dikompilasi di `x86_64-pc-none-elf` dengan `-mno-sse -msoft-float`. |
| Shims platform? | Lexbor: allocator + assert (kecil). Duktape: allocator + `setjmp/longjmp` + **libm** + **FP ABI** (besar → tidak layak). |
| Bisa sepenuhnya userspace? | **Lexbor: ya.** **Duktape: tidak** — butuh kernel mengaktifkan SSE/FPU. |
| Yang harus Kyuzen implement sendiri? | Cascade, computed style, layout, paint, resource dispatcher, event loop, DOM↔JS binding, timer queue. Lexbor/Duktape tidak menyediakan ini. |
| Blocker teknis terbesar? | **FP ABI x86_64** (memblok Duktape penuh & sebagian Lexbor CSS values). |
| Milestone pertama? | Vendor Lexbor → bangun **HTML tokenizer + tree builder + DOM** saja, di balik interface `html::parse` yang sudah ada, diverifikasi `make test-browser-html` + probe QEMU. |

**Temuan paling penting (terverifikasi empiris, §12.3):**

```
$ clang --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2 \
    -c duk_add.c
duk_add.c:1:43: error: SSE register return with SSE disabled
  typedef double duk_double_t; duk_double_t duk_add(duk_double_t a, duk_double_t b){ return a+b; }
                              ^
```

ABI `x86_64-pc-none-elf` mengembalikan `float`/`double` di `%xmm0`. Dengan SSE
dimatikan, clang **tidak bisa men-generate fungsi apa pun yang mengembalikan
`double`**. Duktape dibangun seluruhnya di atas `duk_double_t = double`
(ribuan call site). Karena itu **Duktape tidak bisa dikompilasi** untuk target
KyuzenOS dalam konfigurasi toolchain saat ini. Ini bukan masalah performa atau
porting — ini masalah *compile-time*.

Audit LLVM libc milik proyek sendiri **sudah mencatat fenomena yang sama**
(`docs/design/audit-llvm-libc-22-freestanding.md`, baris 650):

> `difftime` | DEFER | return `double` — pemanggil `-mno-sse -msoft-float` gagal
> kompilasi (`SSE register return with SSE disabled`, dibuktikan via `clang -S`)

Dengan kata lain: keputusan "tanpa SSE" yang menjadi fondasi seluruh ABI
KyuzenOS **secara struktural tidak kompatibel dengan mesin JS berbasis IEEE-754
double apa pun** (Duktape, QuickJS, MuJS). JS hanya bisa dihidupkan bila salah
satu dari tiga hal terjadi: (a) kernel mengaktifkan SSE/FPU + context save
(perubahan kernel, §13), atau (b) mesin JS integer-only custom ditulis dari nol,
atau (c) ABI FP non-standar (pass-by-pointer) — tidak ada mesin mainstream yang
mendukung ini.

**Rekomendasi**: lanjutkan **Lexbor** sebagai pengganti HTML/CSS/DOM (fase
terpisah, §15), dan **tunda Duktape** di balik keputusan eksplisit tentang
FPU/SSE kernel. Jangan mencampur keduanya dalam satu milestone.

---

## 2. Current Browser Architecture

### 2.1 Lokasi & entry point (VERIFIED)

```
apps/browser/
├── main.cpp            (11)   entry `extern "C" int main` → BrowserApp::run → sys_exit()
├── browser_app.cpp/.hpp (438/74) chrome XML + navigasi + viewport render
├── page.cpp/.hpp        (177/64) pipeline: fetch → content-type → parse → css → images → layout
├── font.cpp/.hpp        (85/31)  FreeType+Inter adapter (layout::TextMeasure)
├── transport.cpp/.hpp   (53/37)  KyuzenTransport (ksock)
├── transport_tls.cpp    (81)    TlsTransport (BearSSL)
├── platform.hpp         (23)    satu-satunya tempat menyentuh header C platform
├── tls/tls_kyuzen.c/.h  (313/46) driver BearSSL (C murni, tanpa malloc)
└── engine/
    ├── url.cpp/.hpp     (200/40)
    ├── http.cpp/.hpp    (279/54)
    ├── html.cpp/.hpp    (403/31)  parser HTML subset
    ├── dom.hpp          (45)      DOM ringan RAII
    ├── css.cpp/.hpp     (468/63)  parser CSS subset + cascade
    ├── layout.cpp/.hpp  (377/64)  layout block+inline
    └── strutil.hpp      (29)
Total: ~3127 LOC C++17 (+313 LOC C TLS).
```

Entry chain (VERIFIED dari source):

```
main.cpp
 └─ BrowserApp::run()                         browser_app.cpp:114
     ├─ font_.load()                          font.cpp:27
     ├─ build_chrome()                        browser_app.cpp:61
     │   ├─ ui_window_create / ui_xml_parse / ui_xml_inflate   (libui XML)
     │   └─ ui_scrollview_create + ui_fttext_create            (native widget)
     └─ ui_window_run(win_)                   libui_abi.cpp:69 → Window::run()
         └─ loop { sys_get_event(); tick_cb(); render(); }      window.hpp
```

### 2.2 Trace alur aktual URL → paint (VERIFIED)

```
URL (address bar / klik link)
 │  navigate_text()                          browser_app.cpp:135
 │    └─ parse_url()                          url.cpp:parse_url
 │    └─ navigate_url()                       browser_app.cpp:157
 ▼
Page::load(url, font, viewport_w, detail)    page.cpp:86
 │  ├─ KyuzenTransport / TlsTransport        transport.cpp:10 / transport_tls.cpp:15
 │  │    └─ net_resolve()  → sys_resolve      syscall 86
 │  │    └─ sys_socket/connect/send/recv      ksock
 │  │    └─ (https) tls_connect() → BearSSL   tls_kyuzen.c:162
 │  ├─ http::fetch(tr, url, r)                http.cpp:197   (redirect ≤10, chunked, CL)
 │  ├─ content_is_html(r)                     page.cpp:15    (gate content-type)
 │  ├─ html::parse(r.body, doc)               html.cpp:265
 │  ├─ css::parse_stylesheet(...)             css.cpp:328    (<style> + <link rel=stylesheet>)
 │  ├─ resolve+fetch gambar EAGER             page.cpp:139-170 (image_decode_memory)
 │  └─ layout::build_layout(doc, sheets, ...) layout.cpp:372
 ▼
commit atomik → BrowserApp::show_page()      browser_app.cpp:192
 │  └─ ui_fttext_refresh() → drawView()       browser_app.cpp:408
 │       ├─ fill_rect / blend_px (bg, box bg, hr)
 │       ├─ font_.draw() → kz_text_draw       font.cpp:81
 │       └─ blit_run() → image pixels          browser_app.cpp:376
 ▼
ScrollView (wheel/scrollbar/drag) → canvas → KWM
```

### 2.3 Batas arsitektur (dokumentasi yang sudah ada)

`docs/design/browser/browser.md` §87 menetapkan pemisahan yang penting untuk
audit ini:

> Boundaries: XML != HTML, libui Widget != DOM, HTTP != document,
> DOM != widget tree, browser layout != libui layout, renderer != compositor.

Artinya: **DOM engine (Lexbor nanti) dan widget tree (libui) adalah dua dunia
terpisah**, dan browser layout (engine) ≠ libui layout (toolkit). Ini
menguntungkan migrasi Lexbor: DOM Lexbor menggantikan `dom::Document` saja, tidak
menyentuh libui.

### 2.4 Komponen platform yang dipakai (VERIFIED)

| Kebutuhan | Mekanisme saat ini | File |
|---|---|---|
| Networking | `sys_socket/connect/send/recv/sock_close` + `net_resolve` | transport.cpp, transport_tls.cpp |
| DNS | `sys_resolve` (syscall 86) | include/userlib.h |
| TLS | BearSSL 0.6 subset (TLS1.2, ECDHE+AES-128-GCM) | tls/tls_kyuzen.c |
| Entropi | `sys_entropy` (syscall 87, RDRAND) | kernel/entropy.c |
| Waktu | `sys_get_time` (RTC WIB→UTC) | tls_kyuzen.c:102 |
| Filesystem | `sys_file_size`, `sys_read_file_to_buffer` | font.cpp:29-33 |
| Memori | `sys_alloc/sys_free/sys_realloc`; libc `malloc` = arena FreeListHeap | font.cpp:10-16 |
| Grafis | FtText (canvas mentah) + `kz_text_draw`; `image_decode_memory` | browser_app.cpp, libs/text, libs/media |
| Font | FreeType + Inter (`kzfont.h`) | font.cpp |
| Event | `sys_get_event` (syscall 29); `ui_tick_cb` per iterasi | window.hpp run() |
| Timer | **tidak ada timer infra** — hanya `sys_uptime()` ms + PIT IRQ; loop bangun ~60/s via `sys_yield` | window.hpp |
| Syscall | `int 0x80`, RAX=num; #9/#10 heap, #34 exit, #14 uptime, #20 RTC, #46 sleep, #48/#49 stdio, #86 resolve, #87 entropy | include/userlib.h |

**Kyuframe (VERIFIED: TIDAK ADA).** Pencarian seluruh repo (di luar
`third_party/`) untuk `Kyuframe`/`kyuframe`/`kframe` = **0 hasil**. Arsitektur
UI yang benar-benar ada adalah:
- `libs/gui/libdesktop/` (libdesktop — desktop shell)
- `libs/gui/widget/` (toolkit C++ libui; ABI C di `abi/libui_abi.cpp`)
- `include/libui.h`, `include/libui_xml.h` (ABI C + XML chrome)

Diagram §13 pada permintaan menyebut "UI └── Kyuframe" — **nama itu tidak ada di
repo**; padanannya adalah **libui (toolkit widget) + libdesktop**. Audit ini
memakai nama yang benar.

**GHAL (VERIFIED: ADA, tapi tidak dipakai browser).** `graphics/ghal.h`
(Graphics HAL kernel: `ghal_surface_t`, `present`, `fill_rect`, `blit`, vtable
backend) ada di sisi **kernel**. Aplikasi browser ring-3 **tidak** menyentuhnya
langsung — ia memakai libui/KWM. Jadi GHAL bukan bagian dari jalur integrasi
Lexbor/Duktape di userspace.

---

## 3. Current HTML / CSS / JS Limitations

Legenda: ✅ implemented · 🟡 partial · ⬜ stub · ❌ unsupported

### 3.1 HTML (VERIFIED dari `html.hpp` + `html.cpp`)

| Area | Status | Bukti |
|---|---|---|
| Tokenizer | ✅ hand-written, `<`-scan | html.cpp:276-396 |
| Tree construction | ✅ stack-based | Builder, html.cpp:121-198 |
| Spec-compliant error recovery | ❌ (10 aturan ad-hoc terdokumentasi) | html.hpp:8-21 |
| Malformed HTML | 🟡 deterministik, tidak crash | html.hpp recovery rules |
| Elements | 🟡 ~30 tag (`html head body title meta div span p h1-h6 br hr strong em b i u a img ul ol li pre code blockquote button input`) | html.hpp:4-5 |
| Atribut | ✅ lowercase + dup-first-wins + cap 32 | html.cpp:245-259 |
| Entities | 🟡 6 named + nbsp + numerik (des/hex) | html.cpp:55-106 |
| Forms | 🟡 dirender, tidak disubmit | browser.md §16 |
| Tables | ❌ tidak ada model tabel | — |
| Scripts | ❌ `<script>` body **DIBUANG** (tidak ada JS) | html.cpp:337-362 |
| Stylesheets | 🟡 `<style>` dikumpulkan; `<link rel=stylesheet>` di-fetch | html.cpp:363-388, page.cpp:117-131 |
| Embedded resources | 🟡 hanya `<img src>` (eager fetch) | page.cpp:139-170 |
| Encoding | ❌ byte-oriented (UTF-8 diasumsikan, tanpa sniffing) | html.cpp |
| Caps | node 4096 / depth 128 / attr 32 / doc 512KB | dom.hpp:12-14, html.hpp:31 |

### 3.2 CSS (VERIFIED dari `css.hpp` + `css.cpp`)

| Area | Status | Bukti |
|---|---|---|
| Tokenizer | ❌ tidak ada tokenizer CSS; parser ad-hoc `find('{')/find('}')/find(';')/find(':')` | css.cpp:328-379 |
| Parser rules | 🟡 `sel { decls }`, komentar `/* */` di-strip | css.cpp:24-39, 328 |
| Selectors | ❌ **hanya** tag / `.class` / `#id` (+ grup koma). Kombinator/descendant/pseudo = **seluruh rule dibuang** | css.cpp:41-55, 349-352 |
| Specificity | 🟡 id(3)>class(2)>tag(1) + order; inline=4 | css.cpp:278-282, 413 |
| Cascade | ✅ UA < author < inline; invalid value tidak menggeser prioritas | css.cpp:94-116 |
| Inheritance | 🟡 color/font-size/bold/italic/align inherit; bg tidak | css.cpp:388-395 |
| Computed values | 🟡 `ComputedStyle` (12 field) | css.hpp:49-64 |
| Box model | 🟡 margin (1-4 Npx, aditif tanpa collapse) + padding tunggal | css.cpp:164-202 |
| Display | 🟡 block/inline/none/list-item (list-item→block) | css.cpp:203-211 |
| Positioning | ❌ | — |
| Flexbox / Grid | ❌ | — |
| Media queries | ❌ | — |
| Pseudo-classes/elements | ❌ (membatalkan rule) | css.cpp:41-55 |
| Units | ❌ hanya `Npx` (0..4096) | css.cpp:421-440 |
| Colors | 🟡 `#RGB`/`#RRGGBB` + 8 nama + transparent | css.cpp:442-484 |
| Fonts | 🟡 font-size Npx, bold, italic (italic = tegak) | css.cpp:133-154 |
| Animasi/transisi | ❌ | — |

### 3.3 JavaScript

| Area | Status |
|---|---|
| Engine/interpreter | ❌ **tidak ada sama sekali** |
| ECMAScript | ❌ |
| Parser / bytecode / VM | ❌ |
| GC | ❌ |
| DOM bindings | ❌ |
| Web APIs | ❌ |
| Events | 🟡 hanya event UI native (klik link); tanpa event DOM |
| Timers | 🟡 hanya `ui_tick_cb` (per-iterasi, bukan `setTimeout`) |
| fetch/XHR | ❌ |
| Promises/async | ❌ |
| Storage/cookies | ❌ (cookie diabaikan diam-diam, browser.md §16) |

`<script>` body dibuang saat parse (html.cpp:337-362); roadmap browser menaruh JS
"eksplisit di luar scope sampai fase mesin-JS diaudit (§51)"
(`browser-roadmap.md:19`). **Audit ini adalah fase itu.**

---

## 4. Lexbor Compatibility Audit

> `[UPSTREAM]` — Lexbor tidak ada di repo; analisis berikut dari pengetahuan
> antarmuka publik Lexbor (C99, self-contained, tanpa ICU). Setiap item yang
> harus diverifikasi saat integrasi ditandai **VERIFY**.

### 4.1 Komponen Lexbor yang relevan

| Komponen Lexbor | Dipakai? | Catatan |
|---|---|---|
| HTML tokenizer (`lexbor/html/tokenizer.h`) | ✅ | inti pengganti `html.cpp` |
| HTML tree builder (`lexbor/html/tree.h`) | ✅ | |
| DOM (`lexbor/dom/`) | ✅ | `lxb_dom_node_t`, `lxb_dom_document_t` |
| HTML parser (`lexbor/html/parser.h`) | ✅ | |
| Document fragments | 🟡 opsional | |
| Encoding (`lexbor/encoding/`) | ✅ | tabel encoding self-contained (tanpa ICU) |
| CSS tokenizer (`lexbor/css/syntax/`) | ✅ | |
| CSS parser (`lexbor/css/`) | ✅ | rules/declarations/values |
| CSS selectors (`lexbor/css/selectors/`) | ✅ | jauh melampaui 3 selector saat ini |

### 4.2 Ketergantungan Lexbor & klasifikasi

Klasifikasi: **A** langsung pakai · **B** shim kecil · **C** adaptasi besar ·
**D** tidak kompatibel.

| Ketergantungan | Klas | Alasan |
|---|---|---|
| `malloc/calloc/realloc/free` | **B** | Lexbor mengalokasi via hook sendiri (`lexbor_malloc/realloc/free`) → override ke heap Kyuzen. Tidak perlu mengubah kode Lexbor. |
| `memcpy/memmove/memset/memcmp` | **A** | tersedia di `libc.a` (entrypoints x86_64, §12.2) |
| `strlen/strcmp/strchr/...` | **A** | tersedia |
| `assert` | **B** | `assert.h` **tidak ada** di SDK (§12.2); sediakan no-op atau trap. Lexbor memakai `assert` ekstensif. |
| `abort()` (OOM) | **A/B** | libc `abort` = trap → `#UD` → panic kernel; sebaiknya override ke jalur graceful. |
| Threading / mutex / atomic | **A** (tidak dipakai) | Lexbor single-threaded; tidak butuh primitive ini. **VERIFY** tidak ada `pthread` di jalur yang dipakai. |
| File I/O | **A** (tidak dipakai) | Parser bekerja dari memori. `stdio` hanya di tools/contoh (tidak ikut). |
| Timer / OS API / env var | **A** (tidak dipakai) | — |
| Locale | **A** (tidak dipakai) | Lexbor punya tabel Unicode sendiri, bukan `locale.h`. |
| Unicode | **A** | tabel self-contained (ukuran besar, tapi tanpa dependency eksternal). |
| Filesystem / dynamic loading | **A** (tidak dipakai) | — |
| **Floating point (`double`)** | **D (potensial)** | **CSS *value* numerik Lexbor memakai `double`** (`lxb_css_value_number_t.num`). **VERIFY** di revisi vendored. Bila benar → kena blocker FP ABI yang sama dengan Duktape (§12.3). HTML/DOM/selectors sendiri integer-only. |

### 4.3 Bisakah CSS subsystem diintegrasikan independen?

**Ya, secara modul.** Lexbor memisahkan `lexbor/css/*` dari `lexbor/html/*` dan
`lexbor/dom/*`; CSS selectors bekerja di atas `lxb_dom_node_t`. Namun dua
keterkaitan wajib diperhatikan:

1. **Selectors butuh DOM Lexbor** — `lxb_css_selector` di-match terhadap
   `lxb_dom_node_t`. Jadi memakai selectors Lexbor berarti **mengadopsi DOM
   Lexbor** (tidak bisa "selectors saja" di atas `dom::Node` lama).
2. **CSS values berpotensi `double`** (§4.2) — memaksa keputusan FP lebih awal.

Konsekuensi: integrasi CSS Lexbor **terikat** pada integrasi DOM Lexbor. Urutan
yang benar: DOM dulu, baru CSS (lihat §15).

---

## 5. Duktape Compatibility Audit

> `[UPSTREAM]` — Duktape tidak ada di repo. Analisis dari antarmuka publik +
> **verifikasi empiris FP ABI** (yang bersifat decisive).

### 5.1 Target & konfigurasi

- Target: `x86_64-pc-none-elf`, freestanding KyuzenOS userspace.
- Konfigurasi Duktape yang relevan: `duk_config.h` (amalgamasi `duktape.c`),
  biasanya `DUK_USE_*`. Kandidat: `DUK_USE_FASTINT` (fast integer path) — tapi
  **inti tetap IEEE-754 double** (`duk_double_t` = `double`, tidak bisa diubah).

### 5.2 Matriks kompatibilitas

| Aspek | Status | Catatan |
|---|---|---|
| Alokasi memori | **A/B** | Duktape memakai `duk_alloc`/`duk_realloc`/`duk_free` (overridable) → heap Kyuzen. |
| GC | **A** | mark-and-sweep internal, single-threaded, tanpa dependency OS. |
| Stack | **A** | `duk_create_heap` dengan stack sendiri; butuh `setjmp` (lihat bawah). |
| Platform abstraction | **A** | `duk_config.h` bisa diarahkan ke freestanding. |
| libc | **C** | butuh `memcpy/memset/memmove/memcmp/strlen` (**A**, tersedia) + `snprintf` (**A**, tersedia) + **`fmod/floor/ceil/pow/fabs/isnan/isinf/signbit`** (**D**, libm **tidak dibangun**, §12.2). |
| Filesystem | **A** (tidak dipakai) | `duk_module`/`require` bisa dimatikan. |
| Waktu | **A** (tidak dipakai) | `Date` bisa dimatikan/`sys_uptime`/`sys_get_time`. |
| Threading | **A** | Duktape single-threaded (satu `duk_context` per thread); tidak butuh pthread. |
| Signals | **A** (tidak dipakai) | — |
| **Floating point** | **D — BLOCKER** | §5.3. |
| Compiler | **C** | butuh `setjmp/longjmp` **atau** C++ exception; keduanya **tidak ada** di Kyuzen (§12.2, §5.4). |
| Exception/error handling | **C/D** | `DUK_USE_SETJMP` → `setjmp`/`longjmp` **absen**; `DUK_USE_CPP_EXCEPTIONS` → exception C++ **dimatikan** (`-fno-exceptions`). |
| Unicode/string | **A** | Duktape punya handling UTF-8/UTF-16 sendiri (tabel CESU-8 internal). |

### 5.3 Blocker decisive — FP ABI (VERIFIED empiris)

Duktape adalah mesin berbasis `double` end-to-end:

```c
typedef double duk_double_t;   // tidak dapat diubah tanpa forking Duktape
```

Setiap operasi aritmetika, `Number`, `to_number`, dll mengembalikan/menerima
`double`. Pada `x86_64-pc-none-elf`, ABI mengembalikan `double` di `%xmm0`.
Dengan `-mno-sse -msoft-float` (flag wajib KyuzenOS), clang **menolak**:

```
error: SSE register return with SSE disabled
```

Terverifikasi juga: `-mfpmath=387` **tidak** menolong (tetap error), dan
`-mfloat-abi=soft` **tidak didukung** untuk target x86_64. Hanya target `i386`
yang berhasil (return x87), tapi KyuzenOS adalah x86_64.

**Kesimpulan §5.3**: Duktape **tidak dapat dikompilasi** untuk KyuzenOS dalam
konfigurasi toolchain saat ini. Ini bukan masalah runtime atau ukuran — ini
masalah compile-time yang tidak bisa di-workaround tanpa mengubah ABI
(kernel SSE) atau mem-fork Duktape ke ABI FP non-standar (ribuan call site).

### 5.4 Blocker sekunder — `setjmp/longjmp` & libm

Bahkan bila FP dibereskan:
- Duktape butuh `setjmp/longjmp` untuk unwinding error (default
  `DUK_USE_SETJMP`). KyuzenOS: `setjmp` **sengaja dikomentari** di entrypoints
  libc (§12.2). Shim `setjmp/longjmp` x86_64 bisa ditulis (~20 instruksi asm) →
  Klas **B/C**.
- Duktape butuh fungsi libm (`fmod`, `floor`, `pow`, ...). **`libm.a` tidak
  dibangun** di KyuzenOS (audit libc: `TARGET_LIBM_ENTRYPOINTS` kosong).
  Implementasi soft-float pun **tetap mengembalikan `double`** → kembali ke
  blocker §5.3.

### 5.5 Bisakah tanpa POSIX/pthread/fork/signal/dynamic lib/full libc?

**Ya untuk semua itu** — Duktape memang tidak memerlukannya. Tapi itu **bukan**
yang memblok. Yang memblok adalah **FP ABI + libm + setjmp**, bukan POSIX.

---

## 6. Dependency Analysis

### 6.1 Ringkasan ketergantungan per komponen

| Komponen | malloc | string.h | assert | libm | setjmp | thread | FP(double) | Klas keseluruhan |
|---|---|---|---|---|---|---|---|---|
| Lexbor HTML | B | A | B | – | – | – | – | **Layak (A/B)** |
| Lexbor DOM | B | A | B | – | – | – | – | **Layak (A/B)** |
| Lexbor CSS parser | B | A | B | – | – | – | **D?** | **Layak bila CSS values integer; VERIFY** |
| Lexbor selectors | B | A | B | – | – | – | – | **Layak (A/B)** |
| Duktape | B | A | B | **D** | **C/D** | – | **D** | **Tidak layak tanpa perubahan kernel** |

### 6.2 Detail per-dependency (dengan status Kyuzen)

| Dependency | Lexbor | Duktape | Kyuzen | Klas |
|---|---|---|---|---|
| `malloc/calloc/realloc/free` | ✅ | ✅ | `libc.a` + arena FreeListHeap (64 MiB cap, 32 arena) | A/B |
| `memcpy/memmove/memset/memcmp` | ✅ | ✅ | entrypoints x86_64 ✅ | A |
| `strlen/strcmp/strchr/strstr/strdup` | ✅ | ✅ | ✅ | A |
| `snprintf` | – | ✅ | ✅ (tanpa `%f`) | A |
| `assert` | ✅ | ✅ | ❌ (`assert.h` absen) | B |
| `abort` | ✅ | ✅ | ✅ (trap `#UD`) | A/B |
| `fmod/floor/pow/fabs/isnan/isinf/signbit` | – | ✅ | ❌ (`libm.a` tidak dibangun) | **D** |
| `setjmp/longjmp` | – | ✅ | ❌ (dikomentari) | **C/D** |
| `double` ABI | – | ✅ | ❌ (compile error) | **D** |
| `locale` | – | – | ada tapi tak dipakai | A |
| `time` | – | ops | `sys_get_time`/`sys_uptime` | A |
| threading/atomic | – | – | single-thread (`LIBC_THREAD_MODE_SINGLE`) | A |
| file I/O | – | – | parser dari memori | A |

---

## 7. DOM ↔ JavaScript Architecture

Bagian ini **mendokumentasikan apa yang harus dibangun**, bukan
mengimplementasikan. Karena Duktape terblok FP (§5), bagian ini bersifat
**rancangan bersyarat**: hanya berlaku bila keputusan FPU ditempuh.

### 7.1 Model kepemilikan yang diusulkan

| Sisi | Kepemilikan | Mekanisme |
|---|---|---|
| DOM Lexbor | dokumen memiliki tree | refcount manual: `lxb_dom_node_ref`/`unref`, destroy via document |
| JS object | Duktape heap | mark-and-sweep + finalizer |
| Jembatan | tabel peta `lxb_dom_node_t* ↔ duk_idx_t` | disimpan di Duktape heap (mis. object internal / hidden property) |

### 7.2 API yang harus dibangun (BUKAN diimplementasikan di sini)

| API | Beban binding |
|---|---|
| `document` | objek global → `lxb_dom_document_t` |
| `document.querySelector(All)` | panggil `lxb_css_selectors_parse` + `lxb_dom_node_select` |
| `document.getElementById` | walk/`lxb_dom_element_by_id` (jika tersedia) |
| `element.textContent` | get: concat text node; set: hapus anak + buat text node |
| `element.innerHTML` | get: serialize; set: **parse fragment** via Lexbor lalu ganti anak |
| `element.setAttribute/getAttribute` | map ke `lxb_dom_element_set_attribute` |
| `element.classList` | objek token-list; map ke `class` attr |
| `element.style` | objek CSSOM; parse via Lexbor CSS declaration |
| `element.appendChild/removeChild` | mutasi tree + **notifikasi layout invalidation** |
| `createElement/createTextNode` | buat node detached |
| `addEventListener` | registry `(node, type) → JS callback` |

### 7.3 Masalah lifetime/GC yang harus diselesaikan (risiko tinggi)

1. **DOM dimiliki dokumen, JS dimiliki GC** — dua sistem kepemilikan. Wrapper JS
   **tidak boleh** mem-`free` node DOM; finalizer hanya melepas *peta*, bukan node.
2. **Siklus lintas-GC**: `element → listener(JS fn) → closure → element`. GC
   Duktape melihat siklus di heap JS-nya, tetapi node DOM Lexbor yang
   mereferensi callback JS **tidak terlihat** oleh GC Duktape → **leak**. Perlu
   strategi eksplisit: weak reference + teardown dokumen yang memutus semua
   listener, atau "DOM arena" yang dibuang utuh saat navigasi.
3. **Navigasi = teardown total**: saat pindah halaman, dokumen lama dibuang;
   semua wrapper JS harus di-invalidate (bukan dipakai lagi). Ini menyederhanakan
   masalah GC secara drastis (tidak ada dokumen hidup lama).
4. **Mutasi DOM → invalidasi layout**: `appendChild/removeChild/innerHTML`
   harus menandai subtree kotor untuk re-layout + repaint.
5. **Propagasi exception JS** tidak boleh melintasi batas C (tidak ada exception
   C++): callback yang throw → ditangkap Duktape, dilaporkan sebagai error, tidak
   membatalkan event loop.
6. **Konversi nilai**: JS number (double) ↔ CSS/DOM nilai. Karena CSS/DOM Kyuzen
   integer, konversi harus eksplisit (pembulatan terdokumentasi).

---

## 8. CSS ↔ Layout Architecture

### 8.1 Batas tanggung jawab (PENTING)

Lexbor **BUKAN** layout engine. Ia menyediakan parsing + matching, bukan
cascade/computed-style/layout/paint.

```
HTML ──Lexbor──▶ DOM Lexbor
                   │
       ┌───────────┴───────────┐
       ▼                       ▼
  Lexbor CSS parser     Lexbor selectors
  (tokenize, rules,     (match selector ↔ node,
   declarations,         specificity primitives)
   values)
       │                       │
       └───────────┬───────────┘
                   ▼
        ┌──────────────────────────┐
        │  KYUZEN: cascade +        │  ← HARUS diimplementasikan Kyuzen
        │  computed style           │     (ganti css.cpp:385 compute_style)
        └──────────────────────────┘
                   ▼
        ┌──────────────────────────┐
        │  KYUZEN: layout tree       │  ← tetap layout.cpp (block+inline)
        │  (box model, flex, pos)    │     atau diperluas
        └──────────────────────────┘
                   ▼
        ┌──────────────────────────┐
        │  KYUZEN: paint             │  ← tetap browser_app.cpp drawView
        └──────────────────────────┘
```

### 8.2 Pembagian konkret

| Tanggung jawab | Lexbor | Kyuzen |
|---|---|---|
| Tokenize CSS | ✅ | |
| Parse rules/declarations | ✅ | |
| Parse selector + specificity | ✅ | |
| Match selector ↔ node | ✅ (`lxb_dom_node_select`) | |
| **Cascade (UA < author < inline)** | ❌ | ✅ |
| **Inheritance** | ❌ | ✅ |
| **Computed style** | ❌ | ✅ |
| **Box model / layout tree** | ❌ | ✅ |
| **Flex / grid / positioning** | ❌ | ✅ (belum ada) |
| **Paint / raster** | ❌ | ✅ |

**Kesimpulan §8**: Lexbor memangkas *parsing + matching* (bagian yang paling
rawan bug dan paling panjang), tetapi **seluruh mesin style & layout tetap milik
Kyuzen**. Ini mengurangi pekerjaan, bukan menghilangkannya. `css.cpp` saat ini
(cascade+computed) dan `layout.cpp` tetap relevan; yang diganti adalah
`parse_stylesheet`/`rule_matches`/`specificity` → Lexbor.

---

## 9. Resource Loading Architecture

### 9.1 Kondisi saat ini (VERIFIED)

| Resource | Bagaimana dimuat | File |
|---|---|---|
| HTML | `http::fetch` → `html::parse` | page.cpp:94-108 |
| CSS (`<style>`) | dikumpulkan saat parse, `css::parse_stylesheet` | page.cpp:110-112 |
| CSS (`<link>`) | resolve → fetch → cap 64KB → parse; **≤8** | page.cpp:117-131 |
| JS | **tidak dimuat** | — |
| Images | resolve absolut → **fetch EAGER** → `image_decode_memory`; **≤16** | page.cpp:139-170 |
| Fonts | hanya font sistem (`/Inter-Regular.ttf`), bukan `@font-face` | font.cpp:27 |
| Lain | tidak ada | — |

Catatan penting: gambar di-fetch **eager sebelum layout** karena "layout tidak
boleh melakukan I/O jaringan" (page.cpp:135-136). Fetch gambar memakai
`KyuzenTransport`/`TlsTransport` per-gambar (page.cpp:51-57).

### 9.2 Pipeline yang diusulkan

```
URL → HTTP/HTTPS (Transport) → response
      │
      ├─ content-type: text/html        → Lexbor HTML parser → DOM
      ├─ content-type: text/css         → Lexbor CSS parser → rules
      ├─ content-type: */javascript     → Duktape (TERBLOK FP)
      ├─ content-type: image/*          → image_decode_memory (PNG/BMP)
      └─ content-type: font/*           → font system (belum ada)
```

### 9.3 Apakah networking/TLS saat ini cukup? (VERIFIED)

**Ya, tanpa perubahan kernel.** Semua resource (HTML/CSS/JS/gambar/font) memakai
jalur `http::fetch(Transport, Url)` yang sama; yang berubah hanya **dispatcher
berbasis content-type** di sisi userspace (sekarang hard-coded di `page.cpp`).
`http.cpp` sudah agnostik terhadap jenis isi (ia hanya mengembalikan `body`).

**Yang perlu di userspace:**
- Dispatcher content-type (perluas `content_is_html` di page.cpp:15).
- Model resource cache (sekarang hanya `images_` per-halaman; CSS tidak di-cache).
- Untuk JS: fetch + eksekusi `<script src>` → **bergantung Duktape (terblok)**.
- Untuk async: fetch gambar/CSS/JS saat ini **sinkron** (page.cpp, satu per satu);
  async butuh pekerjaan konkurensi yang sudah didokumentasi tertunda
  (`browser-roadmap.md:8`).

---

## 10. Memory / Lifetime Model

### 10.1 Sistem alokasi yang akan hidup berdampingan

| Sistem | Sumber memori | Catatan |
|---|---|---|
| Kyuzen allocator | `sys_alloc` (syscall 9/19) → region page-granular; `sys_free` (10) | dasar |
| libc `malloc` | FreeListHeap di atas daftar arena (1 MiB→4 MiB, ≤32 arena, ≤64 MiB/req) | `libs/c/libc-port/src/kyuzen_libc_port.cpp` |
| libc++ `new` | `operator new` → `malloc`; gagal → `abort()` | `kyuzen_cxx_runtime.cpp` |
| Lexbor allocator | hook `lexbor_malloc/realloc/free` | **harus di-override ke heap Kyuzen** |
| Duktape allocator | `duk_alloc/realloc/free` | **harus di-override** |
| DOM objects | milik dokumen Lexbor | refcount manual |
| CSS objects | milik Lexbor (`lxb_css_stylesheet_t`) | dibuang saat navigasi |
| JS objects | heap Duktape | GC |
| render tree (`layout::Box`) | `unique_ptr` C++ | `layout.cpp` |
| paint objects | buffer offscreen `sys_alloc` | `browser_app.cpp:343` |
| resource cache | `std::vector<PageImage>` | `page.hpp:66` |

### 10.2 Model kepemilikan eksplisit yang direkomendasikan

```
sys_alloc (kernel)
   └── arena FreeListHeap (libc malloc)
         ├── Lexbor: semua DOM/CSS/HTML via hook override   ← dokumen memiliki
         ├── Duktape: heap JS via duk_alloc override        ← GC memiliki
         ├── C++ engine (std::string/vector/Box)            ← RAII
         └── buffer viewport (sys_alloc langsung)           ← BrowserApp
```

Aturan:
1. **Satu arah kepemilikan**: dokumen (Lexbor) memiliki DOM; GC (Duktape)
   memiliki objek JS; jembatan hanya menyimpan **peta lemah**.
2. **Tidak ada `free` lintas-sistem**: pointer Lexbor tidak pernah di-`sys_free`
   atau `free` mentah.
3. **Navigasi = teardown total** (halaman lama dibuang utuh) → memutus siklus
   lintas-GC.
4. **Custom allocator wajib** untuk Lexbor & Duktape (keduanya sudah menyediakan
   hook) → Klas **B**, bukan C.
5. **Referensi sirkular** (JS↔DOM): ditangani dengan (a) teardown dokumen saat
   navigasi, (b) weak ref untuk listener, (c) batas jumlah listener per node.
6. **Resource cache**: kunci = URL absolut; dibuang saat navigasi (belum ada
   cache lintas-halaman).

### 10.3 Kapasitas (VERIFIED)

- Heap libc: tumbuh on-demand, ≤32 arena, ≤64 MiB/permintaan
  (`libs/c/README.md`, `kyuzen_libc_port.cpp`).
- Stack task ring-3: **`TASK_STACK_SIZE = 16384`** (16 KiB) —
  `include/task.h:63`; `USER_STACK_SIZE = 256 KiB` (`include/elf.h:77`).
  → **Duktape butuh stack besar; 16 KiB kernel-task stack adalah batas keras**
  (TLS BearSSL saja sudah butuh sesi 33 KB **static**, bukan stack —
  `tls_kyuzen.h:5-6`). Ini risiko tambahan untuk Duktape (§14).

---

## 11. Event Loop Model

### 11.1 Model saat ini (VERIFIED)

```
ui_window_run(win)                          libui_abi.cpp:69
 └─ ui::Window::run()                       window.hpp
     render() awal
     while (running):
        sys_yield + timer IRQ (~60/s)
        if (notify) ...
        if (tip_tick(sys_uptime())) render()
        if (tick_cb && tick_cb(tick_data)) render()      ← hook timer tunggal
        if (sys_get_event(&ev)) switch(ev.type) { mouse/key/scroll/close }
```

**Single-threaded. Tanpa thread. Tanpa timer infra.** Timer hanya
`sys_uptime()` (ms) + IRQ PIT. `ui_tick_cb` = satu callback per-iterasi.

### 11.2 Kebutuhan Lexbor & Duktape

| Pertanyaan | Jawaban |
|---|---|
| Single-threaded cukup awalnya? | **Ya** untuk Lexbor. Untuk Duktape juga (satu `duk_context`), **bila** FP dibereskan. |
| Lexbor mengasumsikan threading? | **Tidak** `[UPSTREAM]`. |
| Duktape mengasumsikan threading? | **Tidak** `[UPSTREAM]` (single-thread per context). |
| JS bisa tetap di browser thread? | **Ya** — sejalan dengan model saat ini. |
| Callback jaringan aman berinteraksi dengan DOM/JS? | Hanya bila **satu thread**. Fetch saat ini sinkron di GUI thread (membeku). Async perlu primitif konkurensi (tertunda, roadmap:8). |

### 11.3 Target event loop (yang direkomendasikan)

```
while (running):
   sys_get_event(&ev)              → input
   drain_timer_queue(sys_uptime()) → setTimeout/setInterval (BARU, userspace)
   drain_task_queue()              → microtask/promise (BARU, JS)
   if (dom_dirty)  relayout()
   if (paint_dirty) render()
```

Timer queue & task queue adalah **tambahan userspace** yang dipasang ke
`ui_tick_cb`/loop. **Tidak butuh perubahan kernel** (cukup `sys_uptime` ms +
`sys_sleep`). Ini bagian yang paling murah dan tidak bergantung FP.

---

## 12. Freestanding Build Analysis

### 12.1 Toolchain kanonik (VERIFIED)

```
--target=x86_64-pc-none-elf -ffreestanding -nostdlib -nostdinc++
-fno-exceptions -fno-rtti -std=c++17 -O2
-mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float
link: ld.lld -m elf_x86_64 -nostdlib -T app.ld   (ENTRY _start / main)
```
(`libs/cpp/bin/kyuzen-c++`, `Makefile` CFLAGS, `apps/Makefile` CFLAGS_COMMON)

### 12.2 Permukaan libc yang tersedia (VERIFIED dari `libc.a` + SDK)

| Ada ✅ | Tidak ada ❌ |
|---|---|
| `string.h` lengkap (memcpy/memmove/memset/memcmp/memmem/strlen/strstr/strcmp/strdup/...) | `assert.h` (header absen) |
| `ctype.h` lengkap | `libm` **seluruhnya** (`fmod/floor/pow/sqrt/fabs/isnan/isinf/signbit`) — `libm.a` tidak dibangun |
| `stdlib`: malloc/calloc/realloc/free/abort/exit/atexit/qsort/bsearch/strtol/atoi/abs/div/rand/srand/aligned_alloc/strdup | `setjmp/longjmp` (dikomentari di entrypoints) |
| `stdio`: printf/snprintf/puts (tanpa `%f`, tanpa `scanf`) | `strtod/atof` (return double) |
| `time`: gmtime/localtime/mktime/strftime/timespec_get (via syscall 14/20) | `time()`, `clock_gettime`, `nanosleep`, `difftime` |
| `locale.h` (ada, tak dipakai) | `getenv/setenv`, `wchar` ops, `%f` printf |

Konfirmasi dari audit libc proyek (baris 650, 657, 665):
- `difftime` DEFER — "return `double` … gagal kompilasi".
- `atof/strtof/strtod/strtold` DEFER — "return float/double yang tak bisa dipakai `-mno-sse`".
- "seluruh libm DEFER — kebijakan tanpa-SSE/x87".

### 12.3 Bukti empiris FP ABI (VERIFIED — direproduksi)

```
$ clang --target=x86_64-pc-none-elf -ffreestanding -nostdlib \
    -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -O2 -c t.c
t.c: error: SSE register return with SSE disabled        ← fungsi return double
```
- `-mfpmath=387` → **tetap error**.
- `-mfloat-abi=soft` → **unsupported option for target 'x86_64-pc-none-elf'**.
- `double` **argumen** + operasi x87 (`fld/fadd`) **berhasil** dikompilasi (tidak
  return double). `long double` return **berhasil** (x87). Jadi x87 aritmetika
  tersedia, **hanya ABI return `double` yang mustahil**.
- Kernel **tidak pernah** set `CR4.OSFXSR`/`OSXMMEXCPT`; **tidak ada**
  `fxsave/fxrstor` di seluruh `kernel/` (VERIFIED: 0 hasil pencarian) → SSE akan
  `#UD`, dan **state FPU tidak disimpan saat context switch**.

### 12.4 Yang dibutuhkan per komponen

| Komponen | Kebutuhan build | Klas |
|---|---|---|
| Lexbor | compile C99 dengan `LIBC_CC + LIBC_TARGET_FLAGS`; sediakan `assert` shim; override allocator; **pastikan tidak ada `double` di jalur CSS values (VERIFY)** | **A/B** |
| Duktape | compile C99; **libm** (absen), **setjmp** (absen), **`double` return ABI (mustahil)** | **D** |

**Sections / static init / dynamic linking / TLS:** tidak ada masalah untuk
keduanya — Lexbor/Duktape statis, tanpa TLS, tanpa `.init_array` eksotis
(linker script `apps/app.ld` sudah menangani `.text/.rodata/.data/.bss`; C++
global ctor via `.init_array` di `_start`).

---

## 13. Kernel Impact

Prinsip proyek: **hindari perubahan kernel kecuali benar-benar perlu.**

### 13.1 Lexbor

```
Kernel changes required:
- (none)

Userspace changes required:
- allocator hook Lexbor → heap Kyuzen
- assert shim (no-op atau trap)
- dispatcher content-type (page.cpp)
- cascade/computed-style tetap Kyuzen (css.cpp)
- [VERIFY] CSS values integer (hindari double Lexbor)

No kernel changes required:
- parsing, DOM, selectors, CSS tokenizer/parser — semua userspace murni
- networking/TLS yang ada sudah cukup
```

### 13.2 Duktape

```
Kernel changes required (BILA memilih jalur Duktape):
- Aktifkan SSE/FPU: set CR4.OSFXSR (bit 9) + CR4.OSXMMEXCPT (bit 10)
  di kernel/cpu.c (sekarang hanya SMEP/SMAP/WP).
- Simpan/pulihkan state FPU saat context switch: FXSAVE/FXRSTOR 512-byte
  per task (sekarang hanya register GP yang disimpan di Full ISR Frame,
  kernel/sched/core.c:150-153) — atau lazy via CR0.TS + handler #NM (vector 7).
- Tambah area FPU per task (aligned 16) di include/task.h / lifecycle.c.
- Hapus -mno-sse -mno-sse2 -msoft-float dari flag app (dan libc/libc++ bila
  ingin FP konsisten) → ini menyentuh SEMUA ABI aplikasi, bukan hanya browser.

Mengapa syscall/API yang ada tidak cukup:
- Masalahnya bukan syscall, melainkan ABI compiler & state CPU. Tidak ada
  syscall yang bisa membuat `double`-return bisa dikompilasi.
```

**Biaya jujur jalur Duktape:** perubahan kernel FPU (bounded tapi nyata:
CR4, context switch, per-task save area, lazy-FP handler) **plus** membangun
`libm` soft-float **plus** shim `setjmp` **plus** perubahan flag ABI global.
Ini bertentangan dengan prioritas "hindari perubahan kernel" dan menyentuh
seluruh ekosistem aplikasi (bukan hanya browser). Karena itu **tidak
direkomendasikan** untuk milestone saat ini.

---

## 14. Risks

Klasifikasi risiko teknis (bukan penilaian kualitas/politik).

| # | Risiko | Klas | Alasan (bukti) |
|---|---|---|---|
| R1 | **Duktape tidak bisa dikompilasi (FP ABI)** | **critical** | §5.3, §12.3 — error compile-time terverifikasi |
| R2 | **Duktape butuh libm + setjmp (absen)** | **high** | §12.2 — `libm.a` tidak dibangun; setjmp dikomentari |
| R3 | **Lexbor CSS values memakai `double`** | **high** | §4.2 — bila benar, kena R1 di jalur CSS; **VERIFY** |
| R4 | **DOM↔GC ownership / siklus lintas-GC** | **high** | §7.3 — dua sistem kepemilikan; leak bila tak dirancang |
| R5 | **Infinite JS loop / untrusted page** | **high** | tak ada watchdog, tak ada sandbox, tak ada limit instruksi |
| R6 | Ukuran biner & memori (Lexbor + tabel Unicode) | medium | `[UPSTREAM]` Lexbor besar; tabel Unicode signifikan |
| R7 | Ukuran stack (Duktape butuh stack besar vs 16 KiB task) | medium | §10.3 — `TASK_STACK_SIZE=16384` |
| R8 | Mismatch allocator | low | §10.2 — hook override tersedia (B) |
| R9 | `assert`/`abort` → panic kernel | low | `assert.h` absen; `abort`=trap; mudah di-shim |
| R10 | Malformed HTML | low | Lexbor spec-compliant (lebih baik dari sekarang) |
| R11 | Performa (interpretasi JS, tanpa JIT) | medium | Duktape interpreter; target low-end |
| R12 | Thread safety | low | single-threaded; tidak ada shared state |
| R13 | CSS completeness (Lexbor parse ≠ layout Kyuzen) | medium | §8 — Kyuzen tetap harus mengimplementasi flex/grid/position |
| R14 | Security boundary (page di proses yang sama dengan browser) | high | tanpa isolasi proses; JS bisa menyentuh memori app |
| R15 | Resource limits (memori/CPU halaman jahat) | medium | ada cap dokumen/CSS/gambar, tapi belum ada cap eksekusi JS |

**Catatan R14/R15:** bahkan tanpa JS, halaman jahat saat ini hanya bisa
menghabiskan memori/CPU (sudah dibatasi cap). Menambahkan JS **memperbesar**
permukaan serangan secara signifikan; ini harus jadi pertimbangan desain
eksplisit sebelum JS dihidupkan.

---

## 15. Proposed Architecture

Berdasarkan temuan audit, arsitektur target (nama yang benar — **tanpa
"Kyuframe"**, yang tidak ada di repo):

```
KyuBrowser
│
├── UI (libui toolkit + libdesktop + KWM)
│   └── XML chrome + ScrollView + FtText
│
├── Browser Core (apps/browser)
│   ├── navigation        (browser_app.cpp)
│   ├── resource loader   (page.cpp → dispatcher content-type)
│   ├── document lifecycle(commit atomik, teardown navigasi)
│   └── event loop        (ui_window_run + timer/task queue BARU)
│
├── Web Engine
│   ├── Lexbor HTML  ───────┐  (GANTI html.cpp)
│   ├── Lexbor DOM   ───────┤
│   ├── Lexbor CSS   ───────┤  (GANTI parse_stylesheet)
│   ├── Lexbor selectors ───┘  (GANTI rule_matches)
│   └── Duktape            ← TERBLOK FP (§5, §13.2); ditunda
│
├── Kyuzen Style (TETAP milik Kyuzen)
│   ├── cascade + computed style   (css.cpp compute_style)
│   └── (flex/grid/position: belum ada)
│
├── Kyuzen Layout (layout.cpp)
│   └── box model, block+inline
│
├── Kyuzen Renderer (browser_app.cpp drawView)
│   └── text, images, borders, backgrounds, paint
│
└── Kyuzen Platform
    ├── memory   (sys_alloc + libc arena)
    ├── filesystem (sys_*)
    ├── networking (ksock + net_resolve)
    ├── TLS      (BearSSL)
    ├── timers   (sys_uptime/sys_sleep)
    ├── input    (sys_get_event)
    └── graphics (libui/KWM/FtText; GHAL = kernel-side)
```

**Perubahan dari diagram permintaan:**
- "UI └── Kyuframe" → **libui + libdesktop** (Kyuframe tidak ada di repo).
- Ditambah **Kyuzen Style** sebagai lapisan eksplisit (cascade/computed) —
  karena Lexbor **tidak** menyediakannya (§8).
- **Duktape ditandai terblok** — tidak dimasukkan sebagai komponen aktif sampai
  keputusan FPU.

---

## 16. Migration Plan

Staged, incremental, dengan gate eksplisit. **Tidak ada implementasi di audit
ini.**

### Stage A — Vendor Lexbor (build independen)
- **Tujuan**: Lexbor terkompilasi freestanding, tanpa di-link ke app.
- **Komponen**: Lexbor source → `third_party/lexbor/`; rule `LX_*` di `Makefile`
  (pola `BL_OS_*` BearSSL).
- **Dependensi**: `LIBC_CC + LIBC_TARGET_FLAGS`; shim `assert`; override allocator.
- **Kesulitan**: sedang (khususnya assert + allocator + VERIFY double di CSS).
- **Verifikasi**: `nm` pada arsip: **0 undefined**, tidak ada `%xmm` (objdump).
- **Regresi**: nol (belum di-link).

### Stage B — Ganti parser HTML dengan Lexbor (HTML saja)
- **Tujuan**: `html::parse` diimplementasikan di atas Lexbor tokenizer+tree builder.
- **Komponen**: `engine/html.cpp` → adapter Lexbor→`dom::Document` (atau DOM Lexbor
  langsung, lihat Stage C).
- **Dependensi**: Stage A.
- **Kesulitan**: sedang.
- **Verifikasi**: `make test-browser-html` (test yang sudah ada) + probe QEMU.
- **Regresi**: **tinggi** — parser adalah fondasi; perbedaan tree bisa mengubah
  layout. Mitigasi: adapter mempertahankan kontrak `dom::Document` dulu.

### Stage C — Integrasi DOM Lexbor
- **Tujuan**: ganti `dom::Node`/`Document` dengan `lxb_dom_document_t`.
- **Komponen**: `engine/dom.hpp`, `layout.cpp`, `css.cpp`, `page.cpp`.
- **Dependensi**: Stage B.
- **Kesulitan**: **besar** (menyentuh seluruh engine).
- **Verifikasi**: seluruh `test-browser-*` + probe; bandingkan jumlah
  node/link/gambar.
- **Regresi**: **tinggi**.

### Stage D — Integrasi CSS parser/selectors Lexbor
- **Tujuan**: `parse_stylesheet` → Lexbor; `rule_matches`/specificity → Lexbor.
- **Komponen**: `engine/css.cpp`.
- **Dependensi**: Stage C (selectors butuh DOM Lexbor).
- **Kesulitan**: **besar** (termasuk keputusan FP pada CSS values — gerbang §4.2).
- **Verifikasi**: `make test-browser-css-layout` + halaman nyata.
- **Regresi**: **tinggi**.

### Stage E — Sambungkan CSS → style → layout
- **Tujuan**: cascade+computed tetap Kyuzen, tetapi bersumber dari rules Lexbor.
- **Komponen**: `css.cpp compute_style`, `layout.cpp`.
- **Dependensi**: Stage D.
- **Kesulitan**: sedang.
- **Verifikasi**: test CSS+layout; visual probe.
- **Regresi**: sedang.

### Stage F — Duktape (TERGANTUNG KEPUTUSAN FPU) ⚠
- **Tujuan**: mesin JS berjalan.
- **Prasyarat MUTLAK**: keputusan tentang FPU/SSE kernel (§13.2). **Tanpa itu,
  stage ini tidak dapat dimulai** (tidak bisa dikompilasi).
- **Dependensi**: Stage A-C, keputusan FPU, `libm` soft-float, shim `setjmp`.
- **Kesulitan**: **sangat besar** (kernel + libm + setjmp + ABI global).
- **Verifikasi**: `duk_eval` ekspresi aritmetika dasar.
- **Regresi**: **critical** (mengubah flag ABI seluruh aplikasi).

### Stage G — DOM bindings minimal
- **Tujuan**: `document`, `querySelector`, `getElementById`, `textContent`,
  `innerHTML`, `set/getAttribute`, `classList`, `style`, `createElement`,
  `appendChild`, `removeChild`, `addEventListener`.
- **Komponen**: layer binding BARU (`engine/js/`).
- **Dependensi**: Stage F.
- **Kesulitan**: besar.
- **Verifikasi**: host test binding + probe.
- **Regresi**: sedang (fitur baru, tidak mengubah jalur lama).

### Stage H — Browser events & timers
- **Tujuan**: event dispatch DOM + `setTimeout/setInterval` di loop.
- **Komponen**: event loop (`window.hpp` hook / loop browser), timer queue.
- **Dependensi**: Stage G.
- **Kesulitan**: sedang.
- **Verifikasi**: test timer + event.
- **Regresi**: sedang.

### Stage I — Resource loading CSS/JS/images
- **Tujuan**: dispatcher content-type; `<script src>`, `<link>`, `<img>`.
- **Komponen**: `page.cpp`.
- **Dependensi**: Stage D (CSS), Stage F (JS), image decoder (ada).
- **Kesulitan**: sedang.
- **Verifikasi**: fixture multi-resource + probe.
- **Regresi**: sedang.

### Stage J — Perluas Web APIs
- **Tujuan**: `fetch`, `XMLHttpRequest`, `localStorage`, dsb. (inkremental).
- **Dependensi**: Stage I.
- **Kesulitan**: berkelanjutan.
- **Verifikasi**: per-API.
- **Regresi**: rendah–sedang.

**Penyesuaian dari rencana permintaan**: urutan A→E (Lexbor) dapat dikerjakan
**sekarang**; F→J (Duktape) **digate** pada keputusan FPU. Ini pemisahan paling
penting yang ditemukan audit.

---

## 17. Testing Strategy

### 17.1 HTML
| Kasus | Kriteria |
|---|---|
| Valid HTML | tree sama dengan spesifikasi Lexbor |
| Malformed (tag tak tertutup, stray close, `<` telanjang) | tidak crash; tree deterministik |
| Nested dalam | depth 128 tidak overflow |
| Tables | **baru** — Lexbor membangun tabel; Kyuzen harus bisa render (atau abaikan anggun) |
| Forms | dirender |
| Entities | decode benar (`&amp;`, numerik) |
| Scripts/styles | `<script>` body tidak bocor ke text; `<style>` terkumpul |
| Large docs | cap 512KB/4096 node dihormati |

### 17.2 CSS
| Kasus | Kriteria |
|---|---|
| Selectors (descendant/child/attr/pseudo) | match benar (Lexbor) |
| Cascade | UA<author<inline benar (Kyuzen) |
| Inheritance | benar |
| Box model | margin/padding |
| Flexbox / responsive | **belum ada** — tandai expected-fail |
| Media queries | **belum ada** — tandai |
| Fonts | size/weight |

### 17.3 JavaScript (bila Stage F tercapai)
| Kasus | Kriteria |
|---|---|
| `document.querySelector()` | mengembalikan node benar |
| `element.textContent` get/set | nilai benar |
| `setAttribute/getAttribute` | round-trip |
| `createElement/appendChild` | tree berubah + re-layout |
| `addEventListener` | callback dipanggil |
| `setTimeout` | callback setelah N ms (loop) |
| `fetch` | async (butuh Stage I) |

### 17.4 Integration
| Kasus | Kriteria |
|---|---|
| HTML+CSS | layout sesuai rules |
| HTML+JS | DOM dimutasi sebelum paint |
| CSS+JS | `element.style` mengubah computed style |
| DOM mutation | re-layout + repaint |
| Events | dispatch + handler |
| Dynamic rendering | halaman berubah setelah timer |
| Network resources | CSS/JS/img dimuat |
| **Acceptance**: probe QEMU **0 fail** + host test **ALL PASS** | |

Strategi yang sudah ada (`tests/host/unit/browser_*_test.cpp`, probe 22 cek)
dipertahankan sebagai **regression gate** tiap stage.

---

## 18. Open Questions

1. **CSS values Lexbor memakai `double`?** — VERIFY pada revisi vendored
   (menentukan apakah Stage D kena blocker FP).
2. **Apakah proyek bersedia mengaktifkan SSE/FPU di kernel?** — keputusan
   arsitektural terbesar; menentukan nasib seluruh JS.
3. **Apakah DOM Lexbor diadopsi penuh, atau di-adaptasi ke `dom::Document`
   lama?** — menentukan besarnya Stage B vs C.
4. **`assert` shim**: no-op atau trap? (memengaruhi kegagalan Lexbor pada input
   buruk).
5. **Batas memori Lexbor** untuk halaman besar (tabel Unicode + tree).
6. **Strategi GC DOM↔JS** — teardown total vs weak ref vs arena.
7. **Batas eksekusi JS** (instruksi/waktu) untuk halaman jahat.
8. **Apakah `libm` soft-float akan dibangun?** (prasyarat Duktape).
9. **Ukuran stack task** cukup untuk Duktape? (16 KiB sekarang).
10. **Target milestone**: Lexbor dulu, atau tunggu keputusan FPU untuk
    mengerjakan JS lebih awal?

---

## 19. Final Recommendation

Berdasarkan **bukti repo**, bukan asumsi:

1. **Lanjutkan Lexbor** untuk HTML/DOM/CSS/selectors. Ia cocok dengan
   freestanding KyuzenOS (C99, self-contained, tanpa POSIX/thread/locale/FS),
   membutuhkan hanya shim allocator + assert (Klas A/B), dan **tidak butuh
   perubahan kernel**.
2. **Tunda Duktape.** Ia tidak dapat dikompilasi untuk `x86_64-pc-none-elf`
   dengan flag KyuzenOS karena ABI return `double` (terverifikasi). Menghidupkannya
   menuntut perubahan kernel (SSE/FPU + context save) **plus** `libm` soft-float
   **plus** shim `setjmp` — melanggar prioritas "hindari perubahan kernel" dan
   menyentuh ABI seluruh aplikasi.
3. **Jangan campur** kedua pekerjaan dalam satu milestone. Lexbor memberi
   perbaikan nyata (parser spec-compliant, selector lengkap) dengan risiko
   terkendali; Duktape adalah keputusan arsitektural terpisah.
4. **Milestone pertama**: Stage A+B (vendor Lexbor → HTML di balik
   `html::parse`), diverifikasi `make test-browser-html` + probe QEMU.
5. **Pertahankan kepemilikan Kyuzen** atas cascade, computed style, layout, dan
   paint — Lexbor tidak menyediakannya.

---

## Audit Verdict

**1. Can Lexbor realistically replace the current HTML parser?**
**Ya.** Lexbor adalah C99 self-contained, tanpa POSIX/thread/locale/FS,
allocator & assert overridable (Klas A/B). Ia berjalan murni userspace tanpa
perubahan kernel. Parser HTML-nya jauh lebih lengkap (spec-compliant) daripada
`html.cpp` (10 aturan recovery ad-hoc).

**2. Can Lexbor CSS/DOM components be used independently?**
**Ya, tetapi CSS terikat pada DOM Lexbor** (selectors di-match terhadap
`lxb_dom_node_t`), jadi "CSS saja di atas DOM lama" tidak praktis. **Catatan
kritis**: CSS *value* numerik Lexbor berpotensi memakai `double` → bila benar,
Stage CSS kena blocker FP yang sama (harus VERIFY di revisi vendored).

**3. Can Duktape realistically provide JavaScript execution?**
**Tidak, dalam konfigurasi toolchain saat ini.** `double`-return tidak bisa
dikompilasi di `x86_64-pc-none-elf` dengan `-mno-sse -msoft-float` (error
terverifikasi: *"SSE register return with SSE disabled"*). Duktape berbasis
`double` end-to-end. Ditambah `libm` absen dan `setjmp` absen.

**4. What platform shims are required?**
- **Lexbor**: allocator override (Klas B), `assert` shim (Klas B). Kecil.
- **Duktape**: allocator (B), `setjmp/longjmp` (C), `libm` soft-float (D),
  **FP ABI** (D — mustahil tanpa perubahan kernel).

**5. Can this remain fully userspace?**
**Lexbor: ya.** **Duktape: tidak** — butuh kernel mengaktifkan SSE/FPU
(`CR4.OSFXSR`/`OSXMMEXCPT`) + FXSAVE/FXRSTOR saat context switch.

**6. What must Kyuzen implement itself?**
Cascade, inheritance, computed style, layout (box model/flex/grid/position),
paint, resource dispatcher (content-type), document lifecycle, event loop,
timer/task queue, DOM↔JS binding, event dispatch, dan (bila JS) watchdog/batas
eksekusi.

**7. What are the biggest technical blockers?**
- **R1 (critical)**: FP ABI `double` x86_64 → Duktape tidak bisa dikompilasi.
- **R2 (high)**: Duktape butuh `libm` + `setjmp` yang tidak ada.
- **R3 (high)**: Lexbor CSS values berpotensi `double` (VERIFY).
- **R4 (high)**: kepemilikan DOM↔GC (dua sistem, siklus lintas-GC).
- **R5/R14 (high)**: eksekusi JS tak-terpercaya tanpa sandbox/watchdog.

**8. What should the first implementation milestone be?**
**Stage A + B**: vendor Lexbor, kompilasi freestanding (0 undefined, tanpa SSE),
lalu ganti `html::parse` dengan Lexbor HTML tokenizer + tree builder di balik
kontrak `dom::Document` yang ada. Verifikasi: `make test-browser-html` ALL PASS
+ probe QEMU 0 fail. Ini langkah bernilai-tinggi, risiko-rendah, dan **tidak
menyentuh kernel maupun Duktape**.
