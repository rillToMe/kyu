#ifndef KZFONTS_H
#define KZFONTS_H

// ============================================================
// libs/text — UI font registry (single source of truth).
//
// Dipakai desktop (system/desktop), Settings (apps/settings/), dan
// Font Demo untuk NAMA/URUTAN/ID yang sama. Data blob (embedded vs
// FS) tetap milik masing-masing app — yang disatukan di sini adalah
// IDENTITAS: id stabil + display name + nama file FS.
//
// Kebijakan (§2/§4/§9/§25):
//   - id 0 = Inter Regular = default eksplisit (bukan "index 0 tanpa
//     definisi"). Adlam terakhir (test font, bukan default UI).
//   - Display name terpisah dari id (ganti nama tampil tanpa
//     mengubah config tersimpan).
//   - Menambah font = tambah baris di tabel bawah + sediakan TTF-nya
//     (embed atau modul FS); tidak ada daftar kedua di app.
//
// Pure header (tanpa syscall/alokasi) — aman di-include C maupun C++
// (termasuk host test).
// ============================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KZ_FONT_INTER_REGULAR = 0,  // default UI font (eksplisit)
    KZ_FONT_DEJAVU_SANS = 1,
    KZ_FONT_NOTO_MONO = 2,
    KZ_FONT_NOTO_MONO_BOLD = 3,
    KZ_FONT_NOTO_ADLAM = 4,  // test font (cakupan Adlam saja)
    KZ_FONT_COUNT = 5,
    KZ_FONT_DEFAULT = KZ_FONT_INTER_REGULAR,
} kz_ui_font_id_t;

// Nama file TTF di akar FS (modul ISO -> FS root, pola DESKTOP_ASSETS).
static const char *const KZ_FONT_FILES[KZ_FONT_COUNT] = {
    "/Inter-Regular.ttf",
    "/DejaVuSans.ttf",
    "/NotoSansMono-Regular.ttf",
    "/NotoSansMono-Bold.ttf",
    "/NotoSansAdlam-Regular.ttf",
};

// Display name (UTF-8, ASCII-only agar aman di semua renderer).
static const char *const KZ_FONT_NAMES[KZ_FONT_COUNT] = {
    "Inter Regular",
    "DejaVu Sans",
    "Noto Sans Mono",
    "Noto Sans Mono Bold",
    "Noto Sans Adlam",
};

// Validasi id dari config (invalid -> default, bukan crash).
static inline int kz_font_id_valid(int id) {
    return id >= 0 && id < (int)KZ_FONT_COUNT;
}

static inline int kz_font_id_sanitize(int id) {
    return kz_font_id_valid(id) ? id : (int)KZ_FONT_DEFAULT;
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KZFONTS_H
