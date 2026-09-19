# Desain: Phase 8 — Advanced Widgets (ListView, TreeView, Table, ScrollView, Menu, MenuBar, Tab, Toolbar)

> **Status**: SELESAI (2026-08-06) — build clean; verifikasi runtime manual oleh
> user (QEMU): menu dropdown open/hover/switch/ESC/dismiss, toolbar, switch tab,
> scroll roda + scrollbar drag di daftar/tabel/pohon/gulir, expand/collapse
> pohon, regresi Phase 7 (Kontrol). Semua 8 widget render di `widget_demo`.
> **Konteks**: Phase 7 memberi Standard Widgets (TextBox, Image, CheckBox,
> Slider, ProgressBar). Phase 8 menambahkan **Advanced Widgets** sesuai roadmap:
> ListView, TreeView, Table, ScrollView, Menu, MenuBar, Tab, Toolbar.

Catatan roadmap "ScrollView reuse Viewport Phase 3C" berlaku sebagai *konsep*
(potong isi di tepi viewport) — toolkit berjalan di userspace dan tidak bisa
menyentuh Viewport kernel, jadi mekanisme scroll/clip dibangun di sisi toolkit.

## Tiga kapabilitas baru yang dibutuhkan Phase 8 (belum ada sebelumnya)

1. **Clipping**. libgui TIDAK punya API scissor/clip (semua `gui_draw_*` hanya
   skip per-pixel di luar canvas). Konten yang digeser scroll harus dipotong di
   tepi viewport → clip hidup di `Painter` toolkit.
2. **Roda scroll**. `EVENT_SCROLL` (type 4, P1 = ±1 notch, +1 = roda ke bawah,
   dari `drivers/mouse.c`) sudah diproduksi tapi `Window::run()` tidak pernah
   menanganinya.
3. **Popup/overlay**. Menu dropdown harus digambar di atas pohon widget dan
   mencuri/menahan klik — mode interaksi baru untuk `Window`.

Tema tidak berubah. Tidak ada file sumber baru, tidak ada perubahan Makefile.

## Perubahan file

| File | Perubahan |
|---|---|
| `include/libui.h` | ~30 deklarasi C ABI baru (ScrollView/ListView/Table/TreeView/Tab/MenuBar/Menu/Toolbar + `ui_window_add_bar`). |
| `libs/widget/` | `Painter::clip` + `text` per-sel; `Widget::on_scroll`/`track_hover`/`is_menu_bar`; base `Scrollable`; 8 class widget; `Window` popup + `add_bar` + EVENT_SCROLL + hover rework; wrapper extern "C". |
| `user_apps/widget_demo.c` | `main` ditulis ulang jadi showcase tab: MenuBar + Toolbar + Tab 5 panel. |
| `roadmap/GUI_ROADMAP.md` | Phase 8 → SELESAI. |
| `DOCUMENTATION/design/gui-phase8-widgets.md` | **Baru** — dokumen ini. |

## C ABI (include/libui.h)

