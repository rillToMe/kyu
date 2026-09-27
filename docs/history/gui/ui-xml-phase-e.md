# UI XML Declarative — Phase E

> XML adalah KONSUMEN libui, bukan pengganti. Setelah Phase E:
> perilaku Widget = libui native, Layout = Phase D, Fokus = Phase C,
> Tema = Phase A, Render = pipeline existing. XML hanya input deklaratif.

## 1. Arsitektur

```text
XML -> parse (xml_parser) -> validasi+inflate (xml_inflate)
  -> API/behavior libui native -> render/input normal
```

- `libui` (core widgets/layout/theme): tak tersentuh (nol perubahan).
- `libui_xml` (baru): parser + representasi + validasi + inflater +
  konteks. Hidup di `libs/gui/widget/src/xml/` + `include/xml/xml.hpp`
  (internal) + `include/libui_xml.h` (ABI C publik).
- Cast `reinterpret_cast` tetap hanya di `abi/libui_abi.cpp` (invarian repo).
- Runtime inflation (bukan codegen). Bukti audit: `Window::add`,
  `Layout::add`, `Grid::put`, `ScrollView::set_child`, `Tab::add`,
  `ListView::add_item`, `ComboBox::add_item` sudah mencakup semua konstruksi;
  dtor Layout/Tab/ScrollView cascade; `RadioGroup` two-way cleanup aman
  kedua urutan destroy. Tak ada blocker arsitektural.

## 2. Parser Subset

Didukung: elemen buka/tutup/self-closing (spasi sebelum `/>` boleh),
atribut quote tunggal/ganda, nesting, whitespace, entity 5 dasar
(`lt/gt/amp/quot/apos`), deklarasi `<?xml?>` di awal, komentar `<!-- -->`.
Ditolak deterministik: DTD, entity kustom/eksternal, namespace, XPath,
XInclude, XSD, CSS, CDATA, DOCTYPE, PI non-deklarasi, `&` mentah, `<` di
nilai atribut, atribut duplikat, teks non-whitespace di mana pun
(UI dari atribut saja — representasi SENGAJA tanpa field teks).

## 3. Skema

Akar: `<window>` (satu-satunya). Elemen: `vbox hbox grid label button
textbox textedit? TIDAK textedit checkbox radio combobox slider progressbar
separator image listview tab scrollview` + struktural `item` (combo/list
saja) + `page` (tab saja). Disengaja DIKELUARKAN: TextEdit (undo/clipboard
app-wiring), Table/TreeView (kolom/depth), Dialog/Prompt/Menu/Toolbar/
StatusBar/MenuBar (app chrome), FtText (callback), GridView (thumbnail
non-owning). Tabel atribut per elemen: lihat `reject_extra` + `make_*`
di `xml_inflate.cpp` (satu sumber kebenaran, bukan duplikat docs).

Atribut generik: `id width height enabled visible tooltip` (+ penempatan
`row col rowspan colspan` — diizinkan di semua widget, bermakna hanya
sebagai anak grid). `width/height` eksplisit = `set_size` (kontainer
VBox/HBox menimpa di arrange = perilaku native). Widget ber-ctor-size
(textbox/combo/list/tab/scroll/image/progress) memakai atribut yang sama
sebagai param ctor (tanpa error ganda).

## 4. Widget Didukung

label button textbox checkbox radio combobox slider progressbar separator
image listview tab scrollview + kontainer vbox hbox grid. Semua dipetakan
ke ctor + setter native yang sudah ada; default = default native
(XML tak punya tabel default sendiri).

## 5. Atribut

- bool ketat: `true/false/1/0`.
- int ketat `^-?[0-9]+$`, `|v|<=1e9`; domain per atribut (mis. width 1..4096,
  spacing 0..64, padding 0..4096).
- spacing: int atau token `xs/sm/md/lg/xl` (= `UI_SPACE_*` Phase D).
- align: `start/center/end/stretch` (grid saja).
- variant: `secondary/primary/danger`; orientation: `horizontal/vertical`.
- theme-mode `dark/light`; theme-accent 7 nilai native; theme-custom
  `0xRRGGBB` (wajib bila accent=custom; `theme-mode` wajib bila ada atribut
  tema lain; accent default = neutral = default native).
- grid: `rows cols` wajib (>=1; >8 = clamp native, >64 = tolak),
  `gap` default 8, `padding` uniform ATAU per-sisi (campur = error),
  anak wajib `row+col` (`rowspan/colspan` default 1; `put` native yang
  memutuskan: 0 = `BAD_PLACEMENT`).
- radio `group="nama"` (bukan elemen struktural — radio boleh di sel mana
  pun); `selected` programatik-diam.
