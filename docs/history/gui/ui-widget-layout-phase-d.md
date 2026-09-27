# UI Widget + Layout Foundation — Phase D

> Di atas Phase A (tema) + B (visual) + C (interaksi). Tanpa XML, tanpa
> redesign fondasi. Phase E mengonsumsi API di sini secara deklaratif.

## Widget Inventory

| Widget | Baru | Fokus | Disabled | Keyboard |
|---|---|---|---|---|
| Button | – | ya | ya | Enter/Spasi |
| Checkbox | – | ya | ya | Enter/Spasi |
| Radio | YA | ya | ya | panah + Enter/Spasi |
| TextBox | – | ya | ya | teks + error state |
| TextEdit | – | ya | – | penuh (Tab = indent) |
| Slider | – | ya | ya | panah/PgUp/PgDn/Home/End |
| ProgressBar | – | – | – | – (read-only) |
| Tab | – | ya (strip) | per-judul | panah (lewati disabled) |
| ComboBox | YA | ya | ya | Enter/Spasi/panah/Esc |
| Menu | – | – | per-item | – (mouse) |
| Tooltip | YA | tidak | – | – (presentasional) |
| Separator | YA | tidak | – | – (non-interaktif) |

Radio/ComboBox/Tooltip/Separator menutup celah audit Phase A §4.

## Radio API

```c
ui_widget_t* ui_radio_create(ui_window_t* win, const char* label);
void ui_radio_set_selected(ui_widget_t* widget, int selected);  // diam
void ui_radio_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);
ui_radio_group_t* ui_radio_group_create(void);
void ui_radio_group_destroy(ui_radio_group_t* group);
void ui_radio_set_group(ui_widget_t* widget, ui_radio_group_t* group);
ui_widget_t* ui_radio_get_selected(ui_radio_group_t* group);
```

Grup = objek logis (bukan widget): tanpa bounds/gambar/traversal/global.
Cleanup dua arah (grup hancur → anggota lepas; radio hancur → keluar grup,
termasuk bila terpilih). Tanpa grup = mandiri (klik memilih). Klik/panah
hanya fire bila seleksi BERUBAH. Panah lewati disabled + pindahkan fokus.
Visual: lingkaran 12px (selaras CheckBox) + dot aksen + ring
border/hover/focus + label redup saat disabled.

## ComboBox API

```c
ui_widget_t* ui_combobox_create(ui_window_t* win, int width);
int ui_combobox_add_item(ui_widget_t* widget, const char* label);
int ui_combobox_remove_item(ui_widget_t* widget, int index);
void ui_combobox_clear(ui_widget_t* widget);
int ui_combobox_count(ui_widget_t* widget);
int ui_combobox_selected(ui_widget_t* widget);
void ui_combobox_set_selected(ui_widget_t* widget, int index);  // diam
void ui_combobox_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata);
```

String disalin (pola ListView), kapasitas 16. Popup = `ui::Menu` (reuse
`open_popup/close_popup`, bukan framework baru); dibuat ulang tiap dibuka,
pick-trampolin nilai (tanpa alokasi). Sinkron tutup-dari-luar via
`Widget::on_popup_dismiss()` (klik-luar/ESC/Tab/dtor). Tab menutup popup
dulu baru traversal (tak bocor ke balik popup). Tertutup + panah = navigasi
langsung; terbuka + panah = hover; Enter = commit + tutup.

## Tooltip Behavior

- `ui_widget_set_tooltip(widget, text)` (disalin; 0/"" menghapus).
- Tampil 600ms setelah hover diam (`sys_uptime`, pola notify), tanpa fokus/
  traversal, hilang saat pointer pindah/target mati/dialog dibuka.
- Posisi deterministik: atas target, fallback bawah, clamp window
  (`Window::tip_calc`, unit-test murni). Warna ikut tema window.
- Target hancur → sembunyi tanpa deref (`owns_widget` + validasi di titik
  gambar). Hanya teks (tanpa markup).

## Separator Behavior

- `ui_separator_create(win, UI_SEP_HORIZONTAL/VERTICAL)`; tebal tetap 1px,
  panjang via `ui_widget_set_size` (konvensi existing).
- `pick()` selalu 0 (klik menembus), tak focusable, tak masuk traversal.
- Warna `border_subtle` (tanpa warna khusus).

## Layout Measurement Model

- Anak: ukuran sendiri (fixed/preferred; Label otomatis dari teks).
- Kontainer: ukuran ditentukan caller (`ui_widget_set_size`), KECUALI
  VBox h = isi dan HBox w = isi (perilaku existing, termasuk trailing
  spacing — didokumentasikan, tidak diubah karena ScrollView bergantung).
