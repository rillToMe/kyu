# UI Color & Widget Audit

> Status: AUDIT SAJA. Tidak ada production code diubah.
> Source of truth: source code aktual repo (bukan dokumentasi lama).
> Verifikasi: `libs/gui/color/`, `libs/gui/widget/include/**`, `libs/core/libgui.c`,
> `include/libui.h`, `include/libgui.h`, `apps/*`, `system/desktop/*`.
> Catatan: audit sebelumnya (`docs/design/gui/color-api-audit.md`) menyatakan
> `COLOR_HEX` belum ada — saat ini `COLOR_HEX(0xRRGGBB)` SUDAH ada di
> `libs/gui/color/include/color_types.h:86-88` dan dipakai theme + apps.

## 1. Executive Summary

- Representasi warna sehat: `color_t {r,g,b,a}` + `COLOR_HEX(0xRRGGBB)` + serialisasi
  `FORMAT_ARGB` ke framebuffer XRGB. Tidak ada inkonsistensi RGB/BGR di jalur widget.
- Theme system: PARTIAL (bukan ABSENT, bukan PRESENT penuh). Ada `ui_theme_t` 6 field
  + `ui::Theme` 6+8 field + `derive()` + persist `settings.ui` v1/v0. Tidak ada
  foreground/background/surface/border/text-secondary/disabled/selection/focus/
  danger/warning/success sebagai role bernama — semua di-derive dari 6 warna dasar
  atau hardcode di `derive()`.
- Semua widget theme-aware (ambil `p.theme.*`), TAPI 8 warna turunan di-hardcode di
  `theme.hpp:61-71` (`0x252526`, `0x2D2D2D`, `0x3C3C3C`, `0x333333`, `0x454545`,
  `0xDCDCAA`, `0x00E5FF`) + `COLOR_BLACK` dipakai langsung untuk border/shadow +
  default prompt `0x7CC7FF`. Ganti accent app tidak mengubah turunan ini.
- Tidak ada per-widget color setter (`ui_button_set_color` dkk TIDAK ADA di ABI).
  API color-oriented rendah tidak ada; masalahnya sebaliknya — API terlalu
  theme-sempit (6 warna untuk ~20 widget) sehingga turunan di-hardcode.
- Blue dependency adalah default, bukan aksen opsional: `button_bg 0x0F3460` +
  `button_hover 0x2A4A7E` adalah default `Theme()` dan dipakai Button, ListView,
  Table, TreeView, GridView, Menu, Scrollbar. Desktop (`system/desktop/theme.hpp`,
  ~30 warna) sistem warna KEDUA yang paralel, tidak memakai `color_t`.
- Dark/Light readiness: semua widget NOT READY sebagai properti sendiri — readiness
  = readiness theme yang di-load. Widget tidak hardcode terang/gelap, tapi turunan
  `derive()` selalu charcoal gelap sehingga theme terang pecah (panel/chrome/divider
  tetap gelap).
- AI-looking root causes (source-verified): 1 font bitmap 8x16 untuk semua teks UI,
  border 1px flat di semua widget, tidak ada elevation selain shadow Menu/Dialog,
  spacing tidak terstandar (VBox spacing per call-site, margin root 8px hardcode),
  selected == button_bg di 4 widget (seleksi tidak dibedakan dari tombol).

## 2. Color Representation

- Tipe: `color_t { uint8_t r, g, b, a }` (`libs/gui/color/include/color_types.h:15-17`).
  Field order R,G,B,A. Bukan integer tunggal.
- Literal C: `COLOR_HEX(0xRRGGBB)` = braced-list `{r,g,b,255}` (init position:
  `static const`, member-init, local init). BUKAN ekspresi — argumen fungsi /
  assignment runtime pakai `COLOR_RGB(r,g,b)` / `COLOR_RGBA(r,g,b,a)` /
  `color_make()` (`color_types.h:28-34,86-88`).
- Semantik channel TETAP `0xRRGGBB` (didok di header). `0xAARRGGBB` hanya format
  serialisasi pixel (`color_to_u32(..., FORMAT_ARGB)`), bukan konstruktor.
- Konstruktor init ganda: `COLOR_RGB_INIT` / `COLOR_RGBA_INIT` (constant expression)
  vs `COLOR_RGB` / `COLOR_RGBA` (runtime inline). Nilai identik, peran beda.
- Alpha: straight (non-premultiplied), `a=0` transparan, `a!=0` opaque
  (`color_types.h:9-10`). `color_with_alpha`, `color_opaque` tersedia.
  SEMUA permukaan window opaque: `Theme::set()` paksa `color_opaque()`
  (`theme.hpp:38-43`); `libgui _lgui_px()` paksa `a=255` (`libs/core/libgui.c:85-88`).
- Byte order: struct field-based = endian-independent. Serialisasi shift-based:
  `FORMAT_ARGB` -> `0xAARRGGBB` (display KyuzenOS); `FORMAT_RGBA/ABGR/BGRA` untuk
  varian hardware (`color_types.h:50-62`). Decode balik `color_from_u32`.
- Conversion: `color_to_u32` / `color_from_u32` (per format). HSL/HSV integer-only
  (`color_space.h`: `h 0..359`, `s/l/v 0..255`, round-trip error max +-3).
- Blending: `color_blend_alpha` integer (`color_blend.h:23-38`), hasil selalu
  `a=255`; `color_div255` tanpa divide; `color_blend_span` batch + seam SIMD.
  Painter `blend/vgrad/rrect_grad/image` duplikasi rumus yang sama di atas
  `color_from_u32(..., FORMAT_ARGB)` (`painter.hpp:120-138`).
- Opacity: via `color_with_alpha` (coverage AA, shadow ring alpha 52/38/24/12,
  border alpha 55/90). Tidak ada opacity per-widget di ABI.