- combo/list `selected` (validasi terhadap jumlah item; `-1` = tak ada).
- slider `min max value` (max<=min = perilaku native); progress `value`
  (clamp native); textbox `text error`; checkbox `checked`; tab `page`
  (`title` wajib, tepat 1 panel, `enabled` opsional); scrollview 0..1 anak
  (+ `pan`); image `src` wajib (diteruskan ke native apa adanya — parser
  tanpa akses file); separator default horizontal.
- Kanonik teks = atribut `text` (tak ada `<label>Hello</label>`).

## 6. Ownership

`parse -> pohon milik dokumen -> validate/inflate -> destroy dokumen`.
String widget disalin ke penyimpanan milik-widget (konvensi `_ui_strdup`).
UI TAK bergantung pada dokumen (diuji: dokumen mati sebelum interaksi).
Inflasi transaksional: bangun DETACHED, window disentuh hanya saat commit
(tema + `Window::add`); gagal = rollback (delete roots berjenjang + grup).
Satu inflasi per konteks; konteks hancur setelah window (urutan dokumen;
urutan balik tetap aman via two-way cleanup). Tanpa global/XML registry.

## 7. Error Model

Kode `UI_XML_*` + baris/kolom 1-based + elemen/atribut terkait.
Unknown element/attribute = error (tak ada typo lolos). Nilai invalid =
error, kecuali yang punya semantik clamp native (slider value, progress,
span dijepit `put`) — didelegasikan, bukan aturan XML.

## 8. Limits

`MAX_DOC 64K, DEPTH 16, NODES 512, ATTRS 16, CHILDREN 64 (parser;
batas native 16/32/8 di inflater), ATTR_NAME 32, ATTR_VALUE 256,
ID 48/32, GROUPS 8, ROOTS 16` — dari kendala repo (Layout 16 anak,
Grid 32 sel, ComboBox 16 item, TextBox 256).

## 9. Theme Mapping

Atribut tema -> `ui_theme_config_t` -> `ui_window_set_theme_config()` ->
palet semantik (satu-satunya jalur). `theme-custom` -> `color_hex()`
(satu-satunya `0xRRGGBB` di XML). Tanpa inherit/cascade/class.

## 10. Layout Mapping

`vbox/hbox/grid` -> native (pengukuran native; VBox/HBox self-size lewat
`arrange`). Grid: inflater AUTO-SIZE dari `measure_tracks` bottom-up
(settle anak dulu) bila width/height tak eksplisit — native Grid
caller-sized, jadi tanpa ini VBox menumpuknya sebagai h=0 dan anak overlap
saudara (regresi yang men-crash-kan probe pertama). Eksplisit selalu menang
(penting untuk track FILL). Tanpa mesin geometri kedua.

## 11. IDs

`id` milik konteks (max 32, duplikat = error). `ui_xml_find` untuk lookup.
`group="nama"` milik konteks (max 8 grup).

## 12. Callback Policy

XML tanpa kode. Binding eksplisit dari C:
`ui_xml_bind(ctx, id, UI_XML_ON_CLICK/_ON_CHANGE, cb, userdata)`.
Pemetaan: CLICK = button->click, checkbox/radio/combo/slider/list->change;
CHANGE = checkbox->toggle, radio/combo/slider/list->change, textbox->enter.
Pasangan lain = 0 (ditolak). Cukup untuk demo nyata; tanpa sinyal/slot.

## 13. Resource Policy

`src` image = string opaque ke native (pemuat KyuzenFS milik Image).
Parser tak membuka file, tak resolve path, tak ada traversal.

## 14. Contoh

Lihat `apps/xml_demo.c` (`UI_DOC`) — layar settings-like: tema dark+purple,
grid 3x2 (combo + checkbox + radio grup), separator, textbox+tooltip,
slider, tombol primer/sekunder, binding C (apply/cancel/notif/mode/theme).

## 15. Known Limitations

1. Grid auto-size memakai `settle()` anak saat inflasi (posisi relatif
   detached; ukuran final sama — dikunci test I08b/c/d + GEO).
2. FILL tanpa size eksplisit = 0 (semantik native, terdokumentasi).
3. `make test-textedit` rusak pre-existing (pra-Phase E).
4. Probe butuh `test_disk.img` segar bila `start: file tidak ada`
   (artefak disk persisten antar-run, bukan bug XML).
5. Kontrak platform: app tak boleh `return` dari main (`ENTRY(main)`) —
   `xml_demo` memakai `sys_exit()` (pelajaran dari #GP saat exit).
6. Mouse QEMU butuh klik ganda untuk target kecil (fokus + aksi).

## 16. Future NOT Implemented

CSS/style/class/cascade, scripting, XPath/XSD/DTD, binding framework,
MVVM/sinyal, factory refleksi generik, codegen, persen/flex/grid-CSS,
warna CSS, TextEdit/Table/TreeView/Dialog/Menu/chrome dalam skema,
track-config XML (`col0="..."/fill`), inflasi multi-konteks reuse.
