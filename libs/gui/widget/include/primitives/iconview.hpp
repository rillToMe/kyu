// libs/widget/include/primitives/iconview.hpp — ikon mandiri (bukan tombol).
//
// Kadang UI hanya perlu MENUNJUKKAN ikon: penanda di dalam baris, indikator
// status, ikon di samping judul. Tanpa widget ini, app akan menggambar
// bentuknya sendiri — persis yang dihindari sistem ikon.
//
// Ukuran diambil dari token `theme/metrics.hpp` lewat `ui_icon_size()` supaya
// ikon yang berdampingan dengan widget lain selalu satu ukuran optik.
#ifndef KWIDGET_PRIMITIVES_ICONVIEW_HPP
#define KWIDGET_PRIMITIVES_ICONVIEW_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

class IconView : public Widget {
public:
    int icon_id;
    int size_role;      // UI_ICON_SIZE_*
    color_t color;      // warna eksplisit; alpha 0 = pakai tone tema
    bool use_theme_color;

    IconView(int icon, int size_role_)
        : icon_id(icon), size_role(size_role_), color(), use_theme_color(true) {
        const Metrics m;
        int s = m.icon_md;
        switch (size_role) {
        case UI_ICON_SIZE_SM: s = m.icon_sm; break;
        case UI_ICON_SIZE_LG: s = m.icon_lg; break;
        case UI_ICON_SIZE_XL: s = m.icon_xl; break;
        case UI_ICON_SIZE_MD:
        default:              s = m.icon_md; break;
        }
        w = s;
        h = s;
    }
    void set_icon(int icon) {
        icon_id = icon;
        mark_dirty();
    }
    // Warna eksplisit (mis. ikon status yang memang berwarna). Alpha 0 =
    // kembali ke tone tema.
    void set_color(color_t c) {
        color = c;
        use_theme_color = (c.a == 0);
        mark_dirty();
    }
    // Ikon bukan kontrol: tidak menyita fokus dan klik menembus ke bawahnya.
    virtual Widget* pick(int mx, int my) override {
        (void)mx; (void)my;
        return 0;
    }
    virtual void draw(Painter& p) override {
        color_t c = use_theme_color ? p.theme.text_secondary : color;
        if (!enabled) c = p.theme.text_disabled;
        int s = w < h ? w : h;
        p.icon(icon_id, x + w / 2, y + h / 2, s, c);
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_ICONVIEW_HPP