- Helpers: `color_darken` / `color_lighten` (skala 0..255),
  `color_get_contrast_text` (luma Rec.601, threshold 128) — dipakai? `darken/lighten`
  dipakai Button + Menu-disabled; `get_contrast_text` TIDAK dipakai widget mana pun
  (dead API untuk UI saat ini).
- Palet: `COLOR_TRANSPARENT/BLACK/WHITE/RED/GREEN/BLUE/YELLOW/CYAN/MAGENTA/GRAY`
  (`color_utils.h:13-22`) + `*_INIT` untuk 3 di antaranya. Widget hanya pakai
  `COLOR_BLACK` (border/shadow) dan `COLOR_WHITE` (default `button_fg`). `COLOR_BLUE`
  dkk tidak dipakai widget.
- Parsing warna: TIDAK ADA di `libs/gui/color`. Satu-satunya parser adalah
  `system/desktop/launcher.cpp:65-89 parse_color()` (`0x...`/desimal -> desktop
  `Color`, bukan `color_t`).
- Inkonsistensi RGB/BGR/XRGB/ARGB: TIDAK ADA di jalur widget. Dua dunia paralel
  tetap ada (dicatat audit sebelumnya, masih berlaku): dunia `color_t` (compositor,
  libgui, widget, apps C) vs dunia `uint32_t 0xRRGGBB` mentah (TTY, fb, panic, boot,
  timer) vs dunia ketiga `kyuzen::desktop::Color + rgb()` (system/desktop, ~30 const).

## 3. Color Inventory

| Color | Location | Usage | Hardcoded? | Notes |
|---|---|---|---|---|
| `0x1A1A2E` | `widget/include/core/theme.hpp:28` | `Theme.bg` default | Ya (`COLOR_HEX`) | Dasar window default (navy gelap) |
| `0xE0E0E0` | `theme.hpp:28` | `Theme.fg` default | Ya | Teks umum default |
| `0xE94560` | `theme.hpp:29` | `Theme.accent` default | Ya | Aksen default (raspberry, bukan biru) |
| `0x0F3460` | `theme.hpp:29` | `Theme.button_bg` default | Ya | SUMBER blue global; = editor surface juga (`editor = button_bg`) |
| `0xFFFFFF` | `theme.hpp:30` (`COLOR_WHITE`) | `Theme.button_fg` default | Ya (palet) | Teks tombol default |
| `0x2A4A7E` | `theme.hpp:30` | `Theme.button_hover` default | Ya | SUMBER blue global #2; hover + seleksi TextEdit + Menu hover + drag ghost |
| `0x252526` | `theme.hpp:61` | `Theme.panel` (derive) | Ya, konstanta | Isi modal + popup menu. Selalu gelap walau theme terang |
| `0x2D2D2D` | `theme.hpp:62` | `Theme.chrome` (derive) | Ya, konstanta | Menubar + statusbar. Selalu gelap |
| `0x3C3C3C` | `theme.hpp:63` | `Theme.btnfill` (derive) | Ya, konstanta | Tombol dialog |
| `0x333333` | `theme.hpp:64` | `Theme.divider` (derive) | Ya, konstanta | Garis pemisah 1px semua widget |
| `0x454545` | `theme.hpp:65` | `Theme.mborder` (derive) | Ya, konstanta | Border modal |
| `0xDCDCAA` | `theme.hpp:70` | `Theme.acc_text` (derive) | Ya, konstanta | Shortcut menu + baris aksen dialog (amber VS Code) |
| `0x00E5FF` | `theme.hpp:71` | `Theme.caret` (derive) | Ya, konstanta | Caret TextEdit + PromptDialog (cyan) |
| `0x7CC7FF` | `widget/include/editor/textedit.hpp:54` | `ps1_color` default | Ya | Prompt terminal default; `terminal.c:17` duplikasi via `COLOR_PROMPT` |
| `0x000000` | `painter.hpp:194-197`, `button.hpp:33-35` (`COLOR_BLACK`) | Border tombol (alpha 55/90), shadow ring (52/38/24/12), inset pressed (60) | Ya (palet) | Satu-satunya palet mentah di render path |
| `0xF5F5F5` | `libs/core/libgui.c:159` | Canvas awal window biasa | Ya (`COLOR_RGB`) | Ter-overwrite saat `Window::render` isi `theme.bg` |
| `0x1E293B` | `libs/core/libgui.c:203` | Canvas awal desktop | Ya | Wallpaper default sebelum app gambar |
| `0x333333` | `libs/core/libgui.c:335` | `gui_draw_bar` background | Ya | Legacy bar API; widget ProgressBar tidak pakai ini |
| `0x2D2D2D` | `apps/notepad.c:57` | `NOTEPAD_THEME.bg` | Ya (app theme) | App override, bukan global |
| `0xD4D4D4` | `apps/notepad.c:58,62` | `NOTEPAD_THEME.fg/button_fg` | Ya | Off-white |
| `0x0098BC` | `apps/notepad.c:59` | `NOTEPAD_THEME.accent` | Ya | Biru aksen halus (lihat §4) |
| `0x1E1E1E` | `apps/notepad.c:61` | `NOTEPAD_THEME.button_bg` (= editor) | Ya | Charcoal editor |
| `0x3E3E42` | `apps/notepad.c:63` | `NOTEPAD_THEME.button_hover` | Ya | Hover + seleksi notepad |
| `0x0B0D10` | `apps/terminal.c:141,144` | `terminal bg/button_bg` | Ya | Hampir hitam |
| `0xD7DCE2` | `apps/terminal.c:142,145` | `terminal fg/button_fg` | Ya | |
| `0x16191D` | `apps/terminal.c:146` | `terminal button_hover` | Ya | |
| `0x121212` | `apps/widget_demo.c:138`, `apps/settings/appearance.cpp:9` | `tema_gelap/kThemeDark bg` | Ya | Duplikat 2 file (sama persis) |
| `0xF0F0F0` | `widget_demo.c:143`, `appearance.cpp:14` | `tema_terang/kThemeLight bg` | Ya | Satu-satunya theme terang; turunan derive tetap gelap = pecah |
| `0x222222` | `widget_demo.c:143,145` | light fg/button_fg | Ya | |
| `0xD32F2F` | `widget_demo.c:144`, `appearance.cpp:15` | light accent | Ya | Merah |
| `0xCFD8DC` | `widget_demo.c:144`, `appearance.cpp:15` | light button_bg | Ya | Blue-gray terang |
| `0x90A4AE` | `widget_demo.c:145`, `appearance.cpp:16` | light button_hover | Ya | |
| `0x0D1F14` | `widget_demo.c:148`, `appearance.cpp:19` | `tema_hijau/kThemeGreen bg` | Ya | |
| `0x4CAF50` | `widget_demo.c:149`, `appearance.cpp:20` | green accent | Ya | |
| `0x1B4D2E` | `widget_demo.c:149`, `appearance.cpp:20` | green button_bg | Ya | |
| `0xE8F5E9` / `0x2E7D46` / `0xDFF2E0` | `widget_demo.c:150`, `appearance.cpp:21` | green fg/button_fg/hover | Ya | |
| `0xE8E8EC` | `apps/fontdemo.c:125`, `apps/settings/fonts.cpp:37` | FtText draw callback fg | Ya | Bukan theme; hardcode di callback app |

