# KyuzenOS Browser — design doc

## 1. Architecture

```text
XML chrome (browser_app)          Web engine (apps/browser/engine)
  browser toolbar XML               url -> http/Transport -> html/dom
  detached inflate + find/bind      -> css cascade -> layout -> hit test
         |                                     |
    libui native                       KyuzenTransport (netutil/sys_*)
    ScrollView + FtText                TlsTransport (BearSSL, https)
                                       Page (fetch/parse/layout/images)
         |                                     |
    kzfont (Inter/FT) + image_decode_memory (stb PNG/BMP)
```

Boundaries (§87): XML != HTML, libui Widget != DOM, HTTP != document,
DOM != widget tree, browser layout != libui layout, renderer != compositor.
Satu-satunya API libui baru (minimal, §70): `ui_fttext_set_click`
(klik+koordinat), `ui_scrollview_scroll/set_scroll`, `ui_widget_pos`.

## 2. Supported protocols

HTTP/1.0 + 1.1 (GET, Host, Content-Length, chunked, close-delimited,
redirect 301/302/303/307/308 ≤10) via `http://` maupun `https://`.
`https://` = TLS 1.2 via TlsTransport (lihat §3); skema lain tetap panel
error jujur.

## 3. HTTPS status

DONE (2026-09-26). BearSSL 0.6 (`third_party/bearssl`, MIT) subset klien:
TLS 1.2 saja, suite C02F (ECDHE-RSA-AES128-GCM-SHA256) + C02B
(ECDHE-ECDSA-AES128-GCM-SHA256); hash SHA-256/384/512/1 (384/512 hanya
untuk verifikasi tanda tangan rantai ECC); kurva P-256/P-384/P-521.
Tanpa CBC/CCM/3DES/ChaCha/TLS1.0-1.1 (sengaja: tiap keluarga = file +
permukaan serangan; GCM didukung semua server uji).
  Glue: `apps/browser/tls/tls_kyuzen.c` (profil custom ±40 baris ala
  `ssl_client_full.c`, driver engine di atas ksock, tanpa malloc/stdio).
  Tiga injeksi OS (BearSSL butuh ini dari platform):
  - Entropi: syscall 87 `sys_entropy` (`include/entropy.h`,
    `kernel/entropy.c`: CPUID RDRAND + retry 10x, cap 256B). GAGAL JUJUR
    bila CPU tanpa RDRAND (`ENTROPY_ENOHW`) — tanpa fallback LCG/PIT
    (haram untuk seed kunci TLS).
  - Waktu: `sys_get_time` (RTC WIB) -> Unix UTC -> hari/detik BearSSL
    (`br_x509_minimal_set_time`). RTC ngawur = handshake ditolak.
  - Trust: prebuilt `brssl ta` -> `apps/browser/tls/tas_https.inc`
    (digenerate, tanpa parse PEM runtime): USERTrust RSA CA (jangkar
    info.cern.ch) + SSL.com TLS ECC Root 2022 (jangkar example.com).
  Batasan jujur (by design BearSSL): hanya SAN dNSName yang dicocokkan
  (SAN IP diabaikan) — URL https literal-IP tak tervalidasi; nama DNS
  wajib. Kode error numerik BearSSL selalu tampil di panel
  ("code 54/56/62...", lihat `bearssl_ssl.h`). Fixture TLS lokal
  (`tls_serve.py:8772` + `testca.crt`) hanya untuk host-test; tamu QEMU
  memakai situs nyata (slirp tidak bisa memetakan nama fiktif).
  Uji: `make test-tls` (5 grup: handshake+GET, trust kosong/host
  salah/waktu masa-depan ditolak) + probe QEMU 19 cek (fails=0).

## 4. URL parser

Skema/host/port/path/query/fragment; lowercase host; port default;
tolak userinfo/IPv6/karakter kontrol (jujur). Resolusi relatif RFC 3986
sederhana (`/abs`, `rel`, `./`, `../` clamp-di-root, `?q`, `#frag`,
`//host`, absolut). Address bar: tanpa skema -> `http://`.

## 5. HTTP client

`http::fetch(Transport, Url)` per koneksi (Connection: close). Error jujur:
Dns/Conn/Tls/Timeout/Closed/BadResponse/TooLarge/TooManyRedirects/
UnsupportedScheme. 404/500 = respons valid (dirender). Tanpa Location pada
status redirect = dikirim apa adanya. Engine agnostik enkripsi: `fetch`
bicara semantik HTTP untuk http+https; Transport yang menentukan (plaintext
vs TLS) — Page memasangkan https SELALU dengan TlsTransport.

## 6. HTML subset

html head body title meta div span p h1-h6 br hr strong em b i u a img
ul ol li pre code blockquote button input. 10 aturan recovery
terdokumentasi di `html.hpp` (auto-close p/li, void, script-drop/style-keep,
komentar, `<` telanjang, entitas unknown literal, cap 4096 node/128 depth).
Entitas: 6 named + nbsp + numerik desimal/hexa (UTF-8). DOM RAII
(`unique_ptr`, tanpa pinjam buffer parser).

## 7. CSS subset