- Grid track: AUTO (isi) / FIXED (px) / FILL (sisa dibagi rata).
- Tak ada persen, flexbox, solver, baseline.

## Sizing Model

`fixed` (ctor/set_size) / `preferred` (Label, VBox-h, HBox-w, Grid-AUTO) /
`fill` (Grid-FILL). STRETCH (align) mengubah ukuran anak ke sel — terdefinisi
dan idempoten, tetapi memengaruhi pengukuran AUTO berikutnya (terkunci test).

## Alignment

`UI_ALIGN_START/CENTER/END/STRETCH` — di Grid (dalam sel, dua sumbu).
VBox/HBox tetap start/top-aligned fixed (perilaku existing, terkunci test).

## Padding

`ui_padding_t {left,top,right,bottom}` + `ui_grid_set_padding()` (negatif
dijepit). Memengaruhi origin + area FILL. VBox/HBox tanpa padding
(didokumentasikan; perilaku existing).

## Spacing

Skala audit: 4/8/12/16/24 (`UI_SPACE_XS/SM/MD/LG/XL`). VBox/HBox spacing =
ctor (tanpa gap tepi, termasuk trailing di ukuran diri). Grid `gap` antar
track, tanpa tepi. Tanpa margin semantics (belum ada).

## Grid Model

- `ui_grid_create(win, rows, cols, gap)` (maks 8×8, 32 sel), `ui_grid_put`
  dan `ui_grid_put_span` (span dijepit; overlap/invalid/luar → 0).
- `ui_grid_set_col/row` (mode + px), `ui_grid_set_padding`,
  `ui_grid_set_align`.
- Algoritma: single-span → spanning (kembangkan track terakhir) → FILL
  (sisa ≥ 0 dibagi rata + sisa bagi). Tanpa iterasi/solver.
- Subclass `Layout`: ownership/dirty/pick/traversal/fokus dipakai ulang;
  hanya sel terpasang yang digambar/di-hit.

## Ownership Rules

- Layout/Grid: anak milik kontainer (dihapus bersamanya); anak harus heap.
- RadioGroup: bukan pemilik widget; detach dua arah; grup milik pemanggil.
- ComboBox: menu + string miliknya; tutup popup sebelum hancur.
- Tooltip: teks milik widget (bebas di dtor).
- Tanpa global, tanpa ownership tersembunyi.

## Invalidation Rules

- Mutasi (put/padding/align/track/visible/enabled/data) → `mark_dirty()`.
- Penempatan via `place()` (union lama+baru, mekanisme existing).
- Tanpa sistem invalidasi kedua.

## Test Coverage

`test-libui-theme` §18–§23 (D1–D44 + matriks): radio (create/select/
eksklusivitas/disabled/fokus/keyboard/destruksi/ABI), combobox (CRUD/
seleksi/buka-tutup/keyboard/esc/disabled/destruksi/ABI), tooltip
(show/hide/posisi/clamp/fokus/destruksi), separator, layout (VBox/HBox/
padding/spacing/align/fixed/auto/fill/grid/span/resize/hidden/disabled/
nested), geometri eksak, matriks Dark+Purple/Light+Neutral/Green.
QEMU: `_layout_probe.py` (radio klik+panah, combo fokus/buka/commit/esc,
tooltip hover, separator, grid, serial bersih).

## Known Limitations

1. Grid maks 8×8 / 32 sel; popup menu tanpa flip bawah-layar.
2. Tab strip + panel sama-sama stop traversal (warisan C).
3. Menu tanpa model keyboard (warisan C).
4. STRETCH memutasi ukuran anak (terdefinisi; lihat Sizing).
5. btnfill == panel di jalur config (tombol sekunder dialog menyatu
   background — diamati, bukan bug Phase D).
6. Probe fileman: 3 TIDAK posisi (Pictures, toolbar, delete) = kalibrasi
   + state disk antar-run (pre-existing); delete diperparah asumsi warna
   pra-Phase-B di probe (diperbaiki ke primer-aksen + fallback).
7. `make test-textedit` rusak pre-existing.

## Explicit XML Boundary

```text
Phase D does not implement XML.
Phase D does not define XML schema.
Phase D does not contain XML-specific APIs.
```

Tak ada `ui_xml_*`, `ui_inflate_*`, factory, codegen, atau nama properti
gaya-XML di API inti. Semua API di atas native dan semantik.
