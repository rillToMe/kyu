# UI XML First Real Consumer — Phase F

> Phase E membangun lapisan XML + demo sintetis. Phase F membuktikan
> XML di aplikasi NYATA: satu halaman Settings dikonversi, tanpa
> perubahan build, tanpa regresi, tervalidasi QEMU headless.

## 1. Scope (dipilih dari audit, bukan selera)

- Konsumen: `AppearancePage` (`apps/settings/appearance.cpp`) — form statis
  (2 label + 3 tombol + status) + 3 binding = bentuk ideal XML.
- SENGAJA TIDAK: skema dialog/prompt (tanpa konsumen — aturan Phase E §16
  melarang ekspansi spekulatif), konversi halaman dinamis (System/
  Personalization/Fonts punya list/preview/thumbnail — bukan bentuk XML),
  codegen, stylesheet.
- Satu-satunya API baru yang terbukti perlu: inflasi DETACHED
  (`ui_xml_inflate_detached` + `ui_xml_root_at/count` + `ui_xml_release`) —
  karena commit-ke-window menyebabkan DOUBLE-PARENTING saat app
  me-parenting sendiri (widget yang sama di dua layout → render ganda
  kacau, ditemukan via forensik piksel QEMU).

## 2. Perubahan

- `apps/settings/appearance.cpp`: `build()` manual (12 pemanggilan API) →
  string XML statis + `parse/detached-inflate/find/bind/release/destroy`.
  `ThemeBinding` + `onTheme` + `applyTheme` dipakai ulang apa adanya.
  Fallback minimal bila inflasi gagal (anti-null-root).
- `include/libui_xml.h` + `abi/libui_abi.cpp`: 4 fungsi detached baru.
  Nol perubahan build: settings me-link glob toolkit (sumber xml ikut
  otomatis); `settings.elf` link OK (cek undefined/exception bersih).
- `tests/host/unit/libui_xml_test.cpp`: blok F (7 cek bentuk+binding) +
  detached/release (9 cek). Total 129 PASS / 0 FAIL, allocs=frees.
- `tests/host/probes/_appearance_probe.py`: boot→settings→sidebar→
  Appearance (serial)→klik Dark→delta status→ESC→bersih. 8/8 PASS.

## 3. Validasi QEMU (ringkas)

- Navigasi Appearance: serial `[settings] Appearance` setelah klik baris.
- Binding XML nyata: klik Dark → `applyTheme` → status
  `(from settings.ui)` → `Dark` (hash region berubah) + tema teraplikasi.
- Tanpa panic; window tutup via ESC.
- Pelajaran probe: ketik prompt-cepat harus absolute-search (race
  anchor-pos), klik ganda untuk target kecil, `test_disk.img` basi
  menyebabkan `start: file tidak ada`.

## 4. Known Limitations

1. Dialog/prompt BELUM ada di skema (tunggu konsumen nyata kedua).
2. `release` wajib setelah detached-inflate (kalau tidak, destroy membuang
   roots) — dikunci test F15/F16.
3. Grup radio + detached: ctx boleh mati kapan pun (two-way cleanup),
   roots tetap milik caller setelah release.
4. Probe appearance bergantung serial trace `showPage` (stabil) + geometri
   sidebar 150px/baris 20px (kontrak ListView).

## 5. Rekomendasi Berikutnya

- Konsumen kedua yang natural: halaman Fonts (list + preview) BUKAN bentuk
  XML — jangan dipaksa. Kandidat benar: dialog konfirmasi/error fileman
  sebagai resource XML (`<dialog>` + show API) saat fileman disentuh.
- Setelah DUA konsumen dialog: pertimbangkan `<dialog>`/`<prompt>` schema.
- Jangan perluas skema atribut (track-config, persen, warna CSS) tanpa
  layar konkret yang membutuhkan.
