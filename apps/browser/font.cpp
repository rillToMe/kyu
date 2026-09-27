// apps/browser/font.cpp — loader Inter + measure/draw.
#include "font.hpp"

#include "platform.hpp"

namespace browser {
namespace {

// Allocator kz_heap di atas sys_alloc (pola settings/fonts.cpp).
void* fontAlloc(std::uint32_t size) { return sys_alloc(size); }
void fontFree(void* ptr) {
    if (ptr) sys_free(ptr);
}
void* fontRealloc(void* ptr, std::uint32_t old_size, std::uint32_t new_size) {
    return sys_realloc(ptr, old_size, new_size);
}

}  // namespace

BrowserFont::BrowserFont() : font_(nullptr), file_data_(nullptr), cur_size_(0) {}

BrowserFont::~BrowserFont() {
    if (font_) kz_font_destroy(static_cast<kz_font_t*>(font_));
    if (file_data_) sys_free(file_data_);
}

bool BrowserFont::load() {
    const char* path = KZ_FONT_FILES[KZ_FONT_DEFAULT];
    std::uint32_t sz = sys_file_size(const_cast<char*>(path));
    if (sz < 1000 || sz > 8u * 1024u * 1024u) return false;
    char* buf = static_cast<char*>(sys_alloc(sz));
    if (!buf) return false;
    if (sys_read_file_to_buffer(const_cast<char*>(path), buf, sz) != 1) {
        sys_free(buf);
        return false;
    }
    kz_heap_t heap = {fontAlloc, fontFree, fontRealloc};
    kz_font_blob_t blob = {reinterpret_cast<const std::uint8_t*>(buf), sz};
    kz_font_t* f = kz_font_load(&blob, &heap, &kz_ft_backend);
    if (!f) {
        sys_free(buf);
        return false;
    }
    file_data_ = buf;
    font_ = f;
    cur_size_ = 0;
    return true;
}

void BrowserFont::use_size(int size) const {
    if (!font_) return;
    if (size < 8) size = 8;
    if (size > 72) size = 72;
    if (size != cur_size_) {
        if (kz_font_set_size(static_cast<kz_font_t*>(font_), (std::uint32_t)size) == 0)
            cur_size_ = size;
    }
}

int BrowserFont::width(const std::string& text, int size, bool bold) const {
    if (!font_ || text.empty()) return 0;
    use_size(size);
    std::uint32_t w = 0, h = 0;
    if (kz_text_measure(static_cast<kz_font_t*>(font_), text.c_str(), &w, &h) != 0)
        return 0;
    int out = (int)w;
    if (bold) out += (int)text.size();  // dobel strike ~1px/char
    return out;
}

int BrowserFont::line_height(int size) const {
    if (!font_) return size + 4;
    use_size(size);
    std::uint32_t w = 0, h = 0;
    // Ukur "Ag" (ascender+descender wakil) agar tinggi stabil per ukuran.
    if (kz_text_measure(static_cast<kz_font_t*>(font_), "Ag", &w, &h) != 0)
        return size + 4;
    return (int)h + 4;
}

int BrowserFont::draw(std::uint32_t* canvas, std::uint32_t cw, std::uint32_t ch, int x,
                      int baseline_y, std::uint32_t argb, const std::string& text,
                      bool bold) {
    if (!font_ || !canvas || text.empty()) return -1;
    // Ukuran aktif = cur_size_ (layout memanggil use_size via measure saat
    // build; draw mengasumsikan ukuran sudah benar untuk run ini).
    color_t c = color_from_u32(argb, FORMAT_ARGB);
    int dmg[4];
    int rc = kz_text_draw(canvas, cw, ch, static_cast<kz_font_t*>(font_), x, baseline_y,
                          c, text.c_str(), dmg);
    if (bold && rc >= 0)
        kz_text_draw(canvas, cw, ch, static_cast<kz_font_t*>(font_), x + 1, baseline_y,
                     c, text.c_str(), dmg);
    return rc;
}

}  // namespace browser
