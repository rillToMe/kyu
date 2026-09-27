// apps/browser/font.hpp — font Inter + adapter TextMeasure engine.
#ifndef BROWSER_FONT_HPP
#define BROWSER_FONT_HPP

#include <cstdint>
#include <string>

#include "engine/layout.hpp"

namespace browser {

// Memuat /Inter-Regular.ttf dari FS (pola settings/fonts.cpp). Satu ukuran
// aktif per font manager: set_size di-cache (ganti hanya bila beda).
class BrowserFont : public layout::TextMeasure {
public:
    BrowserFont();
    ~BrowserFont();

    bool load();  // false bila TTF tak terbaca (viewport fallback 8x16)
    bool loaded() const { return font_ != nullptr; }

    // layout::TextMeasure (lock ukuran + kz_text_measure).
    int width(const std::string& text, int size, bool bold) const override;
    int line_height(int size) const override;

    // Gambar UTF-8 pada canvas (langsung kz_text_draw). Bold = dobel strike
    // 1px (tidak ada file bold; terdokumentasi). Return seperti kz_text_draw.
    int draw(std::uint32_t* canvas, std::uint32_t cw, std::uint32_t ch, int x,
             int baseline_y, std::uint32_t argb, const std::string& text, bool bold);

    // Ukuran terakhir yang di-set (agar layout bisa sinkron sebelum measure).
    void use_size(int size) const;

private:
    void* font_ = nullptr;    // kz_font_t* (opaque, hindari include di header)
    void* file_data_ = nullptr;
    mutable int cur_size_ = 0;
};

}  // namespace browser

#endif  // BROWSER_FONT_HPP
