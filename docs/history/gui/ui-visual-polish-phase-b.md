# UI Visual Polish — Phase B

> Fondasi tidak berubah: `ThemeConfig → palet semantik → widget` (Phase A).
> Phase B hanya render/state/spacing/typography di atas peran yang ada.
> Audit latar: `ui-color-widget-audit.md`. Fondasi: `ui-theme-system.md`.

## Before

- Tombol bergradasi biru (`button_bg 0x0F3460` + `button_hover 0x2A4A7E`) —
  satu-satunya widget bergradasi, gaya tidak dipakai widget lain.
- Selected list == warna tombol (tidak dibedakan dari button).
- Fokus nyaris tak terlihat (border 1px `fg`→`accent` hanya di TextBox;
  Button/Checkbox/Slider tidak bisa fokus sama sekali).
- Tidak ada state disabled kecuali item menu (redup).
- Tidak ada hierarki tombol (semua abu/biru, aksi primer tak dibedakan).
- Checkbox centang = garis aksen di atas kotak (bukan fill).
- Slider thumb balok setinggi widget, track tanpa outline (di Light Mode
  nyaris tak terlihat di atas background terang).
- ProgressBar tanpa outline (track putih di atas terang = hilang).
- Tab aktif/inaktif sama-sama teks primer; tanpa hover.
- Dialog: judul vs isi tanpa pemisah; tombol aksi tanpa hierarki.
- StatusBar kanan (info sekunder) sama warnanya dengan kiri (posisi primer).
- Satu-satunya radius (6px, tombol) + bayangan tetap dipertahankan.

## Changes

| Widget | Perubahan visual |
|---|---|
| Button | Flat (gradien dihapus); varian SECONDARY/PRIMARY/DANGER; hover/pressed/focused/disabled; border `border` (fokus → `focus`); keyboard Enter/Spasi |
| TextBox | Border subtle → hover → focus; teks disabled; `focusable` hanya bila enabled |
| CheckBox | Checked = isi aksen + centang `accent_contrast`; border hover/focus; keyboard; disabled |
| Slider | Track + outline 1px; thumb hover `accent_hover`; fokus = outline `focus`; keyboard panah/PgUp/PgDn/Home/End; disabled |
| ProgressBar | Outline 1px + isi aksen inset |
| Tab | Judul inaktif `text_secondary`; hover judul; underline aksen tetap |
| List/Table/Tree/Grid | (Phase A) seleksi = `selection`, bukan warna tombol |
| Menu | (Phase A) hover subtle + disabled; tanpa perubahan Phase B |
| Dialog | Divider bawah judul; tombol pertama = primer aksen |
| TextEdit | Border = `focus` saat fokus keyboard |
| StatusBar | Teks kanan = `text_secondary` |

Geometri, font 8x16, layout, event routing: tidak berubah (kecuali outline
1px di dalam bounds pada track/progress — tidak menggeser layout).

## Border & Focus

- Bahasa border 1px: `border_subtle` (normal input/track), `border`
  (hover + tepi tombol), `focus` (semua kontrol saat fokus keyboard).
- Focus ring digambar DI DALAM bounds widget (damage tracking tetap tepat).
- Fokus bisa dipegang: Button/CheckBox/Slider sekarang `focusable()`
  (hanya bila enabled); klik memindahkan fokus seperti TextBox.
- Disabled generik: `Widget::enabled` + `pick()` = 0 + `set_focus` menolak +
  dispatch keyboard dilewati. API: `ui_widget_set_enabled()` (aditif).

## Button

- SECONDARY (default): `surface_elevated`, hover campuran teks 11%
  (`theme_mix`), pressed `darken 30`, teks `text`.
- PRIMARY: `accent` / `accent_hover` / `accent_pressed`, teks
  `accent_contrast`, border `accent_pressed`.
- DANGER: `danger`, hover `lighten 30`, pressed `darken 40`, teks
  `color_get_contrast_text(danger)`, border `darken 40`.
