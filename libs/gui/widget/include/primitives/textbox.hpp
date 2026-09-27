// libs/widget/include/primitives/textbox.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_PRIMITIVES_TEXTBOX_HPP
#define KWIDGET_PRIMITIVES_TEXTBOX_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "services/clipboard.hpp"

namespace ui {

// ------------------------------------------------------------
// TextBox — input satu baris; fokus keyboard via klik (Phase 7)
// ------------------------------------------------------------
class TextBox : public Widget {
public:
    enum { MAX_TEXT = 256 };
    char text[MAX_TEXT];
    int cur;                    // posisi kursor (indeks karakter)
    bool hover;                 // Phase B: border menegas saat hover
    // Phase C: error = border danger (+ tint background); validasi milik app.
    bool error;
    // Seluruh isi "terpilih": tombol pengubah teks berikutnya MENGGANTI isi
    // alih-alih menambah (semantik ganti-nama Explorer). Hanya penanda visual
    // (isi tetap digambar sebagai blok terpilih), bukan state editor: widget ini
    // tidak punya seleksi/klip seperti TextEdit.
    bool replace_next;
    ui_click_cb enter_cb;
    void* enter_data;

    TextBox(int width) : cur(0), hover(false), error(false), replace_next(false),
                         enter_cb(0), enter_data(0) {
        w = width; h = 24;
        text[0] = '\0';
        cursor_kind = UI_CURSOR_IBEAM;
    }
    void set_text(const char* t) {
        int n = 0; while (t[n] && n < MAX_TEXT - 1) n++;
        for (int i = 0; i < n; i++) text[i] = t[i];
        text[n] = '\0';
        cur = n;
        replace_next = false;   // isi baru dari kode bukan target ganti
        mark_dirty();
    }
    // Tandai seluruh isi terpilih (ganti-nama inline: nama lama langsung bisa
    // ditimpa). Enter tetap memakai isi apa adanya.
    void select_all() {
        int n = 0; while (text[n]) n++;
        replace_next = (n > 0);
        cur = n;
        mark_dirty();
    }
    void drop_pending() { replace_next = false; }
    // Phase C: error state (state + rendering saja; validasi milik aplikasi).
    void set_error(bool on) {
        if (error == on) return;
        error = on;
        mark_dirty();
    }
    virtual bool focusable() override { return enabled; }
    virtual void set_hover(bool on) override { hover = on; mark_dirty(); }
    virtual void draw(Painter& p) override {
        color_t txt = enabled ? p.theme.text : p.theme.text_disabled;
        // Error: tint danger di background + border danger (unfocused).
        // Fokus + error coexist: border tetap focus ring, tint tetap terlihat.
        color_t bg = (enabled && error) ? theme_mix(p.theme.danger, p.theme.surface, 26)
                                        : p.theme.surface;
        p.rect(x, y, w, h, bg);
        // Border 1px: subtle saat normal, menegas saat hover, danger saat
        // error, focus saat fokus (prioritas tertinggi).
        color_t border = !enabled      ? p.theme.border_subtle
                       : has_focus     ? p.theme.focus
                       : error         ? p.theme.danger
                       : hover         ? p.theme.border
                                       : p.theme.border_subtle;
        p.rect(x, y, w, 1, border);
        p.rect(x, y + h - 1, w, 1, border);
        p.rect(x, y, 1, h, border);
        p.rect(x + w - 1, y, 1, h, border);
        if (has_focus && replace_next) {
            int n = 0; while (text[n]) n++;          // blok terpilih = akan diganti
            p.rect(x + 4, y + 4, n * 8, 16, p.theme.surface_elevated);
        }
        p.text(text, x + 4, y + 4, txt);
        if (has_focus && enabled) p.rect(x + 4 + cur * 8, y + 4, 1, 16, p.theme.focus);
    }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        if (!enabled) return;
        mark_dirty();   // teks/kursor/kotak fokus bisa berubah
        // Phase 9: Ctrl+C/X/V = clipboard (salurkan via P1 dasar 'c'/'x'/'v',
        // plus control-code variant 0x03/0x18/0x16 bila driver memetakannya).
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
