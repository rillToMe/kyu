# KyuzenOS UI Theme System (Phase A)

> Fondasi: mode × aksen → palet semantik → widget. Bukan redesign visual.
> Detail audit latar: `docs/design/gui/ui-color-widget-audit.md`.

## Color Representation

Tetap seperti sebelumnya (tidak berubah di Phase A):

- Tipe: `color_t { uint8_t r, g, b, a }` — straight alpha, permukaan opaque.
- Literal C: `COLOR_HEX(0xRRGGBB)` untuk posisi initializer (`static const`,
  member-init), `color_hex(0xRRGGBB)` untuk posisi ekspresi (assignment,
  argumen fungsi). Semantik channel selalu `0xRRGGBB`, BUKAN `0xAARRGGBB`.
- Tidak ada `#RRGGBB` sebagai literal C (tidak valid secara sintaks).
  Tidak ada runtime string parser.
- Serialisasi framebuffer `FORMAT_ARGB` (`0xAARRGGBB`) tidak berubah.

## Theme Mode

```c
typedef enum {
    UI_THEME_DARK = 0,
    UI_THEME_LIGHT = 1
} ui_theme_mode_t;
```

## Accent

```c
typedef enum {
    UI_ACCENT_NEUTRAL = 0,  // default
    UI_ACCENT_BLUE = 1,
    UI_ACCENT_PURPLE = 2,
    UI_ACCENT_GREEN = 3,
    UI_ACCENT_ORANGE = 4,
    UI_ACCENT_RED = 5,
    UI_ACCENT_CUSTOM = 6    // pakai ui_theme_config_t.custom
} ui_theme_accent_t;
```

Satu warna base per aksen (`theme_accent_base()` di
`libs/gui/widget/include/core/theme.hpp`):

| Aksen | Base |
|---|---|
| Neutral | `0x8B8B8B` |
| Blue | `0x2F81F7` |
| Purple | `0xA371F7` |
| Green | `0x3FB950` |
| Orange | `0xE8912D` |
| Red | `0xF85149` |

`Dark + Purple` = mode DARK + aksen PURPLE yang diturunkan deterministik —
bukan tema hardcoded terpisah. Tidak ada tabel 2×6 di source.

## Memasang Tema

```c
ui_theme_config_t cfg;
cfg.mode = UI_THEME_DARK;
cfg.accent = UI_ACCENT_PURPLE;
cfg.custom = color_hex(0x000000);   // hanya dibaca bila accent == CUSTOM
ui_window_set_theme_config(win, &cfg);
```

Custom accent:

```c
cfg.mode = UI_THEME_LIGHT;
cfg.accent = UI_ACCENT_CUSTOM;
cfg.custom = color_hex(0x7C3AED);
ui_window_set_theme_config(win, &cfg);
```

Jalur lama tetap jalan tanpa perubahan (`ui_window_set_theme(win, &theme)`
dengan `ui_theme_t` 6 warna). Berpindah legacy ↔ config kapan saja; seluruh
window digambar ulang. `NULL` = abaikan.

## Semantic Palette

Peran dan alasan (pemilik Phase A, kecuali ditandai `(B)` = dicadangkan Phase B):

| Role | Dark | Light | Dipakai untuk |
|---|---|---|---|
| `bg` (background) | `0x111111` | `0xF5F5F5` | isi window |
| `surface` | `0x181818` | `0xFFFFFF` | tombol, input, track, header |
| `surface_elevated` | `0x232323` | `0xEAEAEA` | hover, dialog, menu, placeholder |
| `text` | `0xF2F2F2` | `0x181818` | semua teks primer |
| `text_secondary` | `0xA8A8A8` | `0x666666` | shortcut menu, baris aksen dialog |
| `text_disabled` | mix ± | mix ± | item menu nonaktif |
| `border` | `0x303030` | `0xD6D6D6` | tepi tombol |
| `border_subtle` | `0x262626` | `0xE3E3E3` | divider, garis pemisah |
| `accent` | base | base | slider, progress, scrollbar, tab, centang, fokus |
| `accent_hover` | lighten 32 | darken 22 | hover tombol primer |
| `accent_pressed` | darken 36 | darken 42 | tombol primer ditekan |
| `accent_subtle` | 15% di atas bg | 15% di atas bg | (B) lencana/tinta aksen lembut |
| `accent_contrast` | luminance | luminance | (B) teks di atas aksen |
| `selection` | 30% di atas bg | 20% di atas bg | baris terpilih (List/Table/Tree/Grid) |
| `focus` (== accent) | base | base | border/caret input fokus |
| `success` | `0x3FB950` | `0x1A7F37` | (B) diagnostik |
| `warning` | `0xD29922` | `0x9A6700` | (B) diagnostik |
| `danger` | `0xF85149` | `0xCF222E` | (B) diagnostik |