## 4. Blue Color Inventory

| Color | Location | Widget | Purpose |
|---|---|---|---|
| `0x0F3460` | `theme.hpp:29` (default `button_bg`) | Button, ListView selected, Table selected/header, TreeView selected, GridView selected/placeholder, Scrollable track, ScrollView empty, Image empty, TextBox bg, CheckBox box | GLOBAL BLUE. Latar tombol + latar selected + track scrollbar + bg textbox. Juga = `editor` surface (`editor = button_bg`) |
| `0x2A4A7E` | `theme.hpp:30` (default `button_hover`) | Button hover, Menu hover row, MenuBar open/hover title, ListView/Table/TreeView/GridView hover, Dialog hover btn, TextEdit/TextBox selection block, drag ghost bg | GLOBAL BLUE. Hover + seleksi teks. State-specific tapi dipakai lintas widget |
| `0x0098BC` | `apps/notepad.c:59` (`NOTEPAD_THEME.accent`) | CheckBox centang, Slider handle/thumb, ProgressBar fill, Scrollbar thumb, Tab active underline, TextBox focus border/caret, TreeView +/- marker, Dialog top strip | APP-SPECIFIC BLUE. Via role `accent`, tapi karena 8 widget baca `accent`, satu nilai ini mewarnai semua state aksen notepad |
| `0x7CC7FF` | `textedit.hpp:54` default; `terminal.c:17` `COLOR_PROMPT` | TextEdit prompt prefix | WIDGET+APP BLUE. Default toolkit diduplikasi app; `set_prompt_style` bisa override |
| `0x00E5FF` | `theme.hpp:71` (`caret`, konstanta derive) | TextEdit caret, PromptDialog caret | STATE-SPECIFIC (cyan). Konstanta, tidak ikut accent app |
| `0x1E293B` | `libgui.c:203` | Desktop window (canvas awal) | APP-SPECIFIC (slate). Bukan widget |
| `0x2E4A8E` | `system/desktop/theme.hpp:66,76,115` (`ICON_SEL`, `MENU_HOVER`, `TASK_ACTIVE`) | Desktop launcher/menu/taskbar | DESKTOP-SYSTEM BLUE (dunia `Color` terpisah, bukan `color_t`) |
| `0x2A385E` / `0x222C4C` / `0x1A2138` | `theme.hpp:65,116,114` | Desktop icon hover / task hover / task btn | DESKTOP-SYSTEM BLUE |
| `0x9CB8FF` | `theme.hpp:67` (`ICON_SEL_EDGE`) | Desktop icon selection outline | DESKTOP-SPECIFIC light blue |
| `0x12162A` / `0x0B0E1C` / `0x141A2E` / `0x1E2848` | `theme.hpp:74,112,109,110` | Desktop menu/taskbar/wallpaper | DESKTOP navy background blues |
| `0x3A4C80` / `0x2A3355` | `theme.hpp:75,113` | Desktop menu/taskbar edge | DESKTOP edge blues |
| `0xCFD8DC` / `0x90A4AE` | `widget_demo.c:144-145`, `appearance.cpp:15-16` | Button/hover saat light theme aktif | APP-SPECIFIC blue-gray (satu-satunya hover terang) |
| `0x0000FF` (`COLOR_BLUE` palet) | `color_utils.h:18` | — (tidak dipakai widget/desktop) | Palet mati untuk UI saat ini |

Kelompok: global blue = `0x0F3460` + `0x2A4A7E` (default theme, semua widget);
widget-specific blue = tidak ada (tidak ada widget punya biru sendiri — semua via
theme role); state-specific = hover (`button_hover`), selected (== `button_bg`,
kecuali Menu/Dialog yang pakai `button_hover`), focus (`accent`), caret
(`0x00E5FF` konstanta); application-specific = notepad `0x0098BC`, terminal
`0x7CC7FF`, seluruh `system/desktop/theme.hpp` (dunia terpisah).

## 5. Existing Theme System

Status: **PARTIAL**

Evidence:

- ABI: `ui_theme_t` 6×`color_t` (`bg, fg, accent, button_bg, button_fg,
  button_hover`) (`include/libui.h:44-51`). Alpha diabaikan painter.
- Runtime: `ui::Theme` 6 + 8 turunan (`editor, chrome, panel, btnfill, divider,
  mborder, acc_text, caret`) (`theme.hpp:25-27`). `set()` copy + `color_opaque()` +
  `derive()`; `to_abi()` untuk persist (`theme.hpp:33-54`).
