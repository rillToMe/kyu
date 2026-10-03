# libui Redesign — Audit & Report

> Dokumen ini mencatat **apa yang salah, apa yang diubah, dan apa yang
> dibuktikan**. Untuk cara memakai design system-nya, lihat
> [libui-design-system.md](libui-design-system.md).

---

## 1. Audit (kondisi sebelum)

### Yang sudah baik (dipertahankan)

- **Pemisahan lapisan** `core → primitives/layout → containers/chrome/dialog →
  window → abi`. Tidak diubah: batasnya benar dan sudah dijaga test.
- **Damage tracking** per-widget (`mark_dirty`/`take_dirty`) + render clip.
  Semua perubahan visual baru tetap melalui jalur ini.
- **API C yang stabil** (`libui.h`) dengan handle opaque, dan **jalur tema
  legacy 6-warna** yang piksel-identik. Keduanya dipertahankan.
- **Inflater XML transaksional** dengan validasi skema + rollback.
- **`TextEdit` sebagai satu-satunya editor**, dengan `apply_replace()` sebagai
  jalur tunggal mutasi teks.

### Masalah yang ditemukan

| # | Temuan | Bukti | Dampak |
|---|---|---|---|
| 1 | **Tidak ada token jarak/radius/ukuran.** Angka ditulis per widget: `w = len*8 + 24`, `h = 28`, `radius = 6`, `pad = 4`. | 20+ literal di 13 file | Spacing tidak konsisten; mengubah ritme = menyunting belasan file |
| 2 | **Tidak ada sistem ikon.** TreeView menggambar `'+'`/`'-'` dari font, ComboBox menggambar segitiga piksel, Menu menggambar kotak centang, Checkbox menggambar centang dari loop piksel. | 4 implementasi berbeda | Ikon dari widget berbeda terasa dari aplikasi berbeda |
| 3 | **Tidak ada peran tipografi.** Tinggi baris dan posisi teks ditulis tangan (`y + (h-16)/2`, `+ 4`), tanpa tracking/huruf besar. | 14 lokasi aritmetika glyph | Hierarki hanya bisa dibangun oleh warna |
| 4 | **Tidak ada token state.** Tiap widget menulis rantai `if` warnanya sendiri; hover tombol memakai `theme_mix(text, base, 28)`, hover menu memakai `surface_elevated`, hover tab memakai `surface`. | 3 rumus berbeda | Hover terasa berbeda antar kontrol |
| 5 | **Tidak ada token kedalaman.** Satu fungsi `shadow()` dipakai untuk semua overlay, tanpa konsep level. | `painter.hpp` | Tidak ada cara menyatakan "ini modal, bukan menu" |
| 6 | **Tidak ada token gerak.** Tidak ada durasi/easing; tidak ada animasi sama sekali. | — | (Justru aman; tapi tidak ada anggaran bila ditambahkan) |
| 7 | **Teks bocor keluar bounds.** Label/tab/menu menggambar teks tanpa pemotongan; nama berkas panjang menabrak kontrol di sebelahnya. | tidak ada `text_ellipsis` | Cacat visual nyata pada teks panjang |
| 8 | **TextBox tidak bisa digulir.** Teks > lebar kotak hanya hilang; kursor bisa berada di luar area terlihat. | `textbox.hpp` | Input panjang tidak bisa dipakai |
| 9 | **Scrollbar memakai warna aksen.** Track `surface` + thumb `accent` — aksen dipakai untuk "ini bisa digulir", bukan untuk makna. | `scrollable.hpp` | Aksen kehilangan arti; daftar terlihat ramai |
| 10 | **Daftar tidak punya bentuk.** `ListView` hanya teks + latar seleksi; tidak ada ikon, deskripsi, chevron. Settings merakit navigasinya dari teks polos. | `listview.hpp` | Setiap app merakit sendiri baris pengaturan |
| 11 | **Tidak ada primitif `Section`.** Halaman pengaturan adalah deretan label telanjang. | `system.cpp`, `about.cpp` | Hierarki hanya dari urutan, bukan dari struktur |
| 12 | **XML terkubur di dalam `.cpp`/`.c`.** `appearance.cpp`, `xml_demo.c`, `browser_app.cpp` menyimpan dokumen sebagai string literal. | 3 file | Tidak bisa di-review sebagai dokumen; tidak ada penyorotan sintaks |
| 13 | **Test XML menyalin dokumen.** `libui_xml_test.cpp` menyimpan salinan string dokumen demo. | 28 baris duplikat | Drift: dokumen asli berubah, test tetap hijau |
| 14 | **Palet desktop berbeda keluarga.** Taskbar/launcher biru-navy (`0x0B0E1C`, `0x2A3355`, `0x2E4A8E`) sementara libui netral charcoal (`0x111111`, `0x181818`). | `system/desktop/theme.hpp` | Shell terasa seperti aplikasi lain yang menempel |
| 15 | **Settings memakai 3 tema hardcoded** (Dark/Light/Green 6-warna) padahal sistem mode × aksen sudah ada. | `appearance.cpp` | Menambah aksen = menambah tema baru |
| 16 | **`rrect_border` salah untuk lingkaran.** Saat `r == h/2`, jalur cepat "baris tengah" punya rentang KOSONG dan loop arc melewati pixel ber-coverage penuh → tepi kiri/kanan lingkaran tidak tergambar. | ditemukan oleh visual QA | Handle slider, cincin radio, track switch tampak "terbuka" di sisinya |
| 17 | **`surface_variant` bertabrakan dengan `surface_elevated` di mode gelap.** Keduanya `0x232323`. | ditemukan oleh visual QA | Input (terbenam) dan tombol (terangkat) tampil identik — pembeda kedalaman hilang total di mode gelap |

