# KyuzenOS Design System (libui)

> Dokumen ini untuk **penulis aplikasi**. Ia menjelaskan bahasa visual KyuzenOS
> dan cara memakainya. Untuk riwayat audit & keputusan arsitektur, lihat
> `libui-redesign-report.md` di folder yang sama.

## 1. Prinsip

Bahasa visual KyuzenOS dibangun dari empat hal, dalam urutan prioritas:

1. **Tipografi membangun hierarki.** Judul, isi, keterangan dibedakan oleh
   ukuran/huruf besar/warna teks — bukan oleh kotak berwarna.
2. **Ruang memisahkan, garis menegaskan.** Jarak antar elemen adalah pemisah
   utama. Garis 1px dipakai untuk memisahkan BAGIAN, bukan setiap elemen.
3. **Kedalaman adalah informasi.** Bayangan hanya untuk yang benar-benar
   mengambang (menu, dialog). Konten halaman tidak berbayang.
4. **Aksen punya makna.** Warna aksen menandai aksi utama, nilai terpilih, dan
   fokus. Bukan hiasan. Biru **bukan** identitas default — default-nya netral.

### Yang tidak dilakukan

Tidak ada: kartu membulat bersarang, glassmorphism, gradient latar, blob
dekoratif, hero raksasa, kontrol berbentuk pil di mana-mana, bayangan bertumpuk,
badge tak perlu, emoji sebagai ikon, atau animasi yang tidak menjelaskan apa pun.

Menambah `border-radius`, `shadow`, `gradient`, atau `padding` **bukan** cara
membuat UI terasa modern.

---

## 2. Tema: satu objek, semua keputusan visual

```cpp
// Widget menerima tema lewat Painter — tidak pernah memilih warna sendiri.
void MyWidget::draw(ui::Painter& p) {
    p.rect(x, y, w, h, p.theme.surface);          // benar
    p.rect(x, y, w, h, color_hex(0x2C5EF5));      // SALAH: warna hardcoded
}
```

`ui::Theme` memuat tiga hal sekaligus:

| Bagian | Isi | Contoh |
|---|---|---|
| warna | palet ter-resolve | `surface`, `surface_elevated`, `surface_variant`, `text`, `text_secondary`, `border`, `accent`, `danger`, … |
| `metrics` | jarak, radius, ukuran kontrol | `p.theme.metrics.md`, `.radius_control`, `.control_h` |
| `type` | peran tipografi | `p.theme.type.title`, `.body`, `.caption` |

### Mode & aksen

Tema ditentukan oleh **mode × aksen** — bukan tabel tema per kombinasi:

```c
ui_theme_config_t cfg;
cfg.mode   = UI_THEME_DARK;      // UI_THEME_DARK | UI_THEME_LIGHT
cfg.accent = UI_ACCENT_PURPLE;   // NEUTRAL|BLUE|PURPLE|GREEN|ORANGE|RED|CUSTOM
cfg.custom = color_hex(0x7C3AED); // hanya dibaca bila accent == CUSTOM
ui_window_set_theme_config(win, &cfg);
```

Palet penuh diturunkan toolkit. Menambah aksen = menambah satu baris di
`theme/colors.hpp`, bukan menulis satu tema baru.

---

## 3. Token

Semua token hidup di `libs/gui/widget/include/theme/`:

| File | Isi |
|---|---|
| `colors.hpp` | token warna + rumus turunan state |
| `typography.hpp` | peran teks + pengukuran |
| `metrics.hpp` | skala jarak, radius, ukuran kontrol, bar chrome |
| `elevation.hpp` | model kedalaman + kebijakan bayangan |
| `motion.hpp` | durasi + easing |
| `icons.hpp` | sistem ikon |

### Jarak

Satu deret, tanpa angka ajaib:

```
xs=4  sm=8  md=12  lg=16  xl=24  xxl=32  xxxl=48
```

```cpp
ui_vbox_create(win, UI_SPACE_LG);       // benar
ui_vbox_create(win, 13);                // SALAH: bukan token
```

### Radius

Sedikit level, masing-masing punya arti:

| Token | Nilai | Dipakai untuk |
|---|---|---|
| `radius::NONE` | 0 | bar, tabel, editor |
| `radius::SMALL` | 3 | kotak centang, track |
| `radius::CONTROL` | 5 | tombol, input, dropdown |
| `radius::CONTAINER` | 7 | menu, popup, panel |
| `radius::DIALOG` | 9 | modal |
| `radius::PILL` | kapsul | hanya bentuk yang memang kapsul (switch, grip slider, cincin radio) |