- Ownership: per-`Window` (`Window::theme`; `set_theme()` via
  `ui_window_set_theme`, `damage_full`) (`window.hpp:43,103-107`). Bukan global,
  bukan per-widget. Widget akses via `Painter::theme` (`const Theme&`, per-render).
- Mutable: ya (`set_theme` kapan saja; `settings.ui` load/save). Tidak ada
  appearance-mode flag; "mode" = nilai 6 warna yang di-load.
- Persist: `settings.ui` v1 (`KTH1` + 28B) + baca legacy v0 (24B `0x00RRGGBB`) +
  tolak blob kosong (`window.hpp:269-304`, `libui.h:437-444`).
- Yang TIDAK ada: `ui_theme` manager global, palette bernama, color roles selain 6
  itu, dark/light flag, accent terpisah dari `accent` tunggal, per-widget override
  (tidak ada setter warna di ABI — diverifikasi `libui_abi.cpp`: hanya
  `ui_textedit_set_prompt_style` yang menerima `color_t`, itu pun untuk prefix
  prompt terminal, bukan theming).
- `derive()` adalah hardcode berkedok theme: 8 warna konstanta charcoal/amber/cyan
  (`theme.hpp:60-71`). Komentar mengklaim "turunan dihitung sekali per set_theme"
  tapi 5 di antaranya literal tetap, bukan fungsi dari 6 input (kecuali `editor =
  button_bg`). Ini blocker light mode (§10).

## 6. Widget Audit

| Widget | Render File | Colors | States | Theme-aware | Issues |
|---|---|---|---|---|---|
| Window (bg) | `window/window.hpp:404-406` | `theme.bg` full-fill | — | Ya | Margin root 8px hardcode; bg flat tanpa gradasi |
| Button | `primitives/button.hpp:25-37` | `button_bg/hover` + darken/lighten gradien, border `COLOR_BLACK` a55/90, text `button_fg` | normal/hover/pressed | Ya | Radius 6px satu-satunya widget rounded; h=28 fixed; disabled/focused tidak ada |
| Label | `primitives/label.hpp:27` | `theme.fg` | — (statis) | Ya | Auto-width `len*8`, h=16; tidak ada secondary/disabled/error |
| TextBox | `primitives/textbox.hpp:50-63` | bg `button_bg`, border `fg`/`accent`(focus), selection `button_hover`, caret `accent`, text `fg` | normal/focused/(replace_next sbg selected semu) | Ya | Border 1px full-box; h=24 fixed; disabled tidak ada |
| CheckBox | `primitives/checkbox.hpp:32-43` | box `button_bg` + border `fg`, centang `accent` (pixel diagonal), label `fg` | normal/checked | Ya | Box 12px; tidak ada hover/focus/disabled; centang gambar piksel, bukan glyph |
| Slider | `primitives/slider.hpp:61-66` | track `button_bg`, handle `accent` | normal/dragging | Ya | Track 4px, handle 8px full-height block; w=160 h=20 default; tidak ada disabled/focus |
| ProgressBar | `primitives/progressbar.hpp:23-27` | track `button_bg`, fill `accent` | — (read-only 0..100) | Ya | Tanpa border/teks; h=16; `gui_draw_bar` legacy (`0x333333`) tidak dipakai |
| TextEdit | `editor/textedit.hpp:522-576` | bg `editor`, border `divider`, selection `button_hover`, text `fg`, prompt `ps1_color`, caret `caret 0x00E5FF` 2px | normal/focused/readonly/selection/wrap | Ya | Buffer 8K; font 8x16; undo op-based; caret 2px vs TextBox 1px (inkonsisten) |
| Image | `primitives/image.hpp:57-60` | empty `button_bg`, else PNG pixels | — (empty/loaded) | Sebagian | Nearest-neighbor; gagal decode = kotak kosong; zoom 10..400 |
| FtText | `primitives/fttext.hpp:33-44` | Warna milik callback app (toolkit tidak tentukan) | — | Tidak (by design) | Kontrak ukur-sendiri; clip manual |
| ListView | `containers/listview.hpp:71-83` | selected `button_bg`, hover `button_hover`, text `fg` | normal/hover/selected | Ya | Row 20px; MAX 32 items; selected==button_bg (sama dgn tombol) |
| Table | `containers/table.hpp:131-168` | header `button_bg` + text `button_fg`, divider `divider`, selected `button_bg`, hover `button_hover`, cell `fg`, empty `button_fg` | normal/hover/selected/empty | Ya | Header 24px + row 20px; MAX 8 col/64 row; ikon 16px non-owning |
| TreeView | `containers/treeview.hpp:94-112` | selected `button_bg`, hover `button_hover`, marker `accent`, label `fg` | normal/hover/selected/expanded | Ya | Indent 12px/depth; marker `+`/`-` teks (tanpa segitiga); MAX 32 nodes |
| Tab | `containers/tab.hpp:71-88` | active `button_bg` + underline `accent` 2px, inactive `bg`, text `fg` | normal/active | Ya | Strip 26px; judul `w/n` rata (bukan fit-teks spt MenuBar); MAX 8 |
| Scrollable/Scrollbar | `containers/scrollable.hpp:86-94` | track `button_bg`, thumb `accent` | normal/drag | Ya | BAR_W 6px; thumb min 8px; proporsional sederhana |
| ScrollView | `containers/scrollview.hpp:240-259` | empty `button_bg`; bar = Scrollable | normal/pan | Ya | Pan mode (2-axis + center + drag-to-pan + keyboard); non-pan vertikal saja |
| GridView | `containers/gridview.hpp:301-343` | selected `button_bg`, hover `button_hover`, placeholder `button_bg`(+error `button_hover`+`!`), label `fg` | normal/hover/selected/placeholder/empty | Ya | Cell 148x138, thumb box 112, PAD 4; MAX 128; virtualisasi visible-range |
| Menu (popup) | `chrome/menu.hpp:132-160` | bg `panel`, border `mborder`, hover `button_hover`, checked box `accent`, text `fg`, disabled `darken(fg,55%)`, acc `acc_text` | normal/hover/disabled/checked/separator | Ya | Row 22px, sep 9px; MAX 20; satu-satunya widget dengan disabled state |
| MenuBar | `chrome/menubar.cpp:14-25` | bg `chrome` + divider, open/hover `button_hover`, text `button_fg` | normal/hover/open | Ya | h=24; judul fit-teks (`len*8+22`, x+6); kontras dgn Toolbar |
| Toolbar | `chrome/toolbar.hpp:53-62` | bg `bg`, hover `button_hover`, text `fg` | normal/hover | Ya | h=28; tombol `w/n` rata; bg = `bg` (bukan `chrome` spt MenuBar — inkonsisten) |
| StatusBar | `chrome/statusbar.hpp:31-41` | bg `chrome` + divider top, text `button_fg` | — (statis) | Ya | h=20; kiri + kanan; teks selalu `button_fg` (bukan `fg`) |
| Dialog | `dialog/dialog.hpp:93-132` | bg `panel`, top strip `accent` 2px, border `mborder`, title `button_fg`, body `fg`, aksen `#> ` `acc_text`, divider, btn `btnfill`/`button_hover` | normal/hover(btn) | Ya | Min 300px; shadow via `Painter::shadow`; satu-satunya + Menu dgn elevation |
| PromptDialog | `dialog/promptdialog.hpp:80-91` | + input `editor` + border `mborder` + caret `caret` 2px | normal/focus(input implisit) | Ya | Input 24px, PAD_X 22 (klik/caret satu sumber); OK/Batal |
| Toast | `window/window.hpp:306-314` | bg `panel` + border `mborder`, text `fg` | — (timed) | Ya | 210x28 kanan-atas; auto-expire `sys_uptime` |
| Drag ghost | `window/window.hpp:315-324` | bg `button_hover` + border `accent`, text `fg` | — | Ya | Hover-colored box mengikuti kursor |
| Separator (standalone) | — | — | — | — | TIDAK ADA (separator hanya `divider` 1px di dalam Menu/Dialog/StatusBar/Table-header) |
| Radio | — | — | — | — | TIDAK ADA |
| ComboBox/Select | — | — | — | — | TIDAK ADA |
| Tooltip | — | — | — | — | TIDAK ADA |
| Sidebar/Header/Container/Panel/Icon | — | — | — | — | TIDAK ADA sebagai widget (FileManager "sidebar" = ListView; "panel" = Dialog `panel` color; ikon = Table/GridView pixels) |