```c
// Window
void ui_window_add_bar(ui_window_t* win, ui_widget_t* bar);   // bar full-width di puncak

// ScrollView — wadah scrollable generik
ui_widget_t* ui_scrollview_create(ui_window_t* win, int w, int h);
void ui_scrollview_set_child(ui_widget_t* w, ui_widget_t* child);

// ListView
ui_widget_t* ui_listview_create(ui_window_t* win, int w, int h);
void ui_listview_add_item(ui_widget_t* w, const char* label);
int  ui_listview_selected(ui_widget_t* w);
void ui_listview_set_change(ui_widget_t* w, ui_click_cb cb, void* u);

// Table
ui_widget_t* ui_table_create(ui_window_t* win, int w, int h);
void ui_table_add_column(ui_widget_t* w, const char* title, int width);
void ui_table_add_row(ui_widget_t* w, const char* const* cells, int n);
int  ui_table_selected(ui_widget_t* w);
void ui_table_set_change(ui_widget_t* w, ui_click_cb cb, void* u);

// TreeView
ui_widget_t* ui_treeview_create(ui_window_t* win, int w, int h);
void ui_treeview_add_node(ui_widget_t* w, const char* label, int depth, int expanded);
int  ui_treeview_selected(ui_widget_t* w);
void ui_treeview_set_change(ui_widget_t* w, ui_click_cb cb, void* u);

// Tab
ui_widget_t* ui_tab_create(ui_window_t* win, int w, int h);
void ui_tab_add(ui_widget_t* w, const char* title, ui_widget_t* panel);

// MenuBar + Menu
ui_widget_t* ui_menubar_create(ui_window_t* win);
ui_widget_t* ui_menubar_add_menu(ui_widget_t* bar, const char* title);  // return Menu
void ui_menu_add_item(ui_widget_t* menu, const char* label, ui_click_cb cb, void* u);

// Toolbar
ui_widget_t* ui_toolbar_create(ui_window_t* win);
void ui_toolbar_add_button(ui_widget_t* bar, const char* label, ui_click_cb cb, void* u);
```

## Toolkit internal (libs/widget/)

### 1. Painter::clip — scissor rect

```cpp
bool clip_on; int clip_x, clip_y, clip_w, clip_h;
void set_clip(int x, int y, int w, int h);  // clip_on = true
void clear_clip();                          // clip_on = false
// rect():  potong rect target dgn scissor sebelum gui_draw_rect
// text():  tanpa clip → gui_draw_text (fast path);
//          dgn clip → per-sel gui_draw_char, lewati sel di luar scissor
// image(): jepit dua loop blit ke scissor
```
Default (tanpa clip) = seluruh window, identik dgn behavior lama. libgui tetap
skip per-pixel canvas-OOB, jadi clip cukup memotong region tampil.

### 2. Widget — tiga virtual baru

```cpp
virtual bool on_scroll(int delta) { return false; }      // true = redraw
virtual bool track_hover(int mx, int my) { return false; }
virtual bool is_menu_bar() { return false; }             // MenuBar → true
```
`track_hover` perlu ada karena `set_hover(bool)` tidak membawa koordinat —
widget ber-isi (Menu, MenuBar, Toolbar, ListView, Table, TreeView) melacak
sub-elemen yang di-hover sendiri. `is_menu_bar` membedakan perlakuan bar saat
popup terbuka di `Window::run` (MenuBar switch/close vs Toolbar cukup close).

### 3. Scrollable — base 4 widget scrollable

```cpp
enum { BAR_W = 6, ROW_H = 20 };
int scroll, scroll_max;
bool bar_drag; int bar_grab_y, bar_grab_scroll;

void set_scroll_view(int content_h, int view_h);  // max(0, content_h - view_h), clamp scroll
void set_scroll_max(int content_h) { set_scroll_view(content_h, h - BAR_W); }
bool bar_hit(int mx) const;                 // mx di [x+w-BAR_W, x+w]
int  content_w() const;                     // w-BAR_W bila bar tampil, w bila tidak

bool on_scroll(int delta);       // scroll += delta*ROW_H; clamp [0,scroll_max]; true bila berubah
void on_click(int mx, int my);   // bar_hit → mulai bar drag + map; else on_content_click
bool on_drag(int mx, int my);    // geser thumb selama bar_drag
void on_release();               // bar_drag = false
virtual void on_content_click(int mx, int my);  // hook subclass; default fire click_cb
void draw_bar(Painter& p);       // track button_bg + thumb accent (proporsional, min 8px)
```
Roda + drag thumb di-sentralisasi; subclass tinggal menimpa `on_content_click`
dan memotong `draw()`-nya. `Table` memakai `set_scroll_view` dengan
`view_h = h - HEADER_H - BAR_W` karena headernya tetap (tidak ikut scroll).

### 4. Kelas widget (gaya Label/Button yang ada; teks via `_ui_strdup`)

