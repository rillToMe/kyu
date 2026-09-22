// libs/widget/include/primitives/image.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_IMAGE_HPP
#define KWIDGET_PRIMITIVES_IMAGE_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

// Dekoder PNG bersama (libs/media/png.c, stb_image) — dilink oleh app yang memakai
// Image widget. Di-declare extern "C" karena png.c adalah file C.
extern "C" uint32_t* png_decode(const char* filename, int* out_w, int* out_h);
extern "C" void png_free(uint32_t* buf);

namespace ui {

// ------------------------------------------------------------
// Image — PNG dari KyuzenFS, nearest-neighbor ke rect (Phase 7)
// ------------------------------------------------------------
class Image : public Widget {
public:
    uint32_t* px;
    int iw, ih;
    int percent;          // skala aktif (10..400), di-set lewat set_scale/set_fit
    Image(const char* filename, int dw, int dh) : px(0), iw(0), ih(0), percent(100) {
        w = dw; h = dh;
        px = png_decode(filename, &iw, &ih);
    }
    virtual ~Image() { png_free(px); }
    // Phase 10: zoom viewer — target display size dihitung ulang dari ukuran
    // natural PNG (persen 10..400). `w`/`h` jadi area target yang digambar.
    void set_scale(int p) {
        if (p < 10) p = 10;
        if (p > 400) p = 400;
        percent = p;
        mark_dirty();                     // bounds lama (bisa mengecil)
        if (iw > 0) { w = iw * percent / 100; h = ih * percent / 100; }
        mark_dirty();                     // bounds baru
    }
    // Phase 11: skala agar SELURUH gambar masuk view (view_w × view_h). Return
    // persen efektif setelah clamp (0 bila tak ada gambar). Rasio dipilih dari
    // sumbu yang paling sempit supaya kedua sisi pasti masuk.
    int set_fit(int view_w, int view_h) {
        if (iw <= 0 || ih <= 0 || view_w <= 0 || view_h <= 0) return 0;
        int pw = view_w * 100 / iw;
        int ph = view_h * 100 / ih;
        set_scale(pw < ph ? pw : ph);
        return percent;
    }
    // Phase 10: ganti file PNG (viewer galeri) — muat ulang, reset zoom 100%.
    void set_file(const char* filename) {
        mark_dirty();
        png_free(px);
        px = png_decode(filename, &iw, &ih);
        percent = 100;
        if (iw > 0) { w = iw; h = ih; }   // natural size; ScrollView menyesuaikan
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        if (!px || iw <= 0 || ih <= 0) { p.rect(x, y, w, h, p.theme.button_bg); return; }
        p.image(x, y, w, h, px, iw, ih);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_IMAGE_HPP