### Ukuran kontrol

| Token | Nilai | Turunan |
|---|---|---|
| `control::H_SM` | 24 | 16 glyph + 2×4 napas — baris daftar, kontrol rapat |
| `control::H_MD` | 28 | **default** — tombol, input, dropdown |
| `control::H_LG` | 36 | aksi utama halaman |

### Tipografi

Peran semantik, bukan ukuran mentah:

```cpp
p.text_role(title, x, y, color, p.theme.type.title);   // judul halaman
p.text_ellipsis(name, x, y, max_w, color, p.theme.type.body);  // isi + potong
```

| Peran | Tinggi baris | Tracking | Huruf besar | Tone |
|---|---|---|---|---|
| `display` | 32 | 1 | — | primer |
| `title` | 24 | 1 | — | primer |
| `section` | 20 | 0 | **ya** | sekunder |
| `body` | 20 | 0 | — | primer |
| `body_emphasis` | 20 | 0 | — | primer |
| `label` | 18 | 0 | — | primer |
| `caption` | 16 | 0 | — | sekunder |
| `mono` | 16 | 0 | — | primer |

**Kenyataan renderer:** toolkit menggambar dengan font bitmap 8×16 (advance
tetap 8px). Peran di atas mengendalikan tinggi baris, tracking, dan huruf besar
— itulah hierarki yang tersedia. Ukuran px peran (`ft_size_px`) adalah **niat**
untuk aplikasi yang memakai FreeType lewat `ui_fttext_*`; toolkit tidak
menerapkannya sendiri.

### Font sistem (tipografi nyata)

Aplikasi dapat memasang **font UI pilihan pengguna** (`/font.ui`, diatur di
Settings > Fonts) supaya SELURUH teks toolkit memakai font itu — dan seluruh
tata letak yang mengukur teks ikut menyesuaikan:

```cpp
#include "services/uifont.hpp"

int MyApp::run() {
    ui::uifont_install();   // SEBELUM widget pertama dibuat
    ...
}
```

Link tambahan (objek terpisah, bukan bagian toolkit inti):

```cmake
LIBS kyuzen-widget-uifont kyuzen-text-manager kyuzen-text-raster kyuzen-freetype
```

**Kenapa sebelum widget dibuat:** lebar tombol, label, dan baris daftar dihitung
dari metrik font saat widget dibuat. Memasang font setelahnya meninggalkan
ukuran yang salah.

**Kapan pakai:** aplikasi yang ingin mengikuti pilihan font pengguna. Toolkit
inti sengaja tetap bebas FreeType, jadi aplikasi yang tidak memanggilnya tetap
memakai bitmap 8×16 tanpa biaya apa pun.

Kontrak internalnya: `core/text_provider.hpp` mendefinisikan `TextProvider`
(measure/line_height/ascent/draw) dan toolkit memakainya untuk mengukur dan
menggambar. Bila provider tidak lengkap, toolkit kembali ke bitmap — bukan
menggambar separuh.

### Kedalaman

| Level | Permukaan | Bayangan |
|---|---|---|
| `ELEV_BASE` | `bg` | tidak |
| `ELEV_SURFACE` | `surface` | tidak |
| `ELEV_RAISED` | `surface_elevated` | tidak |
| `ELEV_POPUP` | `panel` | ya, 4 cincin |
| `ELEV_DIALOG` | `panel` | ya, 6 cincin |

**Terbenam vs terangkat.** `surface_variant` adalah permukaan *terbenam* (input,
track, gutter) dan selalu bergerak ke arah `bg`. `surface_elevated` adalah
permukaan *terangkat* (tombol, header) dan bergerak ke arah teks. Keduanya
dijamin berbeda — dikunci test di kedua mode.

### Gerak

```
instant=0  fast=90  normal=130  slow=180
```

Hover dan press **tidak** dianimasikan: keduanya harus terasa instan. Animasi
hanya untuk permukaan yang muncul/hilang, dan jumlah frame-nya dibatasi token.

---

## 4. State

Satu definisi untuk semua kontrol (`core/state.hpp`):

