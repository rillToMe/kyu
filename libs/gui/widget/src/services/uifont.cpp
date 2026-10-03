// libs/widget/src/services/uifont.cpp — pemuat font UI sistem (opsional).
//
// DIBANGUN SEBAGAI OBJEK TERPISAH (kyuzen-widget-uifont), BUKAN bagian dari
// glob `src/*/*.cpp`:
// file ini memanggil kz_font_* (libs/text) dan karenanya memerlukan arsip
// FreeType saat link. Toolkit inti SENGAJA tidak men-link FreeType (keputusan
// arsitektur yang dipertahankan), jadi aplikasi yang ingin tipografi nyata
// meng-link library ini SECARA EKSPLISIT:
//
//     LIBS ... kyuzen-widget-uifont kyuzen-text-manager kyuzen-text-raster kyuzen-freetype
//
// dan memanggil ui::uifont_install() sebelum widget pertama dibuat.
#include "services/uifont.hpp"

#include "core/text_provider.hpp"
#include "runtime/platform.hpp"

extern "C" {
#include "kzfont.h"      // kz_font_* (libs/text)
#include "kzfonts.h"     // registry id/nama/berkas font
#include "kzfontcfg.h"   // format /font.ui
}

namespace ui {

namespace {

// Heap kz di atas sys_alloc (pola fontdemo / Settings Fonts).
void* kzAlloc(uint32_t n) { return sys_alloc(n ? n : 0x20); }
void kzFree(void* p) { sys_free(p); }
void* kzRealloc(void* p, uint32_t o, uint32_t n) { return sys_realloc(p, o, n); }
const kz_heap_t kHeap = { kzAlloc, kzFree, kzRealloc };

// State layanan. Pointer POD (nol-kan statis): aturan toolkit melarang objek
// global dengan konstruktor non-trivial (ELF loader tidak menjalankan
// .init_array).
kz_font_t* g_font = 0;
void* g_blob = 0;
int g_active = 0;

// --- Provider: ukur + gambar lewat libs/text ---
int provMeasure(void* ud, const char* text, int n) {
    (void)ud; (void)n;
    if (!g_font || !text) return 0;
    uint32_t w = 0, h = 0;
    if (kz_text_measure(g_font, text, &w, &h) != 0) return 0;
    return (int)w;
}

int provLineHeight(void* ud) {
    (void)ud;
    if (!g_font) return 0;
    // Tinggi baris = tinggi glyph aktif + sedikit napas. Nilai inilah yang
    // membuat kotak kontrol dan baris daftar ikut membesar saat pengguna
    // memilih font yang lebih besar.
    uint32_t w = 0, h = 0;
    if (kz_text_measure(g_font, "Ag", &w, &h) != 0 || h == 0) return 0;
    return (int)h + 4;
}

int provAscent(void* ud) {
    (void)ud;
    if (!g_font) return 0;
    uint32_t w = 0, h = 0;
    if (kz_text_measure(g_font, "Ag", &w, &h) != 0 || h == 0) return 0;
    // Baseline ~3/4 tinggi glyph: cukup untuk latin tanpa descender panjang,
    // dan menjaga teks tetap di dalam kotak kontrol.
    return (int)h - (int)h / 4;
}

void provDraw(void* ud, uint32_t* canvas, int cw, int ch, int x, int baseline_y,
              color_t color, const char* text) {
    (void)ud;
    if (!g_font || !canvas || !text) return;
    int dmg[4];
    kz_text_draw(canvas, (uint32_t)cw, (uint32_t)ch, g_font, x, baseline_y,
                 color, text, dmg);
}

TextProvider g_provider = {
    provMeasure, provLineHeight, provAscent, provDraw, 0
};

// Baca /font.ui -> id font. Absen/rusak -> default (tidak pernah gagal keras).
int readFontChoice() {
    if (sys_file_size(const_cast<char*>(KZ_FONT_CFG_PATH)) != KZ_FONT_CFG_LEN)
        return (int)KZ_FONT_DEFAULT;
    char buf[KZ_FONT_CFG_LEN];
    for (int i = 0; i < KZ_FONT_CFG_LEN; i++) buf[i] = 0;
    if (sys_read_file_to_buffer(const_cast<char*>(KZ_FONT_CFG_PATH), buf,
                                KZ_FONT_CFG_LEN) != 1)
        return (int)KZ_FONT_DEFAULT;
    return kz_font_cfg_decode(buf, KZ_FONT_CFG_LEN);
}

}  // namespace

int uifont_active() { return g_active; }

void uifont_uninstall() {
    text_provider_set(0);   // lepas provider SEBELUM font dihancurkan
    g_active = 0;
    if (g_font) { kz_font_destroy(g_font); g_font = 0; }
    if (g_blob) { sys_free(g_blob); g_blob = 0; }
}

int uifont_install() {
    if (g_active) return 1;                 // idempoten

    const int id = kz_font_id_sanitize(readFontChoice());
    const char* path = KZ_FONT_FILES[id];
    uint32_t sz = sys_file_size(const_cast<char*>(path));
    if (sz < 1000 || sz > 8u * 1024u * 1024u) return 0;
    char* buf = static_cast<char*>(sys_alloc(sz));
    if (!buf) return 0;
    if (sys_read_file_to_buffer(const_cast<char*>(path), buf, sz) != 1) {
        sys_free(buf);
        return 0;
    }
    g_blob = buf;

    kz_font_blob_t blob = { reinterpret_cast<const uint8_t*>(buf), sz };
    kz_font_t* f = kz_font_load(&blob, &kHeap, &kz_ft_backend);
    if (!f) { sys_free(buf); g_blob = 0; return 0; }
    if (kz_font_set_size(f, 15) != 0) {
        kz_font_destroy(f);
        sys_free(buf);
        g_blob = 0;
        return 0;
    }
    g_font = f;
    text_provider_set(&g_provider);
    g_active = 1;
    return 1;
}

} // namespace ui