## 7. Widget-by-Widget Findings

- Button (`button.hpp`): satu-satunya rounded (r=6) + gradien ±10% + border alpha.
  Geometri `len*8+24 × 28`, teks center, pressed geser 1px + inset shadow. States:
  normal/hover/pressed lengkap; focused/disabled/selected tidak ada. Isu: terlihat
  paling "designed" tapi gayanya tidak dipakai widget lain (tombol Dialog flat).
- Label: `len*8 × 16`, `theme.fg`. Tidak ada wrap/align/secondary.
- TextBox (`textbox.hpp`): h=24, padding teks 4px, border 1px `fg`->`accent` saat
  fokus, caret 1px `accent`, selection-block `button_hover` (semantik replace_next
  gaya Explorer). Clipboard Ctrl+C/X/V. Isu: border full-box 1px `fg` terlihat
  "demo"; caret 1px vs TextEdit 2px.
- CheckBox: box 12px + label +20px, h=20. Centang = 2 garis diagonal `accent`
  (pixel art, bukan glyph — font bitmap tak punya ✓). Tanpa hover/focus/disabled.
- Slider: default 160×20, track 4px `button_bg` di tengah, handle 8px `accent`
  setinggi widget (block, bukan knob). Tanpa focus/disabled/tick.
- ProgressBar: `w×16`, flat `button_bg` + fill `accent`. Tanpa border/teks/animasi.
- TextEdit: editor paling matang (8K buffer, undo op 64, seleksi, clipboard, wrap
  word-based, prompt berwarna, readonly terminal-lock). Render: bg `editor` +
  border `divider` 1px, teks 8px/col 16px/row, margin 4px, caret 2px `0x00E5FF`.
  Default `ps1_color 0x7CC7FF` hardcode (satu-satunya default warna di luar theme).
- ListView/Table/TreeView/GridView: pola seleksi identik — selected `button_bg`,
  hover `button_hover`, teks `fg`. Artinya selected == warna tombol, dan di notepad
  (`button_bg 0x1E1E1E`) seleksi nyaris tak terlihat di atas editor. Row 20px
  seragam (kecuali Menu 22px, Table header 24px, Tab strip 26px). Kapasitas kecil
  (32/64/32/128) — cukup untuk audit, dicatat untuk migrasi.
- Tab: active = `button_bg` + garis `accent` 2px; inactive = `bg`. Judul dibagi rata
  (`w/n`) — vs MenuBar fit-teks. Panel di-clip 26px di bawah strip.
- Menu: satu-satunya dengan disabled (`darken(fg, SHADE_55)`) + checked (`accent`
  8px box) + separator (`divider`, inset 8px) + accelerator kanan (`acc_text`
  amber). Row 22px/sep 9px, lebar dari isi (min 130, awal 150). Paling hierarkis.
- MenuBar vs Toolbar inkonsisten: MenuBar bg `chrome` + teks `button_fg` + judul
  fit-teks; Toolbar bg `bg` + teks `fg` + tombol rata `w/n`. Dua bar bertumpuk
  terlihat beda lapisan tanpa alasan.