- DISABLED: `surface` + `text_disabled` + `border_subtle`; tanpa respons.
- Tidak semua tombol menjadi aksen: default tetap SECONDARY; PRIMARY untuk
  aksi utama (termasuk tombol pertama dialog), DANGER opt-in via
  `ui_button_set_variant()` (aditif, non-breaking; nilai asing = SECONDARY).

## Input

- Normal: `surface` + `border_subtle`. Hover: `border`. Fokus: `focus` +
  caret `focus`. Disabled: teks `text_disabled`, input ditolak.
- Aksen penuh tidak pernah dipakai sebagai background input.

## Checkbox / Radio

- Radio tidak ada di toolkit (dicatat, tidak dibuat di Phase B).
- Unchecked: `surface` + `border` (hover: `text`, fokus: `focus`).
- Checked: isi `accent` + centang `accent_contrast` (terbaca Dark/Light).
- Disabled: `surface` + `border_subtle` + centang/label `text_disabled`.

## Slider / Progress

- Track: `surface_elevated` + outline `border_subtle` (fokus: `focus`);
  terlihat di Light Mode. Thumb: `accent`, hover/drag `accent_hover`,
  disabled `text_disabled`. Ukuran thumb (8px) dan tinggi (20px) tetap.
- Progress: outline + isi aksen inset 1px; tinggi 16px tetap.

## Tabs

- Aktif: `surface` + underline `accent` 2px + `text`. Inaktif: `bg` +
  `text_secondary`. Hover judul: `surface` + terlacak via `track_hover`.
- Tidak ada colored rectangle penuh; tidak ada tab disabled (tanpa API —
  dicatat untuk Phase C).

## Lists / Selection

- Selected = `selection` (tinta aksen di atas background), hover =
  `surface_elevated`. Keduanya berbeda dari warna tombol di kedua mode.
- `surface_hover` TIDAK ditambahkan: hover baris terwakili
  `surface_elevated` (alasan terdokumentasi; bila Phase C butuh hover yang
  berbeda dari permukaan elevated, baru dipertimbangkan).

## Menus

- Tetap: popup `panel` + border, hover `surface_elevated` (subtle, bukan
  aksen penuh), disabled `text_disabled`, shortcut `acc_text`.
- Hierarki teks jelas via peran yang sudah ada; tanpa perubahan Phase B.

## Dialog

- Background `surface_elevated` + border + strip aksen 2px (tetap).
- Divider `border_subtle` inset 16px di bawah judul (hierarki judul > isi).
- Tombol: pertama = primer (`accent` + `accent_contrast`), sisanya sekunder
  (`btnfill` + hover `surface_elevated`). Konvensi OK/Batal: primer di kiri.
  Tanpa varian danger (dialog tak membawa info destruktif — dicatat).

## Typography

- Infrastruktur font tidak diganti (bitmap 8x16 untuk UI; FreeType hanya
  via `FtText`/desktop seperti sebelumnya).
- Hierarki via peran warna + pemisah (bukan ukuran/weight yang tak tersedia):
  `title/body/button = text`, `secondary/caption = text_secondary`,
  `disabled = text_disabled`. Diterapkan: tab aktif/inaktif, statusbar
  kiri/kanan, divider judul dialog. Alignment teks per widget tidak diubah.

## Spacing

- Audit: padding internal widget sudah di skala 4px (4/8/12/16/24):
  teks tombol 12, dialog 16, menu 24/12, statusbar 8, input/teks 4, grid 4.
- Pengecualian fungsional (terdokumentasi, bukan acak): inset hover toolbar
  2/3px, judul menubar +11px (centering dari lebar judul +22px), kolom input
  prompt +22px (pad dialog 16px + 6px), inset AA/border 1-3px.
- Satu tambahan: divider dialog (1px, tanpa geser layout). Tidak ada
  standarisasi ulang yang menggeser layout.

## Accent Usage

- `accent`: slider/progress/scrollbar/tab/marker/centang/fokus/caret/tombol
  primer/aksi dialog. `accent_hover/pressed`: tombol primer (+ thumb slider
  saat hover). `accent_contrast`: teks primer + centang.