Properti: color, background-color, font-size (Npx), font-weight/style,
text-align, margin (1-4 Npx), padding, display (block/inline/none/
list-item). Selektor: tag/`.class`/`#id` (+grup koma); kombinator/pseudo =
seluruh rule dibuang. Kaskade UA < author (spesifisitas+order) < inline;
nilai invalid tak menggeser prioritas. Inherit: color/font/bold/italic/
align; bg tidak. Warna `#RGB/#RRGGBB` + 8 nama (sintaks web, BUKAN `0x`
milik XML). Stylesheet eksternal ≤8 (≤64KB, gagal = abaikan).

## 8. DOM

`dom::Node` (Document/Element/Text), atribut lowercase + entitas terdecode,
`parent` mentah non-owning. Title pertama + `<link rel=stylesheet>` +
`<style>` diekstrak saat parse.

## 9. Layout engine

Block stack (margin aditif, tanpa collapsing — disederhanakan,
terdokumentasi) + inline lines + wrap per-kata via `TextMeasure` injeksi.
`pre` preserve; `br/hr`; `display:none` dilewati; gambar = replaced box
(intrinsik, scale-down jaga rasio, rusak = placeholder 64x64 +
`img_broken`). Fetch gambar EAGER sebelum layout (layout tak boleh I/O).

## 10. Renderer

Viewport = FtText dalam ScrollView: buffer offscreen selebar viewport,
teks via `kz_text_draw` (Inter/FT, bold = dobel-strike, italic = tegak),
gambar via blit alpha integer, link underline, lalu salin ke canvas
(ter-clip). Scroll = Scrollable bawaan (roda + scrollbar + drag).

## 11. Scrolling

Roda mouse + scrollbar + drag (jalur Scrollable). Keyboard SENGAJA absen:
shortcut window fire sebelum dispatch fokus sehingga huruf akan membajak
ketikan address bar (butuh focus-getter ABI — fase berikutnya).

## 12. Navigation

History ≤50 (potong forward-branch), Back/Fwd/Reload, token `nav_id`
(§44; sinkron kini, siap async). Sukses = ganti atomik (halaman lama utuh
bila gagal, §37). Judul window = `<title>` (§57). Klik link = hit-test +
resolve + skema-gate.

## 13. XML UI

Satu dokumen toolbar (Back/Fwd/Reload/Address/Go + status + panel error
hidden + slot viewport); viewport native (XML tak bisa menyatakan widget
kustom). Error panel = XML juga (§58: konsumen XML kedua, tanpa campur
dokumen web).

## 14. C++ stdlib usage

`string/vector/array/algorithm/memory/string_view/unique_ptr/sort`.
TIDAK: map/optional/variant/thread/mutex/chrono/regex/exception/RTTI.
`std::to_string` + `operator+(const char*, string)` TIDAK tersedia di
subset (lihat `strutil.hpp`: `append_ulong`/`lit_plus`). Error = enum/bool/
out-param. Syscall 86 + `netutil` (parse/resolve/dial) dipakai ulang app
lain (bukan khusus browser).

## 15. Resource limits (§53)

URL 2048; header 32KB/64; body 1MB; redirect 10; dokumen HTML 512KB;
node 4096/depth 128/attr 32; CSS rules 512/decl 32; gambar 16/halaman,
input 512KB, piksel 2048² (lib 4096²); stylesheet eksternal 8×64KB;
font 8-72px; history 50.

## 16. Unsupported web features (jujur)

JavaScript/WebAssembly, cookies (`unsupported` diam-diam =
diabaikan; situs butuh cookie mungkin rusak — jujur di docs), forms
(dirender, tak disubmit), flex/grid/position/float/animasi/transform,
`em`/unit CSS lain, JPEG/GIF (placeholder; hanya PNG+BMP yang didekode
freestanding tanpa float), video/audio, WebSocket, tabs, bookmark,
download, persistensi history, DNS-over-... (resolver sistem saja).

## 17. Known limitations

- Fetch SINKRON di GUI thread (tanpa thread di subset): UI membeku
  selama navigasi (dibatasi timeout DNS 5s/connect 5s/recv 10s). Status
  "Loading..." mungkin belum terpaint. Model async = fase berikutnya
  (butuh primitif konkurensi yang diaudit).
- FtText viewport h fix 8000 (tanpa resize-ABI): scrollbar selalu penuh.
- Keyboard scroll tidak ada (§11).
- Margin collapse tidak ada; `text-align` hanya untuk runs.
- Ikon app = default.png (belum ada aset ikon browser).

## 18. Testing instructions

```sh
make test-tls test-browser-url-http test-browser-html test-browser-css-layout
make test-netdns test-netutil test-netsock
python tests/browser_site/serve.py 8771   # fixture http (host)
python tests/browser_site/tls_serve.py 8772  # fixture https (host-test saja)
python tests/host/probes/_browser_probe.py  # 19 cek QEMU (butuh ISO)
```

Fixture: `tests/browser_site/` (index/style/page2/image/redirect/loop).
Pemetaan koordinat probe: window (100,80) + `KWM_TITLEBAR_H=32`.
