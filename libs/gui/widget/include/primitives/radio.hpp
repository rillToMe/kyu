// libs/widget/include/primitives/radio.hpp — Radio + RadioGroup (Phase D).
#ifndef KWIDGET_PRIMITIVES_RADIO_HPP
#define KWIDGET_PRIMITIVES_RADIO_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"

namespace ui {

class RadioGroup;   // fwd: Radio pegang grup — definisi di bawah (butuh Radio lengkap)

// ------------------------------------------------------------
// Radio — lingkaran 12px + label; tepat satu terpilih dalam grup.
// Geometri selaras CheckBox (12px + label +20px, h=20) supaya sejajar
// dalam daftar. Lingkaran digambar piksel (bukan rrect) agar bundar penuh.
// ------------------------------------------------------------
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
    virtual void set_hover(bool on) override { hover = on; mark_dirty(); }
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
    w = _ui_strlen(label) * 8 + 20; h = 20;
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
    // Lingkaran d=12: ring 1px + dot aksen bila terpilih.
    bool sel = is_selected();
    color_t ring = !enabled      ? p.theme.border_subtle
                 : has_focus     ? p.theme.focus
                 : hover         ? p.theme.text
                                 : p.theme.border;
    color_t dot = !enabled ? p.theme.text_disabled : p.theme.accent;
    for (int dy = 0; dy < 12; dy++) {
        for (int dx = 0; dx < 12; dx++) {
            int ox = dx - 5, oy = dy - 5;   // pusat di antara 4 piksel tengah
            int d2 = ox * ox + oy * oy;
            if (d2 > 16 && d2 <= 30) p.rect(x + dx, y + dy, 1, 1, ring);
            else if (sel && d2 <= 9) p.rect(x + dx, y + dy, 1, 1, dot);
        }
    }
    p.text(label, x + 20, y + 2, enabled ? p.theme.text : p.theme.text_disabled);
}

} // namespace ui

#endif // KWIDGET_PRIMITIVES_RADIO_HPP
