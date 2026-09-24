#ifndef KZFONTCFG_H
#define KZFONTCFG_H

// ============================================================
// libs/text — format konfigurasi UI font ("font.ui").
//
// Desain (§10): minimal, mengikuti pola settings.ui (tag + payload),
// TANPA settings database. File = 5 byte di akar FS ("/font.ui",
// absolut agar Settings dan desktop membaca file yang SAMA tanpa
// tergantung cwd masing-masing):
//
//   offset 0..3 : "KZF1" (tag)
//   offset 4    : font id (kz_ui_font_id_t, 0..4)
//
// Header ini HANYA format + validasi murni (tanpa syscall) sehingga
// bisa dipakai desktop (lewat sys_abi), Settings (userlib langsung),
// dan host test. IO 20-baris ada di masing-masing sisi karena boundary
// isolasi desktop melarang include userlib.h langsung — formatnya yang
// menjadi single source of truth, bukan kode IO-nya.
//
// Invalid (magic salah / ukuran beda / id di luar rentang) ->
// KZ_FONT_DEFAULT. Tidak pernah crash, tidak pernah NULL font.
// ============================================================

#include <stdint.h>
#include "kzfonts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KZ_FONT_CFG_PATH "/font.ui"
#define KZ_FONT_CFG_LEN 5
#define KZ_FONT_CFG_TAG0 'K'
#define KZ_FONT_CFG_TAG1 'Z'
#define KZ_FONT_CFG_TAG2 'F'
#define KZ_FONT_CFG_TAG3 '1'

// Encode pilihan ke buffer 5 byte. Return panjang (selalu 5).
static inline int kz_font_cfg_encode(int id, char out[5]) {
    out[0] = KZ_FONT_CFG_TAG0;
    out[1] = KZ_FONT_CFG_TAG1;
    out[2] = KZ_FONT_CFG_TAG2;
    out[3] = KZ_FONT_CFG_TAG3;
    out[4] = (char)kz_font_id_sanitize(id);
    return KZ_FONT_CFG_LEN;
}

// Decode buffer (n = byte terbaca). Return id valid (default bila rusak).
static inline int kz_font_cfg_decode(const char *buf, int n) {
    if (!buf || n != KZ_FONT_CFG_LEN) return (int)KZ_FONT_DEFAULT;
    if (buf[0] != KZ_FONT_CFG_TAG0 || buf[1] != KZ_FONT_CFG_TAG1 ||
        buf[2] != KZ_FONT_CFG_TAG2 || buf[3] != KZ_FONT_CFG_TAG3)
        return (int)KZ_FONT_DEFAULT;
    return kz_font_id_sanitize((int)(unsigned char)buf[4]);
}

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KZFONTCFG_H