Temuan 16 dan 17 **tidak ditemukan oleh audit statis** — keduanya muncul saat
gambar hasil render diperiksa. Itu argumen untuk alat QA visual (§7).

---

## 2. Design system yang ditetapkan

### Struktur

```
libs/gui/widget/include/theme/
├── colors.hpp       token warna + rumus turunan state
├── typography.hpp   peran teks + pengukuran
├── metrics.hpp      jarak, radius, ukuran kontrol, bar chrome
├── elevation.hpp    model kedalaman + kebijakan bayangan
├── motion.hpp       durasi + easing
├── icons.hpp        sistem ikon (geometri + nama semantik)
└── theme.hpp        pintu masuk tunggal
```

`core/theme.hpp` menyusun token itu menjadi `struct Theme` yang dibaca widget —
**arsitektur lama dipertahankan** (satu struct `Theme` lewat `Painter`), bukan
sistem paralel.

### Keputusan yang membentuk bahasa visual

1. **Terbenam vs terangkat.** `surface_variant` selalu menuju `bg` (input,
   track, gutter); `surface_elevated` selalu menuju teks (tombol, header).
   Arah yang berlawanan menjamin keduanya berbeda di kedua mode — ini yang
   memperbaiki temuan 17 dan membuat "input adalah lubang, tombol adalah
   permukaan" terbaca tanpa border tebal.
2. **Bayangan hanya untuk yang mengambang.** `ELEV_BASE/SURFACE/RAISED`
   menghasilkan NOL cincin secara eksplisit. Angka popup dipertahankan dari
   implementasi lama supaya menu tidak berubah saat token diperkenalkan.
3. **Aksen bukan hiasan.** Scrollbar pindah dari aksen ke netral; aksen
   disediakan untuk aksi utama, nilai terpilih, dan fokus.
4. **Hierarki dari tipografi.** `section` memakai huruf besar + tracking + tone
   sekunder + satu garis tipis. Halaman pengaturan tidak lagi butuh kartu.
5. **Baris daftar punya bentuk baku** (ikon/judul/deskripsi/trailing/chevron)
   dan **tidak** dibungkus kartu — dijaga anggaran render (§6).
6. **XML struktural.** Atribut visual (`padding`, `radius`, `color`, `shadow`)
   **ditolak** inflater dan dikunci test.

---

## 3. Arsitektur

| Lapisan | Perubahan |
|---|---|
| `theme/` | **baru** — 7 header token, tanpa kode yang bergantung pada widget |
| `core/theme.hpp` | `Theme` diperluas: `surface_variant`, `surface_hover`, `surface_pressed`, `text_tertiary`, `accent_text`, `info`, `Metrics`, `Typography`; `derive_states()` memakai token |
| `core/state.hpp` | **baru** — model state + pemetaan state→warna |
| `core/painter.hpp` | `text_role()`, `text_ellipsis()`, `icon()`, `surface()`, `outline()`, `focus_ring()`, `shadow(level)`, `rrect_border()` diperbaiki |
| `primitives/*` | Button (+tertiary, +ikon), CheckBox, Radio, **Switch (baru)**, TextBox (gulir), Slider, ComboBox, **IconView (baru)** |
| `containers/*` | **Section (baru)**, ListView (baris kaya + keyboard), Scrollable (scrollbar netral), Tab (underline) |
| `chrome/*` | Menu, MenuBar, Toolbar (+ikon), StatusBar |
| `dialog/*` | Dialog (tipografi + elevasi), PromptDialog |
| `xml/*` | elemen `section`/`switch`/`icon`, atribut `icon`/`variant="tertiary"`, parser nama ikon |

