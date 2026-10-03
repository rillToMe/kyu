// apps/clock/bigtime.cpp — font hero milik aplikasi (lihat bigtime.hpp).
#include "bigtime.hpp"


namespace clock_app {

namespace {

// Allocator kz_heap di atas sys_alloc (pola browser/font.cpp, settings/fonts.cpp).
void* fontAlloc(uint32_t n) { return sys_alloc(n ? n : 0x20); }
void  fontFree(void* p) { if (p) sys_free(p); }
void* fontRealloc(void* p, uint32_t o, uint32_t n) { return sys_realloc(p, o, n); }

// Pilihan font pengguna dari /font.ui; absen/rusak -> default.
int read_font_choice() {
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

BigFont::BigFont() : font_(0), blob_(0) {}

BigFont::~BigFont() {
    if (font_) kz_font_destroy(font_);
    if (blob_) sys_free(blob_);
}

int BigFont::load() {
    if (font_) return 1;
    const int id = kz_font_id_sanitize(read_font_choice());
    const char* path = KZ_FONT_FILES[id];
    uint32_t sz = sys_file_size(const_cast<char*>(path));
    if (sz < 1000 || sz > 8u * 1024u * 1024u) return 0;
    char* buf = static_cast<char*>(sys_alloc(sz));
    if (!buf) return 0;
    if (sys_read_file_to_buffer(const_cast<char*>(path), buf, sz) != 1) {
        sys_free(buf);
        return 0;
    }
    const kz_heap_t heap = { fontAlloc, fontFree, fontRealloc };
    const kz_font_blob_t blob = { reinterpret_cast<const uint8_t*>(buf), sz };
    kz_font_t* f = kz_font_load(&blob, &heap, &kz_ft_backend);
    if (!f) { sys_free(buf); return 0; }
    blob_ = buf;
    font_ = f;
    return 1;
}

// Ukuran di-set per penggambaran. Ini bukan pemborosan: kz_font_set_size
// langsung kembali bila ukurannya sama (lihat use_size di browser/font.cpp),
// jadi biaya sebenarnya hanya saat ukuran benar-benar berganti.
void BigFont::measure_hero(const char* s, int* w, int* h) const {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!font_ || !s) return;
    if (kz_font_set_size(font_, HERO_PX) != 0) return;
    uint32_t mw = 0, mh = 0;
    if (kz_text_measure(font_, s, &mw, &mh) != 0) return;
    if (w) *w = (int)mw;
    if (h) *h = (int)mh;
}

void BigFont::measure_sub(const char* s, int* w, int* h) const {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!font_ || !s) return;
    if (kz_font_set_size(font_, SUB_PX) != 0) return;
    uint32_t mw = 0, mh = 0;
    if (kz_text_measure(font_, s, &mw, &mh) != 0) return;
    if (w) *w = (int)mw;
    if (h) *h = (int)mh;
}

int BigFont::draw_hero(uint32_t* canvas, int cw, int ch, int x, int baseline_y,
                       color_t fg, color_t bg, const char* text) {
    if (!font_ || !canvas || !text) return 0;
    if (kz_font_set_size(font_, HERO_PX) != 0) return 0;
    int dmg[4];
    // kz_text_draw_on = IDEMPOTEN (UI-8.3). kz_text_draw menumpuk di atas isi
    // canvas dan akan menebalkan tepi glyph tiap penggambaran ulang.
    return kz_text_draw_on(canvas, (uint32_t)cw, (uint32_t)ch, font_, x,
                           baseline_y, fg, bg, text, dmg) >= 0 ? 1 : 0;
}

int BigFont::draw_sub(uint32_t* canvas, int cw, int ch, int x, int baseline_y,
                      color_t fg, color_t bg, const char* text) {
    if (!font_ || !canvas || !text) return 0;
    if (kz_font_set_size(font_, SUB_PX) != 0) return 0;
    int dmg[4];
    return kz_text_draw_on(canvas, (uint32_t)cw, (uint32_t)ch, font_, x,
                           baseline_y, fg, bg, text, dmg) >= 0 ? 1 : 0;
}

}  // namespace clock_app