- **ScrollView** — `Widget* child`. `draw`: posisi `child->x=x, child->y=y-scroll`,
  gambar dua pass (pertama untuk arrange — VBox menghitung `h`-nya di sini —
  lalu `set_scroll_max(child->h)`, kedua dgn lebar `content_w()`), keduanya
  ter-clip viewport agar isi panjang tidak bocor keluar; `draw_bar`.
  `on_content_click`: teruskan ke `child->pick(mx,my)` → `on_click`.
- **ListView** — `char* items[32]; int n, selected=-1, hover_row=-1`. Row 20px,
  lebar penuh widget. `draw`: clip; baris tampil dari `scroll`; selected →
  `button_bg`, hover → `button_hover`, teks `fg`; `draw_bar`.
  `on_content_click`: `row=(scroll+my-y)/ROW_H`; dalam range → `selected=row`,
  fire `change_cb`. `track_hover`: set `hover_row`.
- **Table** — `HEADER_H=24`, `col[8]+col_w[8]/ncols`, `cells[64][8]/nrows`.
  Header tetap (`button_bg` + judul `button_fg` + rule 1px `fg`); baris 20px
  scroll, teks tiap sel dipotong ke kolomnya (per-sel `set_clip`).
  `on_content_click`: offset `HEADER_H` → select + `change_cb`.
- **TreeView** — `Node { char* label; int depth; bool expanded; } nodes[32]`.
  Flat pre-order. `visible_node(i)`: node tersembunyi bila ancestor terdekatnya
  (node `j<i` depth lebih kecil) collapsed. Indent `depth*12`; marker
  `'+'`/`'-'` di indent (font 8x16 tanpa segitiga); klik marker → toggle +
  `recompute_scroll()`, klik label → select + `change_cb`. O(n²), n≤32.
- **Tab** — `titles[8]+panels[8]/n/active`, `STRIP_H=26`. Strip (tab aktif →
  `button_bg` + underline `accent` 2px, lain `bg`), panel aktif diposisikan di
  `(x, y+26, w, h-26)` dan di-clip. `pick`: strip → `this`, else panel aktif.
  `on_click`: strip → `active=i`, else teruskan ke child ter-pick. Tab
  **memiliki** panelnya (didelete saat destroy).
- **Toolbar** — `Btn { label, cb, data } btns[16]`, `h=28`, `w=lebar window`.
  `draw`: tombol sama lebar (`button_bg`, hover → `button_hover`, gap 2px),
  label di tengah. `on_click`: `idx=(mx-x)/(w/n)` → fire `cb`.
- **Menu** (popup) — `Item { label, cb, data } items[16]`, `w=140, h=n*20+4`,
  `Window* win`. `draw`: `button_bg` + border `fg` + item (hover →
  `button_hover`). `track_hover`: set `hover_idx`. `on_click` (definisi
  out-of-class, butuh Window lengkap): `idx=(my-y-2)/20`; in range → fire
  `items[idx].cb(data)` **lalu** `win->close_popup()`.
- **MenuBar** — `Title { label, Menu* } titles[8]`, `h=24`, `w=lebar window`,
  `Window* win`. `add_menu` → `new Menu(win)`. `is_menu_bar()→true`.
  `on_click` (out-of-class): `idx=title_at(mx)`; bila menu itu yang sedang
  terbuka → `close_popup()` (toggle), else `open_popup(menu, x+idx*title_w, y+h)`.
  `draw` (out-of-class): rect title (terbuka/hover → `button_hover`) + label.
  dtor menghapus menu anak.

### 5. Window — popup + top bars + roda

