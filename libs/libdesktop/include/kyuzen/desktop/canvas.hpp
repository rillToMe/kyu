// libdesktop — adaptor rendering tipis di atas libgui (PIMPL; tipe window
// libgui TIDAK bocor ke aplikasi).
//
// Kebijakan render ("apa yang digambar") milik Shell implementasi.
// Framework hanya menyediakan mekanisme: gambar → tandai damage → flush
// SEKALI per iterasi bila ada damage. Tidak ada framebuffer/compositor/
// damage-tracking duplikat di sini: semua itu tetap milik libgui/KWM.
//
// Model damage (tri-state generik, tanpa konsep "taskbar" di framework):
//   None    = tak ada yang berubah → flush dilewati total (bahkan pemanggil
//             flush backend tidak dipanggil; perilaku pointer-move lama)
//   Partial = area dinamis saja yang digambar ulang (implementasi default:
//             strip taskbar; test-desktop: sama dengan Full)
//   Full    = seluruh layar digambar ulang
#ifndef KYUZEN_DESKTOP_CANVAS_HPP
#define KYUZEN_DESKTOP_CANVAS_HPP

#include <stdint.h>
#include <kyuzen/desktop/geometry.hpp>

namespace kyuzen {
namespace desktop {

enum class Damage {
    None,
    Partial,
    Full,
};

class Canvas {
public:
    Canvas();
    ~Canvas();

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    int width() const;
    int height() const;

    void fill_rect(const Rect& r, Color c);
    void draw_text(const char* text, Point p, Color c);

    // Blit ARGB8888 (alpha per-pixel) dengan blend ke latar canvas.
    // a=0 dilewati (latar utuh), a=0xFF overwrite, sisanya blend integer.
    // Hasil selalu opaque agar deklarasi opaque compositor valid.
    void draw_px(int x, int y, const uint32_t* px, int w, int h);

    // Tandai cakupan yang berubah (dipakai Shell yang menggambar langsung
    // di luar fill_rect/draw_text — hari ini tak ada; disiapkan untuk itu).
    void mark_full();
    void mark_region(const Rect& r);

    // Konsumsi damage tertunda (dipakai Application; Shell biasa tak perlu).
    Damage take_damage();

    // Kirim ke layar. Dipanggil framework SEKALI per iterasi yang
    // ber-damage — aplikasi TIDAK memanggil flush() sendiri.
    void flush();

private:
    struct Impl;
    Impl* impl_;
    // Application (friend) mencantolkan window miliknya. Pointer opaque
    // agar tipe window renderer tak bocor ke header publik.
    void attach(void* win);
    friend class Application;
};

}  // namespace desktop
}  // namespace kyuzen

#endif