**Invarian yang dipertahankan** (dari `CLAUDE.md`): lapisan bawah tidak pernah
meng-include `window/window.hpp`; `operator new/delete` + `__cxa_pure_virtual`
tetap hanya di `src/runtime/runtime.cpp`; tidak ada objek global non-trivial.

---

## 4. Komponen

| Widget | Perubahan |
|---|---|
| Button | +`tertiary` (tanpa isi/border), +ikon, token state, radius token |
| CheckBox | Kotak 16px radius SMALL, centang dari sistem ikon, area klik seluruh baris |
| Radio | Cincin+dot dari token (bukan loop piksel), geometri selaras CheckBox |
| **Switch** | Baru — track+knob, kapsul, keyboard (Spasi/Enter/panah) |
| TextBox | Permukaan terbenam, **gulir horizontal**, caret terlihat, teks dipotong |
| Slider | Track terbenam + **fill aksen** + grip bulat; matematika input tidak diubah |
| ComboBox | Permukaan terbenam, chevron dari sistem ikon, teks dipotong |
| ListView | **Baris kaya** (ikon/judul/deskripsi/trailing/chevron), **navigasi keyboard**, tinggi baris dinamis |
| Scrollable | Track transparan, thumb netral (bukan aksen), hover pada track |
| Tab | Lebar mengikuti judul, underline aksen, strip tenang |
| Menu | Gutter centang kondisional, accelerator tone tersier, hover token |
| MenuBar/Toolbar/StatusBar | Tinggi token, tombol toolbar ber-ikon, hierarki tone |
| Dialog | Hierarki tipografi (judul+divider), elevasi DIALOG, tombol senada halaman |
| PromptDialog | Input terbenam, caret tema, teks dipotong |
| **Section** | Baru — judul peran `section` + garis; primitif halaman pengaturan |
| **IconView** | Baru — ikon mandiri non-interaktif |

---

## 5. XML

- Dokumen pindah ke **`ui/xml/*.xml`** (file sungguhan) dan di-embed build
  lewat `cmake/KyuzenUiXml.cmake` + `cmake/embed_xml.cmake`.
- `settings_appearance.xml` dan `xml_demo.xml` sudah dimigrasikan.
- Test `test-libui-xml` kini memakai **dokumen yang sama** (di-embed), bukan
  salinan — temuan 13 tertutup secara struktural.
- Elemen baru: `section`, `switch`, `icon`. Atribut baru: `icon`,
  `variant="tertiary"`, `size`.
- Atribut visual **ditolak** (test `G07` mengunci daftar penolakan).

---

## 6. Performa

Diukur dengan `test-libui-perf-qa`: berapa pixel kanvas yang disentuh satu
`draw()`, dalam satuan layar 1280×720 (9.216.000 px). Metrik ini dipilih karena
stabil lintas mesin — bukan pengukuran waktu yang goyah.

| Widget | Biaya | Anggaran |
|---|---|---|
| Button (primary) | 2.228 px = 0.0024 layar | ≤ 0.010 |
| Button (ikon) | 772 px = 0.0008 | ≤ 0.010 |
| CheckBox | 252 px = 0.0003 | ≤ 0.010 |
| Switch | 528 px = 0.0006 | ≤ 0.010 |
| Slider | 1.074 px = 0.0012 | ≤ 0.020 |
| TextBox | 6.708 px = 0.0073 | ≤ 0.020 |
| Section (kosong) | 600 px = 0.0007 | ≤ 0.010 |
| ListView 12 baris kaya | 1.075 px = 0.0012 | ≤ 0.050 |

Anggaran ListView sengaja ketat: baris daftar **transparan**, sehingga 12 baris
hanya menyentuh 0.0012 layar. Anti-pola yang dilarang bahasa visual ini —
membungkus tiap baris dalam kartu berlatar (12 × 600 × 40 px = **0.31 layar**)
— akan langsung melewati anggaran dan gagal.

Perubahan yang **menambah** biaya render: `text_role` menggambar per karakter
saat tracking/huruf besar aktif (peran `section` saja); `rrect_border` memindai
maksimum `r` pixel dari tiap sisi (dibatasi radius, bukan lebar). Keduanya
terukur di atas.

---

## 7. Validasi

### Build & test

```
./build.sh          # kernel + libs + apps — bersih, tanpa warning
./build.sh test     # 19/19 test hijau (host)
./build.sh iso      # ISO + module guard — bersih
```

Test libui (semuanya hijau):