```cpp
ui::StateInputs st;
st.hover = hover;
st.pressed = pressed;
st.focused = has_focus;
st.enabled = enabled;
color_t fill = ui::state_surface(p.theme, st);
color_t bd   = ui::state_border(p.theme, st);
```

Prioritas, tanpa pengecualian:

```
disabled > pressed > hover
focus   hanya mempengaruhi BORDER, tidak pernah warna isi
selected mempengaruhi ISI (baris daftar), bukan border
```

Konsekuensi yang sering terlewat: **fokus harus terlihat walau pointer berada di
atas widget**, dan **disabled selalu menang** atas hover/press.

---

## 5. Ikon

Satu sistem, bukan gambar ad-hoc per aplikasi:

```c
ui_button_set_icon(btn, UI_ICON_SETTINGS);
ui_listview_set_row_icon(lv, 0, UI_ICON_DISPLAY);
```

Ikon digambar toolkit dari geometri goresan 1px pada grid 16px. Aplikasi
memakai **nama semantik**; bentuknya bisa diperbaiki di satu tempat.

Tersedia: `CHEVRON_RIGHT/DOWN/LEFT/UP`, `ARROW_RIGHT`, `EXPAND`, `COLLAPSE`,
`CLOSE`, `CHECK`, `PLUS`, `MINUS`, `SEARCH`, `REFRESH`, `MORE`, `EDIT`,
`TRASH`, `FOLDER`, `FILE`, `IMAGE`, `HOME`, `STAR`, `SETTINGS`, `DISPLAY`,
`PALETTE`, `FONT`, `NETWORK`, `POWER`, `INFO`, `WARNING`, `ERROR`.

Ukuran optik: `ui_icon_size(UI_ICON_SIZE_SM|MD|LG|XL)`.

**Emoji bukan ikon UI.**

---

## 6. Daftar: primitif utama

Sebagian besar UI desktop adalah daftar. Baris daftar punya bentuk baku:

```
[ikon]  judul                       [trailing]  [chevron]
        deskripsi
```

```c
int row = ui_listview_add_row(lv, "Display",
                              "Resolution, scaling, night light",
                              UI_ICON_DISPLAY, /*chevron=*/1);
ui_listview_set_row_disabled(lv, row, 0);
```

- Baris dengan `description` menjadi dua baris tinggi (otomatis).
- Navigasi keyboard: panah/Home/End/PgUp/PgDn pindah **dan** memilih;
  Enter mengaktifkan.
- **Jangan** membungkus tiap baris dalam kartu. Baris terpilih memakai tinta
  `selection`; hover memakai `surface_hover`.

### Baris pengaturan = komposisi

Halaman pengaturan dibangun dari daftar + `section`, bukan dari kartu:

```c
ui_widget_t* sec = ui_section_create(win, "Appearance", UI_SPACE_SM);
ui_layout_add(sec, row1);
ui_layout_add(sec, row2);
```

---

## 7. Kontrol: kapan pakai apa

| Kebutuhan | Kontrol | Alasan |
|---|---|---|
| Aksi utama | `button` + `variant="primary"` | satu per permukaan |
| Aksi biasa | `button` (secondary) | default |
| Aksi tenang (Batal, Lewati) | `button` + `variant="tertiary"` | tanpa isi/border — mencegah UI penuh kotak |
| Aksi merusak | `button` + `variant="danger"` | |
| Aksi ikon saja | `button` + `icon`, teks kosong | jadi persegi otomatis |
| Nilai bagian dari form | `checkbox` | dikirim bersama tombol Simpan |
| Pengaturan berlaku segera | `switch` | nyala/mati tanpa tombol Simpan |
| Pilihan eksklusif | `radio` + grup | |
| Pilih dari daftar pendek | `combobox` | |
| Rentang nilai | `slider` | |

---

## 8. XML deklaratif

XML memuat **struktur dan semantik**. Tema memuat **tampilan**.

### Di mana file XML tinggal

`ui/xml/*.xml` — file XML sungguhan, bukan string di dalam `.cpp`. Build
meng-embed-nya jadi header (`cmake/KyuzenUiXml.cmake`):

```cmake
kyuzen_embed_xml(kyuzen-settings SOURCES settings_appearance.xml)
```

```cpp
#include "ui_xml_data.h"   // generated
ui_xml_doc_t* doc = ui_xml_parse(ui_xml_settings_appearance,
                                 ui_xml_settings_appearance_len, &err);
```

