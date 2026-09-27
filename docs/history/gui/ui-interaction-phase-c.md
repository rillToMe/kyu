# UI Interaction Foundation — Phase C

> Di atas Phase A (tema) + Phase B (visual). Tanpa XML, tanpa layout baru,
> tanpa widget baru. Fokus: keyboard-operable + focus-aware + state lengkap.

## Focus Model

- Tiga konsep per widget: `focusable` (bisa dipegang), `enabled` (aktif),
  `focused`/`has_focus` (dipegang). Invarian: `!enabled → tak bisa fokus`,
  `!focusable → tak bisa fokus`. Fokus selalu pada widget valid atau `nullptr`
  (`set_focus` + `focus_traversal` + dispatch keyboard semua melewati
  validasi).
- Single source of truth = `Window::focused` (milik window, bukan global).
- Anti-dangling (C7): `focus_alive()` (`focused == dialog` atau ada di pohon
  via `owns_widget()` — hanya perbandingan pointer, tanpa deref) dipakai di
  `set_focus()`, dispatch keyboard, dan restore dialog. Ini sekaligus menutup
  UAF laten kode Phase B (`focused->enabled` tanpa validasi).
- Batasan jujur: `hovered`/`grabbed` belum divalidasi sama (di luar scope C;
  hanya `focused`).

## Traversal Algorithm

- `Window::focus_traversal(fwd)`: kumpulkan stop via `collect_focus()` —
  DFS insertion order: `top_bars` (urutan `add_bar`) lalu subtree `root`
  (urutan `ui_layout_add`), memakai `dirty_child_count/dirty_child` yang
  sudah ada. Subtree invisible/disabled di-prune. Popup menu (mouse-driven)
  bukan stop. Kap 64 stop (lebihnya diabaikan, deterministik).
- Tab maju / Shift+Tab mundur, wrap-around. Fokus di luar daftar (dihapus)
  → mulai dari ujung (self-healing). Tanpa API tab-index: hierarchy cukup
  (keputusan §4, dievaluasi dan ditolak sebagai overengineering).
- Intersepsi di `run()` cabang non-dialog, SETELAH registry shortcut:
  Tab/Shift+Tab (scancode `0x0F`, tanpa Ctrl/Alt) → traversal, KECUALI widget
  fokus `wants_tab()` (editor multiline = indentasi). Shortcut app yang
  mengikat Tab tetap menang (dicek lebih dulu). `key_cb` tidak lagi menerima
  Tab polos (perubahan perilaku yang disengaja; didokumentasikan).

## Tab Ordering

`tab order = deterministic traversal order` (insertion/DFS). Contoh demo:
strip Tab → tombol "+1" → TextBox → CheckBox → Slider → … (terbukti di QEMU).

## Modal Focus Behavior

- `open_dialog`: simpan `focus_prev`, `set_focus(dialog)` (dialog =
  satu stop fokus modal, `focusable() == true`).
- Selama modal: semua key (termasuk Tab) masuk cabang `dialog` di `run()`
  → traversal window tak terjangkau (trap struktural) + `focus_traversal`
  memaksa kembali ke dialog bila dipanggil langsung.
- `close_dialog`: restore SEBELUM `delete` — prev yang masih hidup di pohon
  + enabled dipakai, sisanya fallback stop pertama (`first_focusable()`).
  Tanpa deref pointer mati.
- Dialog biasa: panah Kiri/Kanan pindah hover tombol, Enter/Spasi aktifkan.
  PromptDialog: `on_key` teks dipertahankan (input primer); Tab diabaikan
  (fokus tetap di dialog).

## TextBox Error API

```c
void ui_textbox_set_error(ui_widget_t* widget, int is_error);
```

- Aditif, konvensi repo (`ui_widget_t*`, `int`). Tanpa validasi/callback/
  regex/framework — state + rendering saja.