- StatusBar: `chrome` + divider atas + teks `button_fg` kiri/kanan. h=20.
- Dialog/PromptDialog: `panel` + border `mborder` + strip `accent` 2px atas +
  divider di atas tombol; tombol flat `btnfill`/`button_hover` (beda dari Button
  bergradasi). Shadow 4-ring (`Painter::shadow`) — satu-satunya elevation
  sinyal. Min width 300, tinggi dari isi.
- Scrollbar: 6px, track `button_bg`, thumb `accent` proporsional (min 8px). Tipis
  dan selalu `accent` — di theme terang thumb tetap warna accent gelap/terang
  sesuai input, tapi track `button_bg` bisa kontras aneh.
- Toast/ghost: `panel`/`button_hover` + border `mborder`/`accent`. Fungsional.

## 8. Typography Findings

- Font UI: bitmap 8x16 built-in (`gui_draw_char/text`, `libs/core/libgui.c:103-127`).
  ASCII saja (>127 diabaikan). Semua widget: 8px/kolom, 16px/baris. Tidak ada ukuran/
  weight/style varian di ABI maupun theme.
- Hierarki nyaris nol: judul Dialog (`button_fg`) vs body (`fg`) beda warna tapi
  sama 16px; header Table (`button_fg`) sama 16px; Tab/Menu/Toolbar/StatusBar semua
  16px. Penekanan hanya via warna (`acc_text` amber, `accent` marker, prompt color).
- FreeType path terpisah: `FtText` + `kz_text_draw` (`libs/text`), dipakai
  `fontdemo.c` dan label desktop (`LABEL_FONT_PX 13`, `BOX_LBL_LINE_H 18` di
  `system/desktop/theme.hpp:61-62`). Widget toolkit TIDAK pakai FreeType untuk teks
  UI — dua jalur tipografi paralel.
- Text vertical centering manual per widget (`(h-16)/2`, `+4`, `+6`, `+2`) —
  tidak terstandar (Button +pressed 1px, Dialog title y+14, body y+36, Menu
  `(ROW_H-16)/2`, StatusBar `(h-16)/2`). Hasil: baseline tidak konsisten 1-2px.
- Empty-text (Table/GridView) di-center horizontal manual. Elipsis GridView = `..`
  (bukan `…`, font tak punya).

## 9. Spacing & Geometry Findings

- Root: selalu VBox margin 8px (`Window::add`, `window.hpp:120-124`); bar menambah
  `bar_h`. Tidak ada margin konfig di ABI.
- Spacing: per call-site (`ui_vbox/hbox_create(win, spacing)`; app pakai 0/6/8).
  Tidak ada scale spacing global. Contoh: filemanager path_row 6, content gap
  bervariabel, calc grid `BTN_PAD`, dialog btn gap 6, menu gutter 24/12.
- Padding teks: Button 12px horizontal (±24 total) / 6px vertikal (28-16);
  TextBox 4px; TextEdit 4px; Menu label x+24, aksen kanan -12; Dialog 16px;
  StatusBar 8px; Toolbar hover inset 2/3px. Tidak ada padding token.
- Border: 1px flat di semua (kecuali Button rounded-6 + border alpha). Lebar tidak
  bervariasi, tidak ada focus ring (fokus = ganti warna border, bukan outline).
- Radius: hanya Button 6px. Dialog/Menu/TextBox/Table semua persegi. Tidak ada
  radius token.
- Ukuran default tersebar: Button `len*8+24×28`; Label `len*8×16`; TextBox `w×24`;
  CheckBox box 12; Slider 160×20/handle 8; ProgressBar h=16; ListView row 20;
  Table header 24/row 20; Tab strip 26; Menu row 22/sep 9; MenuBar 24; Toolbar 28;
  StatusBar 20; GridView cell 148×138/thumb 112/PAD 4; Scrollbar 6px. Tidak ada
  size scale.
- Alignment: teks tombol center, label kiri, menu aksen kanan, dialog tombol center,
  empty-text center, grid label center, status kiri+kanan. Tidak ada align API.

## 10. Dark/Light Mode Readiness

| Widget | Dark | Light | Reason |
|---|---|---|---|
| Window bg | PARTIAL | PARTIAL | `theme.bg` diikuti, tapi margin/root flat; siap bila theme diisi benar |
| Button | PARTIAL | PARTIAL | `button_bg/hover/fg` diikuti + gradien relatif; border `COLOR_BLACK` a55/90 di theme terang = border hitam di atas terang (kasar tapi terlihat) |
| Label | READY | READY | Hanya `theme.fg` |
| TextBox | PARTIAL | PARTIAL | Role diikuti; selection `button_hover` di light theme (`0x90A4AE`) ok, tapi border `fg` 1px `0x222222` di atas `0xCFD8DC` = kontras keras gaya demo |
| CheckBox | PARTIAL | PARTIAL | Role diikuti; tanpa disabled state di kedua mode |
| Slider/ProgressBar/Scrollbar | PARTIAL | PARTIAL | `button_bg`+`accent` diikuti; track-vs-thumb kontras tergantung theme, bukan widget |
| TextEdit | NOT READY | NOT READY | Border `divider 0x333333` konstanta + caret `0x00E5FF` konstanta + bg = `editor = button_bg` (input!) sehingga light `button_bg 0xCFD8DC` = editor terang tapi border tetap gelap; selection `button_hover` ok |
| Menu/Dialog/Toast | NOT READY | NOT READY | `panel 0x252526` + `mborder 0x454545` + `chrome 0x2D2D2D` + `btnfill 0x3C3C3C` konstanta — theme terang tetap render panel gelap |
| MenuBar/StatusBar | NOT READY | NOT READY | `chrome 0x2D2D2D` + `divider 0x333333` konstanta |
| ListView/Table/TreeView/GridView | PARTIAL | PARTIAL | Row hover/selected via role (ok), tapi Table header `button_bg` di light = `0xCFD8DC` terang (ok) sementara divider antar-header `0x333333` gelap = garis gelap di atas terang |
| Tab | PARTIAL | PARTIAL | Active `button_bg` + `accent` underline ok; inactive `bg` ok |
| Toolbar | READY | READY | `bg`+`button_hover`+`fg` murni role (paling siap) |
| Image/FtText | READY | READY | Pixels/callback, bukan theme (empty-box `button_bg` ikut theme) |
| PromptDialog input | NOT READY | NOT READY | `editor`+`mborder` konstanta jalur sama dgn TextEdit |