```cpp
Widget* popup = 0;                   // overlay digambar paling atas
Widget* top_bars[4]; int n_bars = 0; // bar full-width (MenuBar/Toolbar)
int bar_h = 0;                       // tinggi kumulatif bar → offset root

void add_bar(Widget* b);     // b->x=0; b->y=bar_h; bar_h+=b->h; if (root) root->y = 8+bar_h;
void open_popup(Widget* m, int ox, int oy);  // popup=m; posisikan; render
void close_popup();                         // popup=0; hover popup dibersihkan

render():  bg → draw top_bars → draw root → draw popup (teratas) → gui_flush

track_hover(): popup terbuka → hover hanya popup atau bar (root TIDAK di-hover);
               else → bar (reverse) lalu root; lalu hovered->track_hover(mx,my).

run():
  MOVE       → sama; track_hover() yang di-ulang juga fire popup/bar track_hover
  CLICK ↓    → popup terbuka:
                   popup->pick ? popup->on_click (item menu: cb + close)
                   bar hit:
                       is_menu_bar → bar->on_click (switch/close)
                       else        → close_popup(); bar->on_click
                   else → close_popup() (klik luar → dismiss)
               popup tutup: grab dari pick_bar lalu root (bar bukan anak root!)
  CLICK ↑    → on_release + grab=0
  KEY_PRESS  → ESC: popup ? close_popup : running=false
  SCROLL     → if (hovered && hovered->on_scroll(ev.param1)) render();   // BARU
  WIN_CLOSE  → running=false
```

`VBox::arrange` kini menghitung `h`-nya sendiri (`h = cy - y`) — dipakai
`ScrollView` untuk tahu berapa tinggi konten yang harus di-scroll.

## Demo (user_apps/widget_demo.c)

Window ±360×400, tema sama, `add_bar` menubar + toolbar, lalu root VBox:

- **MenuBar**: "File" (Baru, Buka, Tutup) + "Edit" (Salin, Tempel) — item
  callback → status label `"Menu: File > Baru"`.
- **Toolbar**: "Muat", "Simpan", "Cari" → status label.
- **Status label** (di atas tab).
- **Tab** (320×300) 5 panel:
  - **Kontrol**: regresi Phase 7 — counter `+1`, TextBox + Enter-echo, CheckBox,
    Slider→ProgressBar, Image(kyuzen.png 64×64).
  - **Daftar**: ListView 20 item (scroll) → status `"Daftar: Item N"`.
  - **Tabel**: Table 3 kolom (Nama/Status/Nilai) × 13 baris → status `"Tabel: baris N"`.
  - **Pohon**: TreeView depth 0-2 (cabang collapsed) → expand/collapse,
    select → status `"Pohon: node N"`.
  - **Gulir**: ScrollView membungkus VBox 15 label.

## Verifikasi (manual — user verifikasi di QEMU)

1. `make` → build clean (kernel + semua apps).
2. `start widget_demo` → window: MenuBar + Toolbar puncak, status, Tab.
3. **MenuBar**: klik "File" → dropdown; hover highlight item; klik "Baru" →
   status update + menu tutup; klik luar → dismiss; ESC tutup menu (bukan
   window); klik "Edit" → switch menu; klik judul menu yang terbuka → tutup.
4. **Toolbar**: klik tombol → status update; hover highlight.
5. **Tab**: klik tiap tab → panel switch (strip highlight + underline accent).
6. **ListView/Table/TreeView/ScrollView**: roda scroll → konten geser + thumb
   bergerak; drag thumb; klik baris → select + status.
7. **TreeView**: klik `+`/`-` → expand/collapse anak (tersembunyi/tampil).
8. **Regresi (Kontrol)**: `+1`, TextBox fokus/ketik/Enter-echo, CheckBox,
   Slider→ProgressBar, Image tetap bekerja. ESC menutup window.

## File

- `include/libui.h` — C ABI 8 widget baru + `ui_window_add_bar`.
- `libs/widget/` — toolkit C++: Painter::clip, Scrollable, 8 widget, Window popup/bar/scroll, wrapper extern "C".
- `user_apps/widget_demo.c` — demo showcase tab.
- `roadmap/GUI_ROADMAP.md` — Phase 8 → SELESAI.

## Belum ada (Phase 9)

Clipboard, Dialog, Notification, Drag & Drop, Cursor, Shortcut, Settings.
Sengaja tidak dibangun sekarang (Prinsip #1 Start Simple, #2 Grow Naturally).