- Visual: background tint `danger` 10% (`theme_mix`) + border `danger`;
  fokus + error coexist (border tetap `focus` ring, tint tetap terlihat).
- Coexist teruji piksel di kedua kondisi + clear kembali ke subtle.

## Disabled-State Rules

- Generik (Phase B) + guard `pick()` di `Layout`/`Tab` + `set_focus` menolak
  + dispatch keyboard dilewati + fokus dilepas saat di-disable via ABI.
- Tab item: `ui_tab_set_enabled(widget, index, enabled)` (konvensi
  `ui_menu_set_enabled`); disabled = tak bisa klik/panah/fokus, judul
  `text_disabled`, hover dilewati. Strip Tab sendiri focusable (panah
  navigasi) + focus ring di dalam bounds.
- Menu item disabled: sudah lengkap pra-C (hit −1, hover skip, teks redup) —
  dikunci test, tanpa perubahan kode.
- Scrollbar: sesuai semantik repo = mouse-only, bukan stop fokus
  (`!focusable()` dikunci); scroll tetap jalan. Tak dibuat focusable artifisial.

## Keyboard Activation Rules

- Button/Checkbox: Enter/Spasi (Phase B, dipertahankan) + traversal bisa
  mencapai mereka (Phase B `focusable`, kini berguna).
- Slider: panah ±1, PgUp/PgDn ±sepuluh rentang, Home/End (Phase B).
- Tab strip: panah Kiri/Kanan lewati judul disabled (baru).
- TextBox/TextEdit: semantik existing (Enter submit, Tab editor).
- Menu: tanpa model keyboard (tidak ada sebelumnya; tidak dibuat).

## Semantic Color Usage

- Baru dikonsumsi: `danger` (error TextBox + tombol danger Phase B),
  `focus` (semua ring/border fokus incl. dialog/strip/tab).
- Tetap cadangan (tanpa state buatan): `accent_subtle`, `success`, `warning`.
- Tanpa logika hue di widget (aturan Phase B dipertahankan).

## API Additions (aditif, non-breaking)

```c
void ui_textbox_set_error(ui_widget_t* widget, int is_error);
void ui_tab_set_enabled(ui_widget_t* widget, int index, int enabled);
```

Perilaku traversal/fokus/dialog: tanpa API baru (otomatis). Tidak ada
signature berubah, field dihapus, atau rename.

## Test Coverage

`make test-libui-theme` §13–§17 (C1–C25): traversal maju/mundur/wrap/skip/
self-heal/ring; dialog initial/trap/restore/fallback; error off/on/focus/
clear; disabled button/checkbox/tab/menu/scrollbar; aktivasi keyboard;
piksel (normal≠disabled/error, ring fokus, tint, underline, divider, primer).
`tests/host/probes/_focus_probe.py` (baru): QEMU headless boot→login→
`start widget_demo`→Tab/Tab/Shift+Tab→screendump (delta + centroid ring)
+ serial bersih.

## Known Limitations

1. `hovered`/`grabbed` belum divalidasi anti-dangling (hanya `focused`).
2. Dialog bersarang single-level (`focus_prev` satu slot).
3. Tab strip + panel sama-sama stop (kontainer dan isi tertraversal).
4. Popup menu mouse-only (tanpa model keyboard).
5. `FOCUS_MAX = 64` stop (lebihnya diabaikan).
6. Makefile: header widget bukan prasyarat target test → binary bisa basi
   setelah edit header tanpa menyentuh `.cpp` (ditemukan saat validasi;
   mitigasi: `touch` file test sebelum `make`).
7. `make test-textedit` rusak pre-existing; probe fileman menyebut nilai
   tema lama (warisan Phase A, tak disentuh).

## Yang TIDAK Diimplementasikan (eksplisit)

XML / parser / schema / inflator / factory / codegen, Grid/Constraint
layout, widget baru (Radio/ComboBox/Tooltip), framework validasi,
traversal Tab-order kustom, unifikasi tema desktop.