Alasannya: XML yang terkubur di dalam kode tidak bisa di-review sebagai
dokumen, tidak enak di-diff, dan editor tidak memberinya penyorotan sintaks.

### Contoh

```xml
<window>
    <vbox spacing="lg">
        <label text="Appearance"/>

        <section title="Mode" spacing="sm">
            <radio id="mode_dark" text="Dark" group="mode" selected="true"/>
            <radio id="mode_light" text="Light" group="mode"/>
        </section>

        <section title="Accent color" spacing="sm">
            <hbox spacing="sm">
                <button id="accent_neutral" text="Neutral"/>
                <button id="accent_blue" text="Blue"/>
            </hbox>
        </section>
    </vbox>
</window>
```

### Atribut yang ADA (semantik)

| Atribut | Nilai | Arti |
|---|---|---|
| `variant` | `primary`/`secondary`/`tertiary`/`danger` | peran aksi |
| `icon` | nama ikon (`settings`, `check`, …) | ikon semantik |
| `spacing` | `xs`/`sm`/`md`/`lg`/`xl` atau px | jarak antar anak |
| `size` | `sm`/`md`/`lg`/`xl` | ukuran optik ikon |
| `theme-mode`, `theme-accent` | | pilihan tema pengguna |

### Atribut yang DITOLAK (visual)

`padding`, `radius`, `color`, `shadow`, dan nilai visual lain **ditolak
inflater** (test `G07` menguncinya). Kalau sebuah nilai visual tidak bisa
diambil dari tema, itu tanda tokennya perlu ditambah di libui — bukan tanda XML
perlu atribut baru.

### Elemen

`window`, `vbox`, `hbox`, `grid`, `section`, `label`, `button`, `textbox`,
`checkbox`, `switch`, `radio`, `combobox` (+`item`), `slider`, `progressbar`,
`separator`, `icon`, `image`, `listview` (+`item`).

Binding callback tetap dari C — tidak ada kode di XML:

```c
ui_xml_bind(cx, "apply", UI_XML_ON_CLICK, on_apply, 0);
```

---

## 9. Resep membangun halaman

```cpp
// 1. chrome aplikasi
ui_widget_t* bar = ui_menubar_create(win);
ui_window_add_bar(win, bar);
ui_widget_t* tb = ui_toolbar_create(win);
ui_toolbar_add_button_icon(tb, "Refresh", UI_ICON_REFRESH, cb, ud);
ui_window_add_bar(win, tb);
ui_widget_t* sb = ui_statusbar_create(win);
ui_statusbar_set_text(sb, "Ready", "My App");
ui_window_add_bar(win, sb);

// 2. navigasi + isi
ui_widget_t* root = ui_hbox_create(win, UI_SPACE_LG);
ui_widget_t* nav = ui_listview_create(win, 200, h);
ui_listview_add_row(nav, "General", 0, UI_ICON_SETTINGS, 0);
ui_layout_add(root, nav);

ui_widget_t* scroll = ui_scrollview_create(win, w, h);
ui_widget_t* page = ui_vbox_create(win, UI_SPACE_XL);
ui_widget_t* sec = ui_section_create(win, "Section", UI_SPACE_SM);
ui_layout_add(sec, control);
ui_layout_add(page, sec);
ui_scrollview_set_child(scroll, page);
ui_layout_add(root, scroll);
ui_window_add(win, root);

// 3. jalankan
ui_window_run(win);
```

**Referensi visual:** jalankan **UI Gallery** (`uigallery`) — galeri memakai
widget produksi yang sama, jadi ia selalu menunjukkan tampilan yang benar.

---

## 10. Checklist sebelum merge

- [ ] Tidak ada warna/radius/jarak/ukuran font hardcoded di widget atau app.
- [ ] Semua state relevan terlihat: normal, hover, pressed, focused, disabled.
- [ ] Fokus keyboard terlihat dan urutannya masuk akal (Tab/Shift+Tab).
- [ ] Teks panjang dipotong dengan `text_ellipsis` (tidak bocor keluar bounds).
- [ ] Kedua mode tema diperiksa (terang **dan** gelap).
- [ ] Biaya render tidak melampaui anggaran (`ctest -R test-libui-perf-qa`).
- [ ] `ctest --test-dir build/host` hijau.