Ringkas: tidak ada widget hardcode terang/gelap secara langsung — semua baca theme.
Tetapi `derive()` mengunci 8 warna ke charcoal gelap, sehingga SETIAP theme terang
pecah di Menu/Dialog/MenuBar/StatusBar/TextEdit. Light mode butuh `derive()`
berbasis input, bukan konstanta.

## 11. Accent Color Readiness

| Component | Ready | Coupling | Notes |
|---|---|---|---|
| `accent` role (Slider/Progress/Scrollbar/Tab/marker/focus/Dialog strip) | PARTIAL | 8 widget baca `theme.accent` langsung — ganti 1 nilai = semua ikut | Sudah mendukung custom accent TANPA ubah render; buktinya notepad `0x0098BC`, widget_demo merah/hijau |
| `button_hover` sebagai selection | NOT READY | TextEdit/TextBox/ListView/Table/TreeView/GridView/Menu pakai `button_hover` untuk selection/hover | Accent baru butuh hover turunan; saat ini hover = field independen, bukan fungsi accent — desync mudah (hover biru + accent merah) |
| `acc_text 0xDCDCAA` + `caret 0x00E5FF` | NOT READY | Konstanta di `derive()`, tidak ikut `accent` (komentar: "tidak ikut accent app supaya selalu sesuai spesifikasi") | Custom accent tidak mengubah shortcut amber/caret cyan — sengaja, tapi berarti bukan full-accent system |
| `button_bg 0x0F3460` sebagai selected | NOT READY | 4 widget pakai `button_bg` untuk selected | Selected terikat ke tombol, bukan accent — accent baru tidak mengubah seleksi |
| Target Neutral/Blue/Purple/Green/Orange/Red/Custom | PARTIAL | Jalur `accent` mendukung; jalur turunan + selection tidak | Tanpa rewrite pipeline: ya untuk widget aksen-murni; tidak untuk seleksi/hover/caret tanpa sentuh `derive()` + 6 call-site selection |
| Blocker arsitektur | — | `Theme::derive()` konstanta + tidak ada shade API berbasis accent di theme (shade ada di `color_utils`, tapi tidak dipanggil dari `derive()`) | `derive()` harus jadi fungsi (accent -> hover/selected/caret/divider) sebelum accent system penuh |

Widget tersulit dipisahkan dari hardcoded colors: Dialog/Menu/MenuBar/StatusBar/
TextEdit (8 konstanta derive) — bukan karena render kompleks, tapi karena warnanya
tidak berasal dari input.

## 12. Application Coupling

- `apps/widget_demo.c`: 3 preset (`tema_gelap/terang/hijau`, duplikat dengan
  settings) + runtime `COLOR_RGB` theme + `set_theme` via tombol. Tidak bypass
  internal; contoh theme-switching yang benar. Risiko: duplikat preset = drift.
- `apps/settings/appearance.cpp/hpp`: `kThemeDark/Light/Green` (sama persis dgn
  demo) + `applyTheme` -> `ui_window_set_theme` + save/load. Pemilik preset
  "resmi". Tidak bypass internal.
- `apps/notepad.c`: `NOTEPAD_THEME` 6×`COLOR_HEX` + `set_theme`. Satu-satunya app
  dengan identitas accent sendiri (`0x0098BC`). Tidak bypass.
- `apps/terminal.c`: theme runtime `COLOR_RGB` + `COLOR_PROMPT 0x7CC7FF` +
  `ui_textedit_set_prompt_style`. Prompt color = satu-satunya warna non-theme di
  ABI, dipakai benar via API. Tidak bypass.
- `apps/calc.c`, `apps/clock.c`, `apps/viewer.c`: TIDAK set theme (pakai default
  `0x1A1A2E/0x0F3460/0x2A4A7E`) — mewarisi global blue default. Tidak hardcode
  warna; coupling = implisit ke default.
- `apps/filemanager/*`, `apps/gallery/*`, `apps/imageview/*`: widget composition
  berat (Table/GridView/Image/ScrollView pan/menubar/toolbar/statusbar/dialog/
  prompt/notify/shortcut/DnD) tapi TIDAK set warna sendiri — coupling ke struktur
  ABI (kolom, thumb non-owning, visible_range, pan, row_at/cell_at), bukan ke
  styling. Refactor warna aman bagi mereka selama ABI + geometri stabil.
- `apps/fontdemo.c`, `apps/settings/fonts.cpp`: hardcode `0xE8E8EC` di callback
  `FtText` (bukan theme). Refactor theme tidak menyentuh mereka; mereka juga tidak
  ikut theme (inkonsistensi terisolasi).
- `system/desktop/*`: TIDAK memakai libui SAMA SEKALI. Sistem warna paralel
  (`kyuzen::desktop::Color + rgb()`, ~30 const di `theme.hpp`, `parse_color` di
  `launcher.cpp`, `Canvas::to_gui_color` adapter). Refactor libui tidak menyentuh
  desktop; unifikasi = proyek terpisah (risiko `.init_array`/static-init).
- `libs/core/libgui.c`: `0xF5F5F5`/`0x1E293B`/`0x333333` hanya warna awal/legacy,
  ter-overwrite render. Bukan coupling app.

## 13. Architectural Problems