- `accent_subtle` + `success/warning/danger` belum dikonsumsi widget
  (dicadangkan; danger dipakai varian tombol). Tanpa special-case per aksen:
  derivasi mode-aware di tema, widget buta-hue.
- Tervalidasi: Neutral, Blue, Purple, Green, Orange, Red, Custom
  (termasuk kuning ekstrem `0xFFFF00` → teks hitam via kontras).

## Dark/Light Validation

- Render piksel diuji: Dark+Neutral (semua state), Light+Neutral (tombol,
  input), Dark+Purple & Light+Purple (konsumsi aksen), custom kuning.
- Kontras teks↔background dikunci via jarak luma di matriks Phase A;
  teks-di-atas-aksen via `accent_contrast`/`color_get_contrast_text()`.

## Compatibility

- Public API: hanya PENAMBAHAN (`ui_button_set_variant`,
  `ui_widget_set_enabled`, enum varian). Tidak ada signature berubah.
- Jalur legacy (`ui_theme_t` 6 warna) tetap didukung; aplikasi legacy
  tampil koheren dengan bahasa visual baru (peran membawa nilai lama bila
  1:1). Piksel-identik pra-Phase-B TIDAK dipertahankan dengan sengaja
  (gradien tombol dihapus) — test gradien lama diganti kunci flat+state.
- `settings.ui` v1/v2, focus/hover/disabled routing lama: utuh.

## Files Changed

- `include/libui.h` — enum varian + 2 deklarasi ABI (aditif).
- `libs/gui/widget/include/core/widget.hpp` — `enabled`, `set_enabled()`,
  guard `pick()`.
- `libs/gui/widget/include/window/window.hpp` — guard fokus/disabled.
- `libs/gui/widget/abi/libui_abi.cpp` — 2 bridge (lepas fokus saat disable).
- Widget: `button` (rewrite state-based), `textbox`, `checkbox`, `slider`,
  `progressbar`, `tab`, `dialog`, `textedit` (border fokus), `statusbar`
  (kanan sekunder).
- `tests/host/unit/libui_theme_test.cpp` — §2 flat, §12 render states.
- Tak disentuh: tema Phase A, `color_t`, KWM/compositor/syscall, desktop,
  kernel, build system, aplikasi.

## Tests

- `make test-libui-theme`: **107 PASS, 0 FAIL** (67 warisan + 40 baru:
  flat/varian/fokus/disabled/keyboard per widget, tab/menu/dialog/list,
  light+purple, custom ekstrem).
- `make test-color` ALL PASS; fileman 37/37; owner 11/11.
- `make apps`: exit 0, 18 ELF, tanpa error/warning baru.
- Tidak ada screenshot harness di repo; validasi via render-canvas stub
  piksel-per-piksel (pola test yang sudah ada).

## Remaining Issues

- Tab/menu/scrollbar tanpa state disabled (tanpa API) — Phase C.
- Dialog tanpa varian danger (tanpa info destruktif) — Phase C.
- Focus traversal keyboard (Tab/Shift-Tab antar kontrol) belum ada di
  `Window` — prasyarat aksesibilitas penuh, Phase C.
- `accent_subtle`/`success`/`warning` belum dikonsumsi (lencana, validasi
  input, error text) — Phase C.
- `_fileman_probe.py` QEMU menyebut nilai tema lama — perlu ditinjau bila
  dijalankan (catatan warisan Phase A).
- `make test-textedit` rusak pre-existing (file tak ada di tree).

## Recommendation for Phase C

1. Focus traversal (Tab order) + peran `surface_hover` bila hover elevated
   terbukti ambigu di daftar padat.
2. Konsumsi `accent_subtle` (lencana/seleksi ringan), `success/warning/
   danger` (validasi input, pesan error), error state TextBox (perlu API
   `set_error` — aditif).
3. Varian danger/ghost tambahan + tombol ikonik bila kebutuhan aplikasi
   menuntut; pemilih mode/aksen di Settings Appearance (pekerjaan aplikasi).
4. QEMU visual pass (Dark+Neutral, Light+Neutral, Dark+Purple,
   Light+Purple) sebelum rilis.
