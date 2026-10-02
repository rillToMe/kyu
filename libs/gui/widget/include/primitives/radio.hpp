// libs/widget/include/primitives/radio.hpp — Radio + RadioGroup.
//
// Geometri SELARAS CheckBox: indikator 16px + gap SM + label, tinggi kontrol
// sama. Dua kontrol yang berdampingan dalam satu form harus punya garis dasar
// dan kolom label yang identik — itu bagian dari "strong alignment".
//
// Indikator digambar sebagai dua permukaan (cincin + dot) alih-alih loop
// piksel per-piksel: lebih murah, dan hasilnya konsisten dengan kontrol lain
// yang memakai radius token.
#ifndef KWIDGET_PRIMITIVES_RADIO_HPP
#define KWIDGET_PRIMITIVES_RADIO_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"

namespace ui {

class RadioGroup;   // fwd: Radio pegang grup — definisi di bawah (butuh Radio lengkap)

class Radio : public Widget {
public:
    char* label;
    RadioGroup* group;       // 0 = mandiri (selalu bisa dipilih, tak bisa batal)
    bool hover;
    bool checked_self;       // state bila tanpa grup
    ui_click_cb change_cb;
    void* change_data;

    Radio(const char* t);
    virtual ~Radio();
    bool is_selected() const;
    void set_group(RadioGroup* g);
    // Programatik diam-diam (konvensi set_checked/set_selected): dalam grup
    // hanya "nyalakan" yang berlaku (radio klasik tak bisa batal-klik);
    // mandiri: bebas set on/off.
    void set_selected(bool on);
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool on) override {
        if (hover == on) return;
        hover = on;
        mark_dirty();
    }
    virtual void on_click(int mx, int my) override;
    // Keyboard: panah pindah + pilih dalam grup (skip disabled),
    // Enter/Spasi pilih yang fokus. Definisi di radio.cpp (butuh Window
    // lengkap untuk memindahkan fokus).
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override;
    virtual void draw(Painter& p) override;
    // Tetangga segrup ke arah dir (-1/+1), lewati disabled; 0 bila tak ada.
    // Tanpa grup: 0 (mandiri tak punya navigasi).
    Radio* sibling(int dir);
};

// ------------------------------------------------------------
// RadioGroup — pemilik logis pilihan (BUKAN widget: tak digambar, tak punya
// bounds, tak masuk traversal). Tanpa global state; cleanup dua arah:
// hapus grup → anggota terlepas; hapus radio → keluar dari grup (termasuk
// bila sedang terpilih). Alokasi: anggota milik layout seperti widget lain.
// ------------------------------------------------------------
class RadioGroup {
public:
    enum { MAX_RADIOS = 8 };
    Radio* members[MAX_RADIOS];
    int n;
    Radio* selected;

    RadioGroup() : n(0), selected(0) {
        for (int i = 0; i < MAX_RADIOS; i++) members[i] = 0;
    }
    ~RadioGroup() {
        for (int i = 0; i < n; i++)
            if (members[i]) members[i]->group = 0;
    }
    void add(Radio* r) {
        if (!r || n >= MAX_RADIOS) return;
        for (int i = 0; i < n; i++)
            if (members[i] == r) return;   // sudah anggota
        members[n++] = r;
        r->group = this;
        r->mark_dirty();
    }
    void remove(Radio* r) {
        if (!r) return;
        for (int i = 0; i < n; i++) {
            if (members[i] != r) continue;
            for (int j = i; j + 1 < n; j++) members[j] = members[j + 1];
            members[--n] = 0;
            break;
        }
        if (selected == r) selected = 0;   // terpilih dihapus → grup kosong
        r->group = 0;
        r->mark_dirty();
    }
    // Pilih r (diam bila r bukan anggota / disabled / sudah terpilih).
    // fire = panggil change_cb (klik/keyboard); programatik = diam.
    void select(Radio* r, bool fire) {
        if (!r || r->group != this || !r->enabled) return;
        if (selected == r) return;         // tanpa perubahan → tanpa callback
        for (int i = 0; i < n; i++)
            if (members[i]) members[i]->mark_dirty();
        selected = r;
        r->mark_dirty();
        if (fire && r->change_cb) r->change_cb(r->change_data);
    }
};

inline Radio::Radio(const char* t)
    : label(_ui_strdup(t)), group(0), hover(false), checked_self(false),
      change_cb(0), change_data(0) {
    const Metrics m;
    h = m.control_h_sm;
    w = text_width(label, Typography().label) + m.control_h_sm + m.sm;
}

inline Radio::~Radio() {
    if (group) group->remove(this);   // keluar grup (grup tetap valid)
    _ui_free(label);
}

inline bool Radio::is_selected() const {
    return group ? group->selected == this : checked_self;
}

inline void Radio::set_group(RadioGroup* g) {
    if (group == g) return;
    if (group) group->remove(this);
    if (g) g->add(this);
    else mark_dirty();
}

inline void Radio::set_selected(bool on) {
    if (group) {
        if (on) group->select(this, false);
    } else if (checked_self != on) {
        checked_self = on;
        mark_dirty();
    }
}

inline void Radio::on_click(int mx, int my) {
    (void)mx; (void)my;
    if (!enabled) return;
    if (group) group->select(this, true);
    else if (!checked_self) {
        checked_self = true;
        mark_dirty();
        if (change_cb) change_cb(change_data);
    }
}

inline Radio* Radio::sibling(int dir) {
    if (!group) return 0;
    int idx = -1;
    for (int i = 0; i < group->n; i++)
        if (group->members[i] == this) { idx = i; break; }
    if (idx < 0) return 0;
    for (int k = 0; k < group->n; k++) {
        idx = (idx + dir + group->n) % group->n;
        Radio* c = group->members[idx];
        if (c && c->enabled) return c;
    }
    return 0;   // semua disabled
}

inline void Radio::draw(Painter& p) {
    const Metrics& m = p.theme.metrics;
    const TypeRole& role = p.theme.type.body;
    StateInputs st;
    st.hover = hover;
    st.focused = has_focus;
    st.enabled = enabled;
    const bool sel = is_selected();

    const int box = m.icon_md;                 // 16px — selaras CheckBox
    const int by = y + (h - box) / 2;
    // Cincin: radius PILL supaya benar-benar bulat (satu-satunya tempat
    // radius pill dipakai untuk kontrol — bentuknya memang bulat).
    color_t ring = !enabled  ? p.theme.border_subtle
                 : has_focus ? p.theme.focus
                 : hover     ? p.theme.text_secondary
                             : p.theme.border;
    color_t fill = !enabled ? p.theme.surface_variant : p.theme.surface;
    p.surface(x, by, box, box, fill, radius::PILL);
    p.rrect_border(x, by, box, box, radius::PILL, ring, 255);
    if (sel) {
        // Dot = permukaan kedua di tengah, bukan gambar piksel: konsisten
        // dengan cincin dan tetap tajam pada semua ukuran.
        const int d = box / 2 + 1;
        color_t dot = enabled ? p.theme.accent : p.theme.text_disabled;
        p.surface(x + (box - d) / 2, by + (box - d) / 2, d, d, dot, radius::PILL);
    }
    if (label && label[0]) {
        int tx = x + box + m.sm;
        int avail = w - box - m.sm;
        p.text_ellipsis(label, tx, y + text_vcenter(h), avail,
                        state_text(p.theme, st), role);
    }
}

} // namespace ui

#endif // KWIDGET_PRIMITIVES_RADIO_HPP
