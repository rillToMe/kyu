# Desain: Image Viewer (galeri PNG) — Phase 11

> **Status**: SELESAI (2026-09-19).
> **Cakupan**: `user_apps/viewer.c` (UI baru) + 4 API libui baru
> (`ui_image_set_fit`, `ui_image_natural_size`, `ui_scrollview_set_pan`,
> `ui_listview_set_selected`).
> **Verifikasi**: `make test-libui-theme` (37 PASS, termasuk 12 cek baru),
> `make`/`make apps` tanpa warning, dan capture QEMU headless.

## Masalah sebelumnya

Viewer lama: daftar `.png` 96px di atas, `Image` natural-size (1024×1024) di
dalam `ScrollView` 464×210, zoom lewat teks toolbar. Akibatnya:

- Gambar 1024×1024 tampil pada skala 100% di viewport 464×210 → hanya sepetak
  yang terlihat dan pengguna harus menggeser (dan tetap tidak bisa melihat
  sisi kanan, karena `Scrollable` hanya punya scroll **vertikal**).
- Kontrol `Zoom -`/`Zoom +` adalah `Toolbar` yang membagi rata lebar bar →
  label teks tanpa bentuk tombol.
- Tidak ada info berkas/dimensi/zoom, berkas pertama tidak terpilih otomatis.

## Tata letak baru

```
[MenuBar: Berkas | Tampilan]
[tombol: -  +  Pas  1:1  Prev  Next]
[sidebar daftar berkas (170) | gambar (446×374)]
[statusbar: n/total  nama  1024x1024 | Pas 36%]
```

- Window 640×470; `BODY_H = WIN_H - MARGIN - MENUBAR_H - BTN_H - 2*SPACING -
  STATUS_H` = 374 (spesifik nilai layout, bukan sihir).
- Kontrol = `HBox` berisi `Button` sungguhan (gradien + sudut membulat dari
  tema), bukan label `Toolbar`.
- Sidebar = `ListView` (pilih berkas), area gambar = `ScrollView` mode pan.
- Statusbar: kiri `n/total  nama  WxH`, kanan `Pas 36%` / `Ukuran asli  100%` /
  `Zoom  150%`.
- Shortcut: `+`/`=`, `-`, `F` (pas), `1` (1:1), `N`/`P` (berkas berikut/
  sebelumnya), `Esc` (keluar → kembali ke Explorer bila dibuka dari sana).
  Di keyboard US `+`/`_` butuh Shift, jadi shortcut-nya didaftarkan dengan
  `KEY_MOD_SHIFT` yang sesuai (kernel mengirim P1 hasil terjemahan shift).

## Keputusan: auto-fit, bukan geser manual

Saat berkas dibuka/dipilih, viewer langsung menghitung skala agar **seluruh**
gambar masuk area:

```
persen = min(view_w * 100 / iw, view_h * 100 / ih)   // sumbu tersempit
```

di-clamp ke 10..400 (batas `ui_image_set_scale`). Untuk kyuzen.png 1024×1024
di area 446×374 → 36% → gambar 368×368, ditaruh di tengah. Hasilnya: tidak
ada scrollbar sama sekali selama gambar muat, dan tidak perlu menggeser.

Scrollbar baru muncul saat pengguna melakukan zoom melebihi area. Itu
memerlukan tiga hal di toolkit (semuanya **opt-in**):

| API | Fungsi |
|-----|--------|
| `ui_image_set_fit(w, view_w, view_h)` | Hitung + terapkan skala fit; return persen efektif (0 = tak ada gambar) |
| `ui_image_natural_size(w, &iw, &ih)` | Dimensi natural PNG yang tampil (untuk statusbar) |
| `ui_scrollview_set_pan(w, on)` | Mode "lihat gambar": scroll 2 arah + anak di tengah saat lebih kecil + anchor zoom |
| `ui_listview_set_selected(w, i)` | Pilih baris dari kode (viewer dibuka dari Explorer) + auto-gulir ke view |

### Detail `ui_scrollview_set_pan`

- **Bar horizontal** hanya saat isi lebih lebar dari view; bar vertikal hanya
  saat isi lebih tinggi dari view (`update_scroll_maxes()` menghitung
  ketergantungan bar⇄viewport dua iterasi). Saat mode pan OFF, rumus lama
  (`set_scroll_max`, hanya vertikal) dipakai apa adanya — app lain tidak
  berubah satu piksel pun.
- **Center**: isi yang lebih kecil dari view ditaruh di tengah (`child_offset`),
  bukan menempel kiri-atas.
- **Anchor zoom**: saat ukuran isi berubah (zoom) `settle()` memetakan ulang
  offset supaya titik tengah view tetap menunjuk titik yang sama di gambar —
  jadi membesarkan gambar tidak melompat ke pojok. Jika isi lama lebih kecil
  dari view, acuannya tengah isi (`last_c?/2`), bukan `scroll + view/2`.

## Verifikasi

- `make test-libui-theme` → **37 PASS** (12 cek baru Phase 11), antara lain:
  fit 400×200 → 200×200 = 50%, tanpa scrollbar saat muat, center offset,
  scrollbar dua arah setelah zoom, invarian "tengah gambar == tengah view"
  pada sumbu X **dan** Y, ScrollView tanpa pan tetap kiri-atas, dan
  `ui_listview_set_selected` + auto-gulir.
  **Mutation check**: rumus anchor diganti versi naif (tanpa kompensasi
  center) → 2 cek gagal, jadi regresinya benar-benar terkunci.
- QEMU headless (`-display none`, screendump + analisis piksel):
  - Window 640×470 di (100,80); viewport gambar `x 286..732, y 180..554`.
  - Setelah dibuka: blok gambar **368×368** px (36% dari 1024) dengan tengah
    (508,366) ≈ tengah viewport (509,367) → auto-fit + center terbukti.
  - Tanpa zoom: **tidak ada** strip accent (scrollbar) di area gambar.
  - Setelah 4× zoom: bar vertikal 6px di `x 726..731` dan bar horizontal 6px
    di `y 548..553` → scrollbar hanya saat zoom.
  - Tekan `F` → frame **identik byte-per-byte** dengan frame auto-fit.
  - `N`/`P` mengganti berkas: mean area gambar berubah dari
    `(216.6,226.0,230.1)` (kyuzen.png) ke `(85.5,86.3,87.2)` (logo.png), sama
    dengan mean hasil render PIL 36% → berkas benar-benar dimuat ulang.
- Jejak serial `[viewer] png di / = 2` + `[viewer] buka <nama> = <persen>`
  dipakai sebagai bukti headless (pola sama dengan `notepad`).

> Catatan probe: capture QEMU memberi jeda 1 dtk; untuk langkah yang
> menunggu pemrosesan event berantai (mis. dua penekanan tombol cepat) jeda
> 2,5 dtk diperlukan — pada jeda pendek frame bisa tertangkap **sebelum** app
> selesai memproses, sehingga tampak "tidak berubah" padahal bukan bug render.