| Test | Isi |
|---|---|
| `test-libui-theme` | 257 assertion — tema, state, **kedalaman permukaan**, format file |
| `test-libui-xml` | 137 assertion — skema, inflasi, penolakan atribut visual, **dokumen sungguhan** |
| `test-libui-fileman_widgets` | 37 — tabel/menu/gridview |
| `test-libui-owner` | 11 — kepemilikan widget, anti-ghost |
| `test-libui-visual-qa` | render 6 gambar (1280×720 & 1920×1080, terang & gelap, aksen) + cek "benar-benar tergambar" |
| `test-libui-perf-qa` | 8 anggaran render |

Total: **450+ assertion** di enam target test libui.

### QA visual (headless)

`test-libui-visual-qa` merender widget **produksi** ke PPM pada 1280×720 dan
1920×1080, mode terang dan gelap, plus aksen non-netral. Pemeriksaan piksel
menemukan dua bug nyata (temuan 16, 17) yang tidak terlihat oleh audit statis —
keduanya sudah diperbaiki dan dikunci test.

Contoh hasil pemeriksaan (mode gelap, 1280×720):

```
bg 111111 · surface 181818 · surface_variant 131313 · surface_elevated 232323
```

Empat tingkat permukaan yang berbeda — sebelumnya `variant` dan `elevated`
sama-sama `232323`.

### QEMU

`tests/host/probes/_libui_probe.py` — boot → login → desktop → jalankan tiga
aplikasi:

```
PASS boot+login: desktop siap ([desktop] shell started)
PASS settings:  mencapai event loop ([settings] start)
PASS uigallery: shell men-spawn proses
PASS xml_demo:  shell men-spawn proses
PASS sesi:      tanpa panic/BSOD
libui-probe: OK
```

Screendump mengonfirmasi jendela benar-benar tergambar dan **berbeda satu sama
lain** (bukan desktop kosong): Settings 19.1% pixel jendela, Gallery 30.8%
(jendela terbesar, sesuai ukuran 960×640), XML demo 9.6%.

Catatan: probe sengaja TIDAK mencari kata `PANIC` polos — kernel mencetak
`[PANIC_LOG] tidak ada crash` pada setiap boot sehat, sehingga pencarian mentah
memberi false positive di seluruh probe (kesalahan yang sempat terjadi dan
diperbaiki).

---

## 8. Sisa pekerjaan

Jujur — ini yang **belum** dikerjakan:

1. **Aplikasi lain belum dimigrasikan.** Settings, UI Gallery, XML demo, dan
   File Manager sudah. Belum: Browser, Notepad, Task Manager, Image Viewer,
   Gallery, widget_demo, terminal. Mereka tetap jalan (API dipertahankan) dan
   otomatis mewarisi perbaikan widget, tapi tata letaknya belum memakai
   `Section`/baris daftar kaya, dan belum memanggil `ui::uifont_install()`
   sehingga masih memakai bitmap 8×16.
2. **Hierarki tipografi jalur font nyata belum bertingkat.** Provider memakai
   SATU ukuran (15px) untuk semua teks — bitmap juga begitu, jadi ini tidak
   mundur, tetapi `title`/`body`/`caption` belum benar-benar berbeda ukuran.
   Menaikkannya butuh provider multi-ukuran (satu `kz_font_t` per peran) dan
   `TypeRole::ft_size_px` dihormati di jalur gambar.
3. **Animasi belum diimplementasikan.** Token durasi/easing ada dan terukur,
   tapi belum ada satu transisi pun yang memakainya. Sengaja: menambahkan
   animasi tanpa kebutuhan nyata melanggar aturan "gerak harus menjelaskan".
4. **`browser_app.cpp` masih menyimpan XML di dalam `.cpp`.** Infrastruktur
   embed sudah ada; migrasinya belum dilakukan.
5. **Kontrol high-DPI belum diuji.** Token berbasis px; belum ada jalur scaling.
6. **`TextEdit` belum memakai peran tipografi.** Ia punya konstanta `CHAR_W`/
   `LINE_H` sendiri yang selaras dengan grid 8×16, tapi belum membaca token —
   dan belum ikut text provider (editor tetap bitmap walau font sistem aktif).
7. **Probe QEMU File Manager punya keterbatasan harness yang terdokumentasi.**
   Pemetaan koordinat mouse `-display none` tidak 1:1 dengan guest dan skalanya
   tidak linear, jadi jalur mouse diuji lewat *verifikasi hasil* (baris mana yang
   terpilih) sementara jalur keyboard deterministik. Langkah "new folder di
   /home/user" masih TIDAK karena disk uji tidak punya `/home/user` saat
   dijalankan; rename + delete terbukti bekerja di direktori yang ada.
