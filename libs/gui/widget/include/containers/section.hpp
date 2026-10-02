// libs/widget/include/containers/section.hpp — bagian berjudul dalam halaman.
//
// Ini primitif yang membuat halaman pengaturan/form terbaca tanpa kartu:
//
//     ── LABEL BAGIAN ─────────────────────────────
//     baris isi
//     baris isi
//     ─────────────────────────────────────────────
//
// Aturan bahasa visual:
//   * Judul bagian memakai peran `section` (huruf besar, tone sekunder,
//     tracking) — hierarki dari TIPOGRAFI, bukan dari kotak berwarna.
//   * Judul diikuti garis tipis (border_subtle) selebar area konten. Garis
//     itu yang memisahkan bagian, jadi tidak perlu border/kartu di sekeliling
//     isi.
//   * Tanpa latar sendiri: bagian adalah bagian dari halaman.
//
// Komposisi (bukan warisan): Section memakai Layout untuk menyusun isinya,
// jadi ownership/damage/fokus/traversal otomatis ikut mekanisme yang sudah ada.
#ifndef KWIDGET_CONTAINERS_SECTION_HPP
#define KWIDGET_CONTAINERS_SECTION_HPP

#include "layout/layout.hpp"

namespace ui {

class Section : public Layout {
public:
    char* title;
    int gap;                 // jarak antar anak
    int title_gap;           // jarak judul → anak pertama
    bool rule;               // gambar garis setelah judul

    Section(const char* t, int spacing)
        : title(_ui_strdup(t ? t : "")), gap(spacing), title_gap(space::MD),
          rule(true) {
        w = 0;
        h = 0;
    }
    virtual ~Section() { _ui_free(title); }

    void set_title(const char* t) {
        char* n = _ui_strdup(t ? t : "");
        if (!n) return;
        mark_area(x, y, w, h);
        _ui_free(title);
        title = n;
        mark_area(x, y, w, h);
    }
    // Tinggi judul termasuk jarak ke garis.
    int header_h() const {
        const Typography ty;
        return ty.section.bitmap_line_h + space::SM;
    }
    virtual void arrange() override {
        int cy = y + header_h();
        for (int i = 0; i < count; i++) {
            if (!children[i]->visible) continue;
            place(children[i], x, cy);
            cy += children[i]->h + gap;
        }
        h = cy - y - (count > 0 ? gap : 0);
        if (h < header_h()) h = header_h();
    }
    virtual void draw(Painter& p) override {
        arrange();
        const TypeRole& role = p.theme.type.section;
        // Judul: huruf besar + tone sekunder (peran `section`).
        p.text_role(title, x, y, p.theme.tone(role.tone), role);
        if (rule) {
            int ry = y + role.bitmap_line_h + 2;
            p.rect(x, ry, w, 1, p.theme.border_subtle);
        }
        for (int i = 0; i < count; i++)
            if (children[i]->visible) children[i]->draw(p);
    }
    // Section tidak menyita fokus sendiri (judul bukan kontrol); anak-anaknya
    // tetap ikut traversal lewat dirty_child_count() dari Layout.
    virtual bool focusable() override { return false; }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_SECTION_HPP