### Critical

1. `Theme::derive()` hardcode 8 warna gelap (`theme.hpp:60-71`). Menghambat light
   mode + full accent tanpa rewrite. Setiap theme terang/green tetap dapat
   panel/chrome/divider/caret charcoal.
2. Selected == `button_bg` di ListView/Table/TreeView/GridView. Tidak ada role
   selection — refactor seleksi = sentuh 4 draw + definisi peran baru + migrasi
   theme file (v1 28B berubah bentuk bila field tambah).
3. Dua sistem warna paralel (libui `color_t` vs desktop `Color`). Accent/mode
   global OS tidak bisa satu keputusan — desktop dan app window selalu beda sistem.

### Medium

4. Hover/selection/checked/caret tersebar di `button_hover`/`button_bg`/`accent`/
   konstanta tanpa tabel peran — ganti accent = kemungkinan desync hover biru.
5. `button_bg` ganda peran: tombol + track scrollbar + bg textbox + editor surface
   (`editor = button_bg`). Satu nilai melayani 4 kebutuhan kontras beda.
6. Border/shadow `COLOR_BLACK` mentah di Button + `Painter::shadow` — bukan theme
   role; di theme terang terlihat kasar.
7. `color_get_contrast_text` mati (tidak dipakai) — kontras teks di atas aksen
   tidak terjamin saat accent custom (mis. accent kuning + `button_fg` putih).
8. Kapasitas widget kecil (Menu 20, List 32, Table 64, Tree 32, Grid 128) + tidak
   ada Radio/ComboBox/Tooltip/Separator — design system baru akan menambah widget,
   bukan hanya warna.
9. Tipografi tunggal 8x16 + baseline manual per widget — hierarchy + i18n terbatas;
   FreeType hanya di FtText/desktop, bukan teks UI.

### Minor

10. Duplikat preset theme (widget_demo vs settings/appearance) — drift.
11. `ps1_color` default hardcode + duplikat di terminal — satukan ke theme/palet.
12. Toolbar vs MenuBar lapisan beda (`bg` vs `chrome`) + Tab rata vs MenuBar fit-teks.
13. Caret 1px (TextBox) vs 2px (TextEdit/Prompt) — satukan.
14. `gui_draw_bar` legacy (`0x333333`) tak dipakai widget — cleanup kandidat
    (dilaporkan, tidak dihapus).

## 14. Recommended Refactor Direction

HANYA rekomendasi, tanpa implementasi:

1. Semantic roles di ATAS `color_t`, bukan di dalamnya: pertahankan `color_t` +
   `COLOR_HEX(0xRRGGBB)`; tambah role (`bg/fg/surface/border/text[-secondary]/
   disabled/selection/focus/accent[-hover/-pressed]/danger/warning/success`) di
   layer `ui_theme_t`/`ui::Theme`. Jangan tambah `COLOR_WINDOW_BG` ke color headers.
2. `derive()` jadi fungsi murni dari input (accent -> hover/selected/divider/caret
   via `color_darken/lighten`), bukan konstanta. Kunci: light/dark = derive
   berbasis luminance input; accent custom = derive berbasis hue input.
   Pertahankan `color_opaque()` normalisasi + integer-only (no float/SSE).
3. Pisahkan `selection` dari `button_bg` dan `editor` dari `button_bg` sebagai role
   sendiri. Ini perubahan `settings.ui` format — rencanakan v2 + loader v1/v0 tetap
   (pola yang sudah ada).
4. Backward compat: `ui_theme_t` 6 field adalah ABI + format file. Tambah field =
   versi baru + default-derive untuk file lama; JANGAN ubah urutan/size v1.
   Tidak ada per-widget color setter hari ini — jangan tambah; arahkan ke
   `variant = primary/secondary/danger` di level API bila perlu (saat ini tidak ada,
   jadi tidak ada yang perlu di-deprecate).
5. Typography/spacing: token terpusat (font tunggal tetap, tapi baseline/padding/
   radius/h-row distandardkan: contoh `ROW_H 20` vs Menu 22 vs Tab 26 disatukan).
   Elevation: generalisasi `Painter::shadow` (saat ini hanya Menu/Dialog).
6. Desktop (`system/desktop/theme.hpp`) tetap terpisah fase ini; unifikasi ke
   `color_t` = fase tersendiri (sentuh geometry/canvas/launcher/host test).

## 15. Migration Risks

- Format `settings.ui`: tambah role = v2; loader harus tetap baca v1/v0 + tolak blob
  kosong. Salah versi = theme user rusak (fallback default menutupi, tapi silently).
- ABI `ui_theme_t` dipakai C/C++/Rust (`rust/kyuzen-gui` terpisah) — ubah layout =
  rebuild semua app + cek `sizeof`/offset. Aditif + derive-default = risiko rendah;
  reorder/rename = risiko tinggi.
- Visual regression luas: 8 konstanta derive dipakai ~15 draw call-site; ubah
  derive = seluruh UI berubah. Verifikasi via QEMU screenshot byte-compare per
  theme (pola migrasi sebelumnya) + host test theme (`libui_theme_test.cpp`).
- FileManager/Gallery/ImageView tidak hardcode warna tapi sensitif geometri —
  refactor spacing/radius/row-height = layout mereka bergeser. Pisahkan fase warna
  dari fase geometri.
- `COLOR_HEX` argumen dievaluasi 3× (seperti `COLOR_RGB` existing) — dokumentasikan
  side-effect-free; sediakan inline runtime setara bila perlu.
- Scope creep ke dunia u32 (TTY/fb/panic) atau desktop `Color` — tahan; panic path
  no-lock/no-alloc tidak boleh tersentuh refactor theme.
- Test hilang yang dirujuk audit lama (`color_test.c`): verifikasi klaim
  binary-identical/parity sebelum migrasi massal, atau cabut referensinya.
