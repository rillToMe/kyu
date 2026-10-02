// libs/widget/include/primitives/textbox.hpp — input teks satu baris.
//
// Bahasa visual input KyuzenOS:
//   * Permukaan TERBENAM (surface_variant) — input adalah "lubang" di halaman,
//     bukan tombol. Ini pembeda utama dari tombol, dan alasannya kenapa input
//     tidak butuh bayangan.
//   * Border 1px: subtle → hover → focus. Fokus juga menambah caret, jadi
//     posisi kursor selalu terlihat walau teks kosong.
//   * Teks DIGESER (scroll horizontal) saat lebih panjang dari lebar kontrol.
//     Tanpa ini, nama berkas panjang hilang begitu saja di luar kotak.
//
// Kontrak lama yang dipertahankan: `replace_next` (select-all untuk ganti-nama
// inline), `enter_cb`, dan `set_error()`.
#ifndef KWIDGET_PRIMITIVES_TEXTBOX_HPP
#define KWIDGET_PRIMITIVES_TEXTBOX_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "core/state.hpp"
#include "services/clipboard.hpp"

namespace ui {

class TextBox : public Widget {
public:
    enum { MAX_TEXT = 256 };
    char text[MAX_TEXT];
    int cur;                    // posisi kursor (indeks karakter)
    bool hover;
    bool error;
    // Seluruh isi "terpilih": tombol pengubah teks berikutnya MENGGANTI isi
    // alih-alih menambah (semantik ganti-nama Explorer).
    bool replace_next;
    ui_click_cb enter_cb;
    void* enter_data;

    TextBox(int width)
        : cur(0), hover(false), error(false), replace_next(false),
          enter_cb(0), enter_data(0) {
        const Metrics m;
        w = width;
        h = m.control_h;
        text[0] = '\0';
        cursor_kind = UI_CURSOR_IBEAM;
    }
    void set_text(const char* t) {
        int n = 0;
        if (t) while (t[n] && n < MAX_TEXT - 1) n++;
        for (int i = 0; i < n; i++) text[i] = t[i];
        text[n] = '\0';
        cur = n;
        replace_next = false;   // isi baru dari kode bukan target ganti
        mark_dirty();
    }
    void select_all() {
        int n = 0; while (text[n]) n++;
        replace_next = (n > 0);
        cur = n;
        mark_dirty();
    }
    void drop_pending() { replace_next = false; }
    void set_error(bool onv) {
        if (error == onv) return;
        error = onv;
        mark_dirty();
    }
    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool onv) override {
        if (hover == onv) return;
        hover = onv;
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        const Metrics& m = p.theme.metrics;
        const TypeRole& role = p.theme.type.body;
        StateInputs st;
        st.hover = hover;
        st.focused = has_focus;
        st.enabled = enabled;
        st.invalid = error;

        // Permukaan TERBENAM: input = "lubang" di halaman, bukan tombol.
        // (Ini yang membedakan input dari Button tanpa perlu bayangan.)
        color_t bg = p.theme.surface_variant;
        if (enabled && error) bg = theme_mix(p.theme.danger, bg, 26);
        const int r = m.radius_control;
        p.surface(x, y, w, h, bg, r);
        color_t bd = state_border_quiet(p.theme, st);
        if (enabled && has_focus) bd = p.theme.focus;
        p.rrect_border(x, y, w, h, r, bd, 255);

        const int pad = m.sm + 2;          // 10px: sejajar optik dengan tombol
        const int avail = w - 2 * pad;
        if (avail <= 0) return;
        const int adv = glyph::ADVANCE;
        int visible = avail / adv;
        if (visible < 1) visible = 1;

        // Scroll horizontal: jaga caret tetap terlihat. `first` = indeks
        // karakter pertama yang digambar.
        int first = 0;
        if (cur > visible) first = cur - visible;
        if (first > 0 && text[first - 1] == '\0') first--;

        int ty = y + text_vcenter(h);
        // Blok "akan diganti" (select-all) — tinta seleksi, bukan kotak baru.
        if (has_focus && replace_next) {
            int n = 0; while (text[n]) n++;
            int vn = n - first;
            if (vn > visible) vn = visible;
            if (vn > 0) p.rect(x + pad, y + (h - glyph::HEIGHT) / 2,
                               vn * adv, glyph::HEIGHT, p.theme.selection);
        }
        color_t txt = state_text(p.theme, st);
        if (text[first]) {
            p.set_clip(x + pad, y, avail, h);
            p.text_role(text + first, x + pad, ty, txt, role);
            p.clear_clip();
        }
        if (has_focus && enabled) {
            int cx = x + pad + (cur - first) * adv;
            if (cx >= x + pad && cx < x + pad + avail)
                p.rect(cx, y + (h - glyph::HEIGHT) / 2, 1, glyph::HEIGHT,
                       p.theme.caret);
        }
    }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        if (!enabled) return;
        mark_dirty();   // teks/kursor/kotak fokus bisa berubah
        // Ctrl+C/X/V = clipboard (P1 dasar 'c'/'x'/'v', plus control-code
        // variant 0x03/0x18/0x16 bila driver memetakannya).
        if (mods & KEY_MOD_CTRL) {
            switch (ascii) {
            case 'c': case 'C': case 0x03: clipboard_set(text); break;
            case 'x': case 'X': case 0x18:
                clipboard_set(text); text[0] = '\0'; cur = 0; replace_next = false; break;
            case 'v': case 'V': case 0x16: {
                // Tempel menggantikan isi terpilih (seperti Cut/Copy biasa).
                if (replace_next) { text[0] = '\0'; cur = 0; replace_next = false; }
                const char* p = clipboard_get();
                for (int i = 0; p[i] && cur < MAX_TEXT - 1; i++)
                    text[cur++] = p[i];
                text[cur] = '\0';
            } break;
            default: break;    // Ctrl+lain = shortcut app, bukan teks
            }
            return;
        }
        if (ascii >= 32) {                       // printable → sisipkan
            if (replace_next) { text[0] = '\0'; cur = 0; replace_next = false; }
            if (cur < MAX_TEXT - 1) { text[cur++] = (char)ascii; text[cur] = '\0'; }
        } else if (scancode == 0x0E) {           // Backspace
            if (replace_next) { text[0] = '\0'; cur = 0; replace_next = false; }
            else if (cur > 0) text[--cur] = '\0';
        } else if (scancode == 0x1C) {           // Enter
            replace_next = false;                // commit memakai isi apa adanya
            if (enter_cb) enter_cb(enter_data);
        }
    }
};

} // namespace ui

#endif // KWIDGET_PRIMITIVES_TEXTBOX_HPP