Angka faktor (32/36/22/42/15%/30%/20%) = konstanta derive di
`Theme::derive_accent_family()` + `apply_config()`. Semua integer
(`color_darken`/`color_lighten`/campuran `theme_mix`), tanpa float.

Default = **Dark + Neutral**. Biru legacy (`0x0F3460`, `0x2A4A7E`) tidak muncul
di tema bawaan; Blue tetap tersedia sebagai `UI_ACCENT_BLUE` opsional.

## Compatibility Behavior

- `ui_theme_t` (6 field) dan `ui_window_set_theme()` tidak berubah signature
  maupun makna. `Theme::set()` mengisi 6 field + 8 turunan persis seperti dulu
  (`derive_legacy()` — konstanta charcoal/amber/cyan dipertahankan HANYA di
  jalur ini sebagai shim), lalu memetakan peran semantik 1:1 dari nilai yang
  sama (`surface = button_bg`, `surface_elevated = button_hover`,
  `selection = button_bg`, `focus = accent`, ...). Aplikasi legacy tampil
  piksel-identik (dikunci `make test-libui-theme`: gradien tombol =
  `aa_shade`/`aa_mix` jalur lama).
- `Theme::to_abi()` tidak berubah (6 warna) → file `settings.ui` v1/v0 tetap
  dibaca dan ditulis untuk tema legacy.
- Tidak ada setter warna per-widget dulu maupun sekarang; widget membaca peran
  tema. Public widget API (create/set_click/layout/...) tidak berubah.

## Persistensi (`settings.ui`)

| Versi | Tag | Isi | Ditulis bila |
|---|---|---|---|
| v2 | `KTH2` | mode(1) + aksen(1) + custom(r,g,b,a) = 10 byte | tema terakhir dari config |
| v1 | `KTH1` | 6 × `color_t` = 28 byte | tema terakhir legacy |
| v0 | — | 6 × uint32 `0x00RRGGBB` = 24 byte | hanya dibaca (file lama) |

Load menolak: ukuran tak dikenal, tag salah, enum di luar rentang, blob
semua-nol. Gagal load tidak mengubah tema aktif.

## File Terkait

- `include/libui.h` — enum mode/aksen, `ui_theme_config_t`,
  `ui_window_set_theme_config()`, format file.
- `libs/gui/widget/include/core/theme.hpp` — `theme_accent_base()`,
  `theme_mix()`, `Theme::apply_config()/set()/derive_accent_family()/
  derive_legacy()`, `SHADE_*`.
- `libs/gui/widget/include/window/window.hpp` — `set_config()`,
  `theme_cfg`/`cfg_valid`, `settings_save/load` v2.
- `libs/gui/widget/abi/libui_abi.cpp` — bridge C.
- Widget: hanya penggantian peran warna (`button_bg`→`surface`,
  `button_hover`→`surface_elevated`, `button_fg`/`fg`→`text`,
  seleksi→`selection`, disabled→`text_disabled`, border tombol→`border`).
  Geometri, font, layout, event: tidak disentuh.

## Yang Sengaja Tidak Disentuh

- `system/desktop/theme.hpp` (sistem `Color` desktop) — fase terpisah.
- Jalur `uint32_t` mentah (TTY/fb/panic/boot) — bukan `color_t`.
- Renderer/compositor/KWM/syscall, font, spacing, radius, animasi, XML.
- `tests/host/probes/_fileman_probe.py` menyebut nilai tema legacy
  (`btnfill 0x3C3C3C`, `button_hover` 116×22) — probe QEMU itu mengasumsikan
  tema legacy/default lama; bila dijalankan di atas default baru, ekspektasi
  pikselnya perlu ditinjau (di luar scope Phase A).
- `make test-textedit`: target rusak pre-existing (`textedit_test.cpp`
  tidak ada di tree) — tidak diperbaiki di Phase A.
