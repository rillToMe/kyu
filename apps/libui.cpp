// apps/libui.cpp — Widget Toolkit (Phase 6): Modern C++ di atas libgui (C)
//
// Arsitektur (sesuai keputusan user 2026-08-06):
//   Application (C / Rust / Zig / ...)
//     -> Public GUI C ABI (include/libui.h, opaque handles)
//     -> extern "C" wrappers (bagian bawah file ini)
//     -> Modern C++ Toolkit  (namespace ui)
//     -> libgui renderer (C, apps/libgui.c)
//     -> framebuffer / graphics driver
//
// Batasan toolchain bare-metal:
//   - TIDAK ada libstdc++/libc++ -> operator new/delete di-stub ke
//     sys_alloc/sys_free (userlib.o).
//   - -fno-exceptions -fno-rtti -> __cxa_pure_virtual di-stub.
//   - ELF loader TIDAK menjalankan .init_array -> TIDAK ada global/static
//     C++ object dengan constructor non-trivial. Semua object dibuat via
//     new pada waktu jalan (runtime), aman.
//   - vtables masuk .rodata (PT_LOAD R-X, app.ld) — relokasi selesai di
//     link time (non-PIE, base tetap 0x4000000), tidak butuh runtime reloc.
//   - userlib.h/libgui.h tidak punya extern "C" guard -> dibungkus di sini.

#include <stdint.h>
#include <stddef.h>

extern "C" {
#include "userlib.h"
#include "libgui.h"
}

// libui.h punya guard extern "C" sendiri — aman di-include dari C++.
#include "libui.h"

// Math blend/AA dibagi dengan compositor kernel (kernel/gfx/compositor.c).
// Semua static inline, integer saja — aman di-include dari C++.
#include "aa_math.h"

// Dekoder PNG bersama (apps/png.c, stb_image) — dilink oleh app yang memakai
// Image widget. Di-declare extern "C" karena png.c adalah file C.
extern "C" uint32_t* png_decode(const char* filename, int* out_w, int* out_h);
extern "C" void png_free(uint32_t* buf);

namespace {

// ------------------------------------------------------------
// Runtime shim: C++ memori -> syscalls KyuzenOS
// ------------------------------------------------------------
void  _ui_free(void* p)     { if (p) sys_free(p); }

int _ui_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// Copy string — toolkit OWNS salinannya (caller boleh pakai stack buffer).
char* _ui_strdup(const char* s) {
    int n = _ui_strlen(s) + 1;
    char* d = (char*)sys_alloc(n);
    if (!d) return 0;
    for (int i = 0; i < n; i++) d[i] = s[i];
    return d;
}

// Clipboard — buffer teks global toolkit (Phase 9). Cross-app butuh IPC
// kernel (shared memory) — sengaja di luar scope phase ini.
// ponytail: satu buffer global, bukan per-app/per-window; upgrade bila
// multi-app clipboard dibutuhkan (phase kernel + protokol).
static char* g_clipboard = 0;
void clipboard_set(const char* s) {
    char* n = _ui_strdup(s ? s : "");
    if (!n) return;
    _ui_free(g_clipboard);
    g_clipboard = n;
}
const char* clipboard_get() { return g_clipboard ? g_clipboard : ""; }
void clipboard_clear() { _ui_free(g_clipboard); g_clipboard = 0; }

} // namespace

// C++ operator new/delete (global scope, bukan namespace) -> sys_alloc/sys_free.
void* operator new(unsigned long n)              { return sys_alloc((uint32_t)n); }
void* operator new[](unsigned long n)            { return sys_alloc((uint32_t)n); }
void  operator delete(void* p) noexcept          { if (p) sys_free(p); }
void  operator delete[](void* p) noexcept        { if (p) sys_free(p); }
void  operator delete(void* p, unsigned long) noexcept   { if (p) sys_free(p); }
void  operator delete[](void* p, unsigned long) noexcept { if (p) sys_free(p); }

// Dipanggil kalau vtable class abstrak terpanggil (bug) — jangan kembali.
extern "C" void __cxa_pure_virtual() { for (;;) {} }

namespace ui {

// ------------------------------------------------------------
// Theme — layout field identik ui_theme_t (C ABI)
// ------------------------------------------------------------
struct Theme {
    uint32_t bg, fg, accent, button_bg, button_fg, button_hover;
    Theme() : bg(0x1A1A2E), fg(0xE0E0E0), accent(0xE94560),
              button_bg(0x0F3460), button_fg(0xFFFFFF), button_hover(0x2A4A7E) {}
};

// ------------------------------------------------------------
// Warna: aa_mix / aa_shade / aa_cov dari include/aa_math.h — INTEGER saja,
// dibagi dengan compositor kernel (app juga -mno-sse -msoft-float).
// ------------------------------------------------------------

// ------------------------------------------------------------
// Painter — satu-satunya jembatan widget -> renderer (libgui C)
// ------------------------------------------------------------
class Painter {
public:
    gui_window_t* win;
    const Theme& theme;
    // Scissor rect widget-level (Phase 8) — set_clip/clear_clip dipakai widget.
    bool clip_on;
    int clip_x, clip_y, clip_w, clip_h;
    // Phase 5: render/dirty clip — dipasang Window::render, TIDAK disentuh widget.
    bool rclip_on;
    int rclip_x, rclip_y, rclip_w, rclip_h;
    Painter(gui_window_t* w, const Theme& t)
        : win(w), theme(t), clip_on(false), clip_x(0), clip_y(0),
          clip_w(0), clip_h(0), rclip_on(false), rclip_x(0), rclip_y(0),
          rclip_w(0), rclip_h(0) {}
    void set_render_clip(int x, int y, int w, int h) {
        rclip_on = true; rclip_x = x; rclip_y = y; rclip_w = w; rclip_h = h;
    }
    void set_clip(int x, int y, int w, int h) {
        clip_on = true; clip_x = x; clip_y = y; clip_w = w; clip_h = h;
    }
    void clear_clip() { clip_on = false; }
    // Potong rect ke scissor widget + render clip + bounds window.
    // Return false bila kosong. Semua primitif lewat sini (satu jalur clipping).
    bool clip_rect(int& x, int& y, int& w, int& h) {
        int x1 = x + w, y1 = y + h;
        if (clip_on) {
            if (x < clip_x) x = clip_x;
            if (y < clip_y) y = clip_y;
            int cx = clip_x + clip_w, cy = clip_y + clip_h;
            if (x1 > cx) x1 = cx;
            if (y1 > cy) y1 = cy;
        }
        if (rclip_on) {
            if (x < rclip_x) x = rclip_x;
            if (y < rclip_y) y = rclip_y;
            int rx = rclip_x + rclip_w, ry = rclip_y + rclip_h;
            if (x1 > rx) x1 = rx;
            if (y1 > ry) y1 = ry;
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x1 > (int)win->width)  x1 = (int)win->width;
        if (y1 > (int)win->height) y1 = (int)win->height;
        if (x1 <= x || y1 <= y) return false;
        w = x1 - x; h = y1 - y;
        return true;
    }
    void rect(int x, int y, int w, int h, uint32_t c) {
        if (!clip_rect(x, y, w, h)) return;
        gui_draw_rect(win, x, y, w, h, c | 0xFF000000);
    }
    void text(const char* s, int x, int y, uint32_t c) {
        // Tanpa clip: jalur cepat (libgui menandai damage ter-clip sendiri).
        if (!clip_on && !rclip_on) { gui_draw_text(win, s, x, y, c | 0xFF000000); return; }
        // Ter-clip: gambar per-sel 8x16; hanya sel yang beririsan dengan clip.
        int cy = y;
        for (int i = 0; s[i] && i < 512; i++) {
            if (s[i] == '\n') { cy += 16; continue; }
            int cx = x + i * 8;
            int rx = cx, ry = cy, rw = 8, rh = 16;
            if (clip_rect(rx, ry, rw, rh))
                gui_draw_char(win, s[i], cx, cy, c | 0xFF000000);
        }
    }
    // Blit PNG XRGB8888 (px = iw×ih) diskalakan nearest-neighbor ke rect
    // (x,y,w,h). Menulis win->canvas langsung (libgui tak punya draw-image).
    // Phase 5: clip ke window + scissor + render clip, lalu catat damage rect.
    void image(int x, int y, int w, int h, const uint32_t* px, int iw, int ih) {
        if (w <= 0 || h <= 0 || iw <= 0 || ih <= 0 || !px) return;
        int dx = x, dy = y, dw = w, dh = h;
        if (!clip_rect(dx, dy, dw, dh)) return;
        int q0 = dx - x, py0 = dy - y, q1 = q0 + dw, py1 = py0 + dh;
        int cw = (int)win->width;
        for (int py = py0; py < py1; py++) {
            int sy = py * ih / h;
            for (int q = q0; q < q1; q++) {
                int sx = q * iw / w;
                win->canvas[(y + py) * cw + x + q] = px[sy * iw + sx];
            }
        }
        gui_damage_rect(win, dx, dy, dw, dh);
    }

    // ----- Primitif "modern" (blend / gradient / rounded / shadow) -----
    // Compositor kernel memakai byte alpha canvas sebagai MASK opaque
    // (0 = tembus), bukan faktor blend → blending harus dilakukan di sini:
    // baca pixel canvas, campur, tulis kembali.
    void blend(int px, int py, uint32_t c, uint32_t a) {
        if (a == 0) return;
        int x = px, y = py, w = 1, h = 1;
        if (!clip_rect(x, y, w, h)) return;
        uint32_t* d = &win->canvas[y * (int)win->width + x];
        *d = aa_mix(c, *d, a) | 0xFF000000;
        gui_damage_rect(win, x, y, 1, 1);
    }
    void blend_rect(int x, int y, int w, int h, uint32_t c, uint32_t a) {
        for (int iy = y; iy < y + h; iy++)
            for (int ix = x; ix < x + w; ix++) blend(ix, iy, c, a);
    }
    // Gradient vertikal (lerp integer per baris).
    void vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
        for (int iy = 0; iy < h; iy++)
            rect(x, y + iy, w, 1, aa_mix(bot, top, (uint32_t)(iy * 255 / (h > 1 ? h - 1 : 1))));
    }
    // Coverage 0..255 pixel (px,py) di dalam rounded-rect (aa_math.h:
    // supersample 4x4 integer — pengganti Wu yang butuh float).
    static uint32_t rr_cov(int px, int py, int x, int y, int w, int h, int r) {
        return aa_cov(px, py, x, y, w, h, r, r);
    }
    // Rounded rect + gradient vertikal, sudut anti-alias.
    void rrect_grad(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        for (int iy = y; iy < y + h; iy++) {
            uint32_t c = (top == bot) ? top
                       : aa_mix(bot, top, (uint32_t)((iy - y) * 255 / (h > 1 ? h - 1 : 1)));
            if (iy >= y + r && iy < y + h - r) { rect(x, iy, w, 1, c); continue; }
            rect(x + r, iy, w - 2 * r, 1, c);
            for (int k = 0; k < r; k++) {
                blend(x + k, iy, c, rr_cov(x + k, iy, x, y, w, h, r));
                blend(x + w - 1 - k, iy, c, rr_cov(x + w - 1 - k, iy, x, y, w, h, r));
            }
        }
    }
    void rrect(int x, int y, int w, int h, int r, uint32_t c) {
        rrect_grad(x, y, w, h, r, c, c);
    }
    // Border 1px halus mengikuti sudut bulat (alpha, bukan garis keras).
    void rrect_border(int x, int y, int w, int h, int r, uint32_t c, uint32_t a) {
        if (w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        for (int iy = y; iy < y + h; iy++) {
            if (iy >= y + r && iy < y + h - r) {
                blend(x, iy, c, a); blend(x + w - 1, iy, c, a);
                continue;
            }
            for (int k = 0; k < r; k++) {
                // Tepi arc = pixel dengan coverage partial.
                uint32_t cl = rr_cov(x + k, iy, x, y, w, h, r);
                if (cl && cl < 255) blend(x + k, iy, c, a * cl / 255);
                uint32_t cr = rr_cov(x + w - 1 - k, iy, x, y, w, h, r);
                if (cr && cr < 255) blend(x + w - 1 - k, iy, c, a * cr / 255);
            }
        }
        blend_rect(x + r, y, w - 2 * r, 1, c, a);
        blend_rect(x + r, y + h - 1, w - 2 * r, 1, c, a);
    }
    // Drop shadow: 4 ring alpha menurun, offset 3px ke bawah (blur semu).
    // Gambar SEBELUM isi widget — ring di dalam rect ditimpa widget.
    // ponytail: ring 1px, bukan gaussian blur; cukup untuk kesan kedalaman.
    void shadow(int x, int y, int w, int h) {
        static const uint32_t A[4] = { 52, 38, 24, 12 };
        for (int k = 1; k <= 4; k++) {
            int sx = x - k, sy = y - k + 3, sw = w + 2 * k, sh = h + 2 * k;
            uint32_t a = A[k - 1];
            blend_rect(sx, sy, sw, 1, 0, a);
            blend_rect(sx, sy + sh - 1, sw, 1, 0, a);
            blend_rect(sx, sy + 1, 1, sh - 2, 0, a);
            blend_rect(sx + sw - 1, sy + 1, 1, sh - 2, 0, a);
        }
    }
};

// ------------------------------------------------------------
// Widget — basis pohon. Koordinat window-local konten.
// ------------------------------------------------------------
class Widget {
public:
    int x, y, w, h;
    bool visible;
    bool has_focus;          // diset Window saat fokus keyboard intra-window
    ui_click_cb click_cb;
    void* userdata;
    // Phase 9: Drag & Drop + bentuk kursor per-widget.
    bool draggable;
    char* dnd_payload;
    bool drop_target;
    ui_drop_cb drop_cb;
    void* drop_data;
    int cursor_kind;
    // Phase 8: dirty tracking per-widget (tetap satu bbox di Window).
    // Widget yang benar-benar berubah menandai rect-nya sendiri; Window
    // meng-unions hanya rect itu, bukan seluruh pohon widget.
    bool dirty;
    int dm_x, dm_y, dm_w, dm_h;

    Widget() : x(0), y(0), w(0), h(0), visible(true), has_focus(false),
              click_cb(0), userdata(0), draggable(false), dnd_payload(0),
              drop_target(false), drop_cb(0), drop_data(0),
              cursor_kind(UI_CURSOR_ARROW),
              dirty(false), dm_x(0), dm_y(0), dm_w(0), dm_h(0) {}
    virtual ~Widget() { _ui_free(dnd_payload); }
    virtual void draw(Painter& p) = 0;
    virtual void set_hover(bool on) { (void)on; }
    virtual void set_focus(bool on) { has_focus = on; mark_dirty(); }
    virtual bool focusable() { return false; }   // TextBox → true
    // Phase 8: akumulasi rect kotor (window-local) — over-report BOLEH.
    void mark_area(int ax, int ay, int aw, int ah) {
        if (aw <= 0 || ah <= 0) return;
        if (!dirty) { dirty = true; dm_x = ax; dm_y = ay; dm_w = aw; dm_h = ah; return; }
        int x1 = ax + aw, y1 = ay + ah;
        int dx1 = dm_x + dm_w, dy1 = dm_y + dm_h;
        if (ax < dm_x) dm_x = ax;
        if (ay < dm_y) dm_y = ay;
        if (x1 > dx1) dx1 = x1;
        if (y1 > dy1) dy1 = y1;
        dm_w = dx1 - dm_x; dm_h = dy1 - dm_y;
    }
    void mark_dirty() { mark_area(x, y, w, h); }
    // Ambil + reset damage akumulatif widget.
    bool take_dirty(int& ox, int& oy, int& ow, int& oh) {
        if (!dirty) return false;
        ox = dm_x; oy = dm_y; ow = dm_w; oh = dm_h;
        dirty = false; dm_x = dm_y = dm_w = dm_h = 0;
        return true;
    }
    // Traversal dirty (Layout/Tab/ScrollView override).
    virtual int dirty_child_count() { return 0; }
    virtual Widget* dirty_child(int i) { (void)i; return 0; }
    // Layout: hitung ulang posisi anak sebelum pengumpulan damage (reflow).
    virtual void settle() {}
    // Hit-test: widget paling dalam yang memuat (mx,my), atau 0.
    virtual Widget* pick(int mx, int my) {
        if (!visible) return 0;
        return (mx >= x && mx < x + w && my >= y && my < y + h) ? this : 0;
    }
    // Phase 5: union bounds subtree ke (x0,y0,x1,y1) — untuk damage render luas
    // (mis. tick) tanpa region engine. Default = rect widget sendiri.
    virtual void collect_bounds(int& x0, int& y0, int& x1, int& y1) {
        if (!visible || w <= 0 || h <= 0) return;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x + w > x1) x1 = x + w;
        if (y + h > y1) y1 = y + h;
    }
    virtual void on_click(int mx, int my) {
        (void)mx; (void)my;
        if (click_cb) click_cb(userdata);
    }
    // Drag: dipanggil tiap MOUSE_MOVE selama mouse ditekan di widget ini
    // (grabbed). Return true = perlu redraw. on_release = tombol dilepas.
    virtual bool on_drag(int mx, int my) { (void)mx; (void)my; return false; }
    virtual void on_release() {}
    // Keyboard: hanya dipanggil bila widget ini yang punya fokus. ascii dari
    // P1 (0 = non-printable), scancode dari P3 (Backspace 0x0E, Enter 0x1C).
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {
        (void)ascii; (void)scancode; (void)mods;
    }
    // Scroll roda (Phase 8): dipanggil saat EVENT_SCROLL lewat widget yang
    // sedang di-hover. delta = ±1 notch (+1 = roda ke bawah). Return true = redraw.
    virtual bool on_scroll(int delta) { (void)delta; return false; }
    // Hover sub-elemen (Phase 8): set_hover(bool) tidak membawa koordinat,
    // jadi widget ber-isi (Menu, MenuBar, Toolbar, ListView, Table, TreeView)
    // menimpa ini untuk melacak elemen mana yang di-hover. Return true = redraw.
    virtual bool track_hover(int mx, int my) { (void)mx; (void)my; return false; }
    // Bar menu (MenuBar) butuh perlakuan khusus di Window::run saat popup
    // terbuka (switch/close), beda dari bar lain (Toolbar) yang cukup close.
    virtual bool is_menu_bar() { return false; }
    void set_click(ui_click_cb cb, void* u) { click_cb = cb; userdata = u; }
    // Phase 9: DnD. Widget draggable memulai drag saat klik-tahan; click_cb
    // tidak dipanggil (threshold-drag untuk seret-langsung adalah masa depan).
    void set_draggable(const char* payload) {
        char* n = _ui_strdup(payload ? payload : "");
        if (!n) return;
        _ui_free(dnd_payload);
        dnd_payload = n;
        draggable = true;
    }
    void set_drop_target(ui_drop_cb cb, void* u) {
        drop_target = true; drop_cb = cb; drop_data = u;
    }
    void set_cursor(int kind) { cursor_kind = kind; }
};

// ------------------------------------------------------------
// Label — teks statis, lebar otomatis = panjang * 8px
// ------------------------------------------------------------
class Label : public Widget {
public:
    char* text;
    Label(const char* t) : text(_ui_strdup(t)) { w = _ui_strlen(text) * 8; h = 16; }
    virtual ~Label() { _ui_free(text); }
    void set_text(const char* t) {
        char* n = _ui_strdup(t);
        if (!n) return;
        mark_dirty();               // bounds lama (w bisa menyusut)
        _ui_free(text);
        text = n;
        w = _ui_strlen(text) * 8;
        mark_dirty();               // bounds baru
    }
    virtual void draw(Painter& p) override { p.text(text, x, y, p.theme.fg); }
};

// ------------------------------------------------------------
// Button — rounded rect + gradient halus, state normal/hover/pressed
// ------------------------------------------------------------
class Button : public Widget {
public:
    char* text;
    bool hover;
    bool pressed;
    Button(const char* t) : text(_ui_strdup(t)), hover(false), pressed(false) {
        // Grid 8px: padding 12px kiri/kanan, tinggi 28 (teks 16 + 6/6).
        w = _ui_strlen(text) * 8 + 24; h = 28;
        cursor_kind = UI_CURSOR_HAND;
    }
    virtual ~Button() { _ui_free(text); }
    virtual void draw(Painter& p) override {
        uint32_t base = pressed ? aa_shade(p.theme.button_bg, -18)
                      : hover   ? p.theme.button_hover
                                : p.theme.button_bg;
        // Gradient ~10% (terang di atas; dibalik saat pressed) + sudut 6px.
        p.rrect_grad(x, y, w, h, 6,
                     aa_shade(base, pressed ? -5 : 10),
                     aa_shade(base, pressed ?  5 : -10));
        p.rrect_border(x, y, w, h, 6, 0x000000, pressed ? 90 : 55);
        // Inset shadow tipis di tepi atas saat ditekan.
        if (pressed) p.blend_rect(x + 6, y + 1, w - 12, 1, 0x000000, 60);
        p.text(text, x + (w - _ui_strlen(text) * 8) / 2,
               y + (h - 16) / 2 + (pressed ? 1 : 0), p.theme.button_fg);
    }
    virtual void set_hover(bool on) override { hover = on; if (!on) pressed = false; mark_dirty(); }
    virtual void on_click(int mx, int my) override {
        pressed = true;             // render() dipanggil Window setelah ini
        mark_dirty();
        Widget::on_click(mx, my);
    }
    virtual void on_release() override { pressed = false; mark_dirty(); }
};

// ------------------------------------------------------------
// TextBox — input satu baris; fokus keyboard via klik (Phase 7)
// ------------------------------------------------------------
class TextBox : public Widget {
public:
    enum { MAX_TEXT = 256 };
    char text[MAX_TEXT];
    int cur;                    // posisi kursor (indeks karakter)
    ui_click_cb enter_cb;
    void* enter_data;

    TextBox(int width) : cur(0), enter_cb(0), enter_data(0) {
        w = width; h = 24;
        text[0] = '\0';
        cursor_kind = UI_CURSOR_IBEAM;
    }
    void set_text(const char* t) {
        int n = 0; while (t[n] && n < MAX_TEXT - 1) n++;
        for (int i = 0; i < n; i++) text[i] = t[i];
        text[n] = '\0';
        cur = n;
        mark_dirty();
    }
    virtual bool focusable() override { return true; }
    virtual void draw(Painter& p) override {
        p.rect(x, y, w, h, p.theme.button_bg);
        uint32_t border = has_focus ? p.theme.accent : p.theme.fg;
        p.rect(x, y, w, 1, border);
        p.rect(x, y + h - 1, w, 1, border);
        p.rect(x, y, 1, h, border);
        p.rect(x + w - 1, y, 1, h, border);
        p.text(text, x + 4, y + 4, p.theme.fg);
        if (has_focus) p.rect(x + 4 + cur * 8, y + 4, 1, 16, p.theme.accent);
    }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        mark_dirty();   // teks/kursor/kotak fokus bisa berubah
        // Phase 9: Ctrl+C/X/V = clipboard (salurkan via P1 dasar 'c'/'x'/'v',
        // plus control-code variant 0x03/0x18/0x16 bila driver memetakannya).
        if (mods & KEY_MOD_CTRL) {
            switch (ascii) {
            case 'c': case 'C': case 0x03: clipboard_set(text); break;
            case 'x': case 'X': case 0x18:
                clipboard_set(text); text[0] = '\0'; cur = 0; break;
            case 'v': case 'V': case 0x16: {
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
            if (cur < MAX_TEXT - 1) { text[cur++] = (char)ascii; text[cur] = '\0'; }
        } else if (scancode == 0x0E) {           // Backspace
            if (cur > 0) text[--cur] = '\0';
        } else if (scancode == 0x1C && enter_cb) {   // Enter
            enter_cb(enter_data);
        }
    }
};

// ------------------------------------------------------------
// TextEdit — editor multi-baris (Phase 10). Buffer teks polos 8K,
// kursor + scroll roda/otomatis, opsional readonly (terminal output).
// Font 8x16: 8px/kolom, 16px/baris.
// ------------------------------------------------------------
class TextEdit : public Widget {
public:
    enum { MAX_TEXT = 8192, LINE_H = 16, CHAR_W = 8 };
    char text[MAX_TEXT];
    int len;
    int cur;
    int scroll_top;    // baris pertama yang tampak
    bool readonly;
    ui_click_cb enter_cb;   // terminal: Enter diserahkan ke app (tanpa sisip '\n')
    void* enter_data;

    // Terminal: prefix prompt berwarna (gaya shell Linux). Baris yang DIAWALI
    // `ps1` digambar dengan `ps1_len` karakter pertama memakai `ps1_color`,
    // sisanya theme.fg. ps1_len == 0 = fitur mati (notepad & co).
    char ps1[32];
    int ps1_len;
    uint32_t ps1_color;
    TextEdit(int width, int height) : len(0), cur(0), scroll_top(0),
                                      readonly(false), enter_cb(0), enter_data(0),
                                      ps1_len(0), ps1_color(0xFF7CC7FF) {
        w = width; h = height;
        text[0] = '\0';
        cursor_kind = UI_CURSOR_IBEAM;
    }
    void set_enter(ui_click_cb cb, void* u) { enter_cb = cb; enter_data = u; }
    void set_prompt_style(const char* prefix, uint32_t color) {
        ps1_len = 0;
        if (!prefix) { mark_dirty(); return; }
        for (; ps1_len < (int)sizeof(ps1) - 1 && prefix[ps1_len]; ps1_len++)
            ps1[ps1_len] = prefix[ps1_len];
        ps1[ps1_len] = '\0';
        ps1_color = color;
        mark_dirty();
    }
    // Baris idx (offset mulai baris) diawali prefix prompt berwarna?
    bool line_has_ps1(int idx) const {
        if (ps1_len == 0 || len - idx < ps1_len) return false;
        for (int i = 0; i < ps1_len; i++) if (text[idx + i] != ps1[i]) return false;
        return true;
    }
    // Readonly (output terminal) tak boleh mencuri fokus dari input.
    virtual bool focusable() override { return !readonly; }

    // --- utilitas baris ---
    int line_at(int idx) const {
        int ln = 0;
        for (int i = 0; i < idx && i < len; i++) if (text[i] == '\n') ln++;
        return ln;
    }
    int line_start(int idx) const {
        int i = idx;
        while (i > 0 && text[i - 1] != '\n') i--;
        return i;
    }
    int total_lines() const {
        int ln = 1;
        for (int i = 0; i < len; i++) if (text[i] == '\n') ln++;
        return ln;
    }
    int vis_lines() const { int v = h / LINE_H; return v < 1 ? 1 : v; }

    // --- edit buffer ---
    void insert_at(int idx, char c) {
        if (len >= MAX_TEXT - 1) return;
        for (int i = len; i > idx; i--) text[i] = text[i - 1];
        text[idx] = c;
        len++;
        text[len] = '\0';
    }
    void delete_at(int idx) {
        if (idx < 0 || idx >= len) return;
        for (int i = idx; i < len; i++) text[i] = text[i + 1];
        len--;
        text[len] = '\0';
    }
    void append(const char* s) {
        mark_dirty();
        for (int i = 0; s[i] && len < MAX_TEXT - 1; i++) text[len++] = s[i];
        text[len] = '\0';
        cur = len;
        scroll_top = total_lines() - vis_lines();   // ikut ujung (terminal)
        clamp_scroll();
        mark_dirty();
    }
    void clear() { mark_dirty(); len = 0; cur = 0; scroll_top = 0; text[0] = '\0'; }

    // --- scroll ---
    void clamp_scroll() {
        int mt = total_lines() - vis_lines();
        if (mt < 0) mt = 0;
        if (scroll_top > mt) scroll_top = mt;
        if (scroll_top < 0) scroll_top = 0;
    }
    void ensure_cursor_visible() {
        int line = line_at(cur);
        if (line < scroll_top) scroll_top = line;
        else if (line >= scroll_top + vis_lines())
            scroll_top = line - vis_lines() + 1;
        clamp_scroll();
    }

    virtual void on_click(int mx, int my) override {
        if (enter_cb) {              // terminal: kursor terkunci di baris perintah
            cur = len;
            ensure_cursor_visible();
            mark_dirty();
            return;
        }
        int col = (mx - x - 4) / CHAR_W; if (col < 0) col = 0;
        int row = (my - y) / LINE_H + scroll_top;
        int idx = 0, ln = 0;
        while (ln < row && idx < len) {
            if (text[idx] == '\n') ln++;
            idx++;
        }
        int c = 0;
        while (idx < len && text[idx] != '\n' && c < col) { idx++; c++; }
        cur = idx;
        ensure_cursor_visible();
        mark_dirty();
    }
    virtual bool on_scroll(int delta) override {
        int old = scroll_top;
        scroll_top += delta;        // +1 roda bawah = lihat output lebih bawah
        clamp_scroll();
        if (scroll_top != old) mark_dirty();
        return scroll_top != old;
    }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        mark_dirty();
        if (mods & KEY_MOD_CTRL) {
            if (ascii == 'a' || ascii == 'A') { cur = line_start(cur); return; }  // Home baris
            if (ascii == 'e' || ascii == 'E') {                                  // End baris
                int i = cur;
                while (i < len && text[i] != '\n') i++;
                cur = i; return;
            }
            return;
        }
        uint32_t sc = scancode & 0xFF;
        // Terminal (enter_cb): Enter = submit ke app, bukan sisip '\n'.
        if (enter_cb && (ascii == '\n' || ascii == '\r' || sc == 0x1C)) {
            enter_cb(enter_data);
            return;
        }
        int lower = enter_cb ? line_start(len) : 0;   // edit hanya baris perintah
        if (ascii >= 32) {                    // printable → sisip
            if (!readonly) { insert_at(cur, (char)ascii); cur++; }
        } else if (sc == 0x0E) {              // Backspace
            if (!readonly && cur > lower) { delete_at(cur - 1); cur--; }
        } else if (ascii == '\n' || ascii == '\r' || sc == 0x1C) {  // Enter
            if (!readonly) { insert_at(cur, '\n'); cur++; }
        } else if (sc == 0x4B) { if (cur > lower) cur--; }           // Left
        else if (sc == 0x4D) { if (cur < len) cur++; }               // Right
        else if (sc == 0x48 && !enter_cb) {                          // Up
            int col = cur - line_start(cur);
            int ls = line_start(cur);
            if (ls > 0) {
                int ps = line_start(ls - 1), pe = ps;
                while (pe < len && text[pe] != '\n') pe++;
                cur = ps + col; if (cur > pe) cur = pe;
            }
        } else if (sc == 0x50 && !enter_cb) {                         // Down
            int col = cur - line_start(cur);
            int le = cur;
            while (le < len && text[le] != '\n') le++;
            if (le < len) {
                int ns = le + 1, ne = ns;
                while (ne < len && text[ne] != '\n') ne++;
                cur = ns + col; if (cur > ne) cur = ne;
            }
        } else if (sc == 0x47) { cur = enter_cb ? lower : line_start(cur); }  // Home
        else if (sc == 0x4F) {                                        // End
            int i = cur; while (i < len && text[i] != '\n') i++; cur = i;
        } else if (sc == 0x49) {                                     // PgUp
            scroll_top -= vis_lines(); clamp_scroll();
            if (enter_cb) return;          // terminal: biarkan viewport di atas
        } else if (sc == 0x51) {                                     // PgDn
            scroll_top += vis_lines(); clamp_scroll();
            if (enter_cb) return;
        } else return;                    // tombol lain: tanpa redraw
        ensure_cursor_visible();
    }
    virtual void draw(Painter& p) override {
        clamp_scroll();
        p.rect(x, y, w, h, p.theme.button_bg);
        p.set_clip(x, y, w, h);
        int colw = (w - 8) / CHAR_W;    // kolom yang muat (margin 4px)
        int idx = 0, ln = 0;
        while (ln < scroll_top && idx < len) {
            if (text[idx] == '\n') ln++;
            idx++;
        }
        int vy = y;
        while (vy < y + h && idx < len) {
            int cx = x + 4;
            bool ps1_here = line_has_ps1(idx);   // prompt di baris ini → prefix berwarna
            for (int c = 0; c < colw && idx < len && text[idx] != '\n'; c++, idx++) {
                char t[2] = { text[idx], '\0' };
                uint32_t col = (ps1_here && c < ps1_len) ? ps1_color : p.theme.fg;
                p.text(t, cx, vy + 1, col);
                cx += CHAR_W;
            }
            if (idx < len && text[idx] == '\n') idx++;
            vy += LINE_H;
        }
        // caret — kolom/baris dari `cur`, digambar sekali
        if (has_focus) {
            int crow = line_at(cur) - scroll_top;
            if (crow * LINE_H < h) {
                int cx = x + 4 + (cur - line_start(cur)) * CHAR_W;
                if (cx >= x + w) cx = x + w - 1;
                p.rect(cx, y + crow * LINE_H, 1, LINE_H, p.theme.accent);
            }
        }
        p.clear_clip();
    }
};

// ------------------------------------------------------------
// CheckBox — kotak centang + label; klik toggle (Phase 7)
// ------------------------------------------------------------
class CheckBox : public Widget {
public:
    char* label;
    bool checked;
    ui_click_cb toggle_cb;
    void* toggle_data;

    CheckBox(const char* t) : label(_ui_strdup(t)), checked(false),
                              toggle_cb(0), toggle_data(0) {
        w = _ui_strlen(label) * 8 + 20; h = 20;
    }
    virtual ~CheckBox() { _ui_free(label); }
    void set_checked(bool c) { checked = c; mark_dirty(); }
    virtual void on_click(int mx, int my) override {
        (void)mx; (void)my;
        checked = !checked;
        mark_dirty();
        if (toggle_cb) toggle_cb(toggle_data);
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y, 12, 12, p.theme.button_bg);
        p.rect(x, y, 12, 1, p.theme.fg);
        p.rect(x, y + 11, 12, 1, p.theme.fg);
        p.rect(x, y, 1, 12, p.theme.fg);
        p.rect(x + 11, y, 1, 12, p.theme.fg);
        if (checked) {                            // centang diagonal accent
            for (int i = 0; i < 4; i++) p.rect(x + 2 + i, y + 6 + i, 1, 1, p.theme.accent);
            for (int i = 0; i < 6; i++) p.rect(x + 6 + i, y + 9 - i, 1, 1, p.theme.accent);
        }
        p.text(label, x + 20, y + 2, p.theme.fg);
    }
};

// ------------------------------------------------------------
// Slider — track + handle yang bisa diseret (Phase 7)
// ------------------------------------------------------------
class Slider : public Widget {
public:
    int min, max, val;
    bool dragging;
    ui_click_cb change_cb;
    void* change_data;

    Slider(int mn, int mx) : min(mn), max(mx), val(mn), dragging(false),
                             change_cb(0), change_data(0) {
        w = 160; h = 20;
        if (max <= min) max = min + 1;
    }
    void set_value(int v) {
        if (v < min) v = min;
        if (v > max) v = max;
        val = v;
        mark_dirty();
    }
    void clamp_to(int mx) {
        int span = max - min;
        int nw = w - 8;
        set_value(min + (mx - x) * span / nw);   // mx - x = posisi dalam widget
    }
    virtual bool on_drag(int mx, int my) override {
        (void)my;
        if (!dragging) return false;
        int old = val;
        clamp_to(mx);
        if (val != old && change_cb) change_cb(change_data);
        return true;
    }
    virtual void on_release() override { dragging = false; }
    virtual void on_click(int mx, int my) override {
        (void)my;
        dragging = true;
        int old = val;
        clamp_to(mx);
        if (val != old && change_cb) change_cb(change_data);
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y + h / 2 - 2, w, 4, p.theme.button_bg);
        int span = max - min;
        int hx = span ? (val - min) * (w - 8) / span : 0;
        p.rect(x + hx, y, 8, h, p.theme.accent);
    }
};

// ------------------------------------------------------------
// ProgressBar — fill horizontal read-only, 0..100 (Phase 7)
// ------------------------------------------------------------
class ProgressBar : public Widget {
public:
    int val;
    ProgressBar(int width) : val(0) { w = width; h = 16; }
    void set_value(int v) {
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        val = v;
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y, w, h, p.theme.button_bg);
        int fw = val * w / 100;
        if (fw > 0) p.rect(x, y, fw, h, p.theme.accent);
    }
};

// ------------------------------------------------------------
// Image — PNG dari KyuzenFS, nearest-neighbor ke rect (Phase 7)
// ------------------------------------------------------------
class Image : public Widget {
public:
    uint32_t* px;
    int iw, ih;
    Image(const char* filename, int dw, int dh) : px(0), iw(0), ih(0) {
        w = dw; h = dh;
        px = png_decode(filename, &iw, &ih);
    }
    virtual ~Image() { png_free(px); }
    // Phase 10: zoom viewer — target display size dihitung ulang dari ukuran
    // natural PNG (persen 10..400). `w`/`h` jadi area target yang digambar.
    void set_scale(int percent) {
        if (percent < 10) percent = 10;
        if (percent > 400) percent = 400;
        mark_dirty();                     // bounds lama (bisa mengecil)
        if (iw > 0) { w = iw * percent / 100; h = ih * percent / 100; }
        mark_dirty();                     // bounds baru
    }
    // Phase 10: ganti file PNG (viewer galeri) — muat ulang, reset zoom 100%.
    void set_file(const char* filename) {
        mark_dirty();
        png_free(px);
        px = png_decode(filename, &iw, &ih);
        if (iw > 0) { w = iw; h = ih; }   // natural size; ScrollView menyesuaikan
        mark_dirty();
    }
    virtual void draw(Painter& p) override {
        if (!px || iw <= 0 || ih <= 0) { p.rect(x, y, w, h, p.theme.button_bg); return; }
        p.image(x, y, w, h, px, iw, ih);
    }
};

// ------------------------------------------------------------
// Layout — kontainer widget; hit-test anak topmost-first
// ------------------------------------------------------------
class Layout : public Widget {
public:
    enum { MAX_CHILDREN = 16 };
    Widget* children[MAX_CHILDREN];
    int count;

    Layout() : count(0) { for (int i = 0; i < MAX_CHILDREN; i++) children[i] = 0; }
    virtual ~Layout() { for (int i = 0; i < count; i++) delete children[i]; }

    void add(Widget* c) { if (count < MAX_CHILDREN) children[count++] = c; }
    virtual void arrange() = 0;
    // Phase 8: pindahkan anak; tandai posisi lama+baru bila berubah (reflow).
    void place(Widget* c, int nx, int ny) {
        if (c->x != nx || c->y != ny) {
            c->mark_area(c->x, c->y, c->w, c->h);
            c->x = nx; c->y = ny;
            c->mark_area(c->x, c->y, c->w, c->h);
        }
    }
    virtual int dirty_child_count() override { return count; }
    virtual Widget* dirty_child(int i) override { return children[i]; }
    virtual void settle() override { arrange(); }

    virtual void draw(Painter& p) override {
        arrange();
        for (int i = 0; i < count; i++) children[i]->draw(p);
    }
    virtual Widget* pick(int mx, int my) override {
        if (!visible) return 0;
        for (int i = count - 1; i >= 0; i--) {
            Widget* r = children[i]->pick(mx, my);
            if (r) return r;
        }
        return 0;
    }
    virtual void collect_bounds(int& x0, int& y0, int& x1, int& y1) override {
        arrange();   // posisi anak dihitung di sini
        for (int i = 0; i < count; i++) children[i]->collect_bounds(x0, y0, x1, y1);
    }
};

// ------------------------------------------------------------
// VBox — susun anak vertikal berurutan, rata kiri
// ------------------------------------------------------------
class VBox : public Layout {
public:
    int spacing;
    VBox(int s) : spacing(s) { w = 0; h = 0; }
    virtual void arrange() override {
        int cy = y;
        for (int i = 0; i < count; i++) {
            place(children[i], x, cy);
            cy += children[i]->h + spacing;
        }
        h = cy - y;   // ukuran diri = isi; dipakai ScrollView untuk hitung scroll
    }
};

// ------------------------------------------------------------
// HBox — susun anak horizontal (grid tombol kalkulator, Phase 10)
// ------------------------------------------------------------
class HBox : public Layout {
public:
    int spacing;
    HBox(int s) : spacing(s) { w = 0; h = 0; }
    virtual void arrange() override {
        int cx = x;
        int mh = 0;
        for (int i = 0; i < count; i++) {
            place(children[i], cx, y);
            cx += children[i]->w + spacing;
            if (children[i]->h > mh) mh = children[i]->h;
        }
        w = cx - x;
        h = mh;   // VBox luar memakai h ini untuk stack
    }
};

// ------------------------------------------------------------
// Scrollable — basis widget yang bisa di-scroll roda + scrollbar.
// ScrollView/ListView/Table/TreeView menimpa on_content_click dan
// memotong draw()-nya ke area konten. Ponytail: scrollbar thumb
// proporsional sederhana (bukan hitung drag-ratio penuh).
// ------------------------------------------------------------
class Window;   // fwd: Menu/MenuBar pegang Window* untuk popup

class Scrollable : public Widget {
public:
    enum { BAR_W = 6, ROW_H = 20 };
    int scroll, scroll_max;
    bool bar_drag;
    int bar_grab_y, bar_grab_scroll;

    Scrollable() : scroll(0), scroll_max(0), bar_drag(false),
                   bar_grab_y(0), bar_grab_scroll(0) {}

    void set_scroll_view(int content_h, int view_h) {
        scroll_max = content_h - view_h;
        if (scroll_max < 0) scroll_max = 0;
        if (scroll > scroll_max) scroll = scroll_max;
    }
    void set_scroll_max(int content_h) { set_scroll_view(content_h, h - BAR_W); }
    bool bar_hit(int mx) const { return mx >= x + w - BAR_W && mx < x + w; }
    // Lebar konten = tanpa scrollbar bila bar tampil, penuh bila tidak.
    int content_w() const { return scroll_max > 0 ? w - BAR_W : w; }

    virtual bool on_scroll(int delta) override {
        int old = scroll;
        scroll += delta * ROW_H;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
        if (scroll != old) mark_dirty();
        return scroll != old;
    }
    virtual void on_click(int mx, int my) override {
        if (bar_hit(mx)) {
            bar_drag = true;
            bar_grab_y = my;
            bar_grab_scroll = scroll;
            int th = bar_thumb_h();
            int range = h - th;
            if (range > 0) scroll = (my - y - th / 2) * scroll_max / range;
            if (scroll < 0) scroll = 0;
            if (scroll > scroll_max) scroll = scroll_max;
            mark_dirty();
        } else {
            on_content_click(mx, my);
        }
    }
    virtual bool on_drag(int mx, int my) override {
        (void)mx;
        if (!bar_drag) return false;
        int th = bar_thumb_h();
        int range = h - th;
        if (range <= 0) return true;
        scroll = bar_grab_scroll + (my - bar_grab_y) * scroll_max / range;
        if (scroll < 0) scroll = 0;
        if (scroll > scroll_max) scroll = scroll_max;
        mark_dirty();
        return true;
    }
    virtual void on_release() override { bar_drag = false; }
    // Hook klik area konten (subclass). Default fire click_cb.
    virtual void on_content_click(int mx, int my) {
        (void)mx; (void)my;
        if (click_cb) click_cb(userdata);
    }
    int bar_thumb_h() const {
        int content_h = scroll_max + (h - BAR_W);
        if (content_h <= 0) return h;
        int th = (h - BAR_W) * (h - BAR_W) / content_h;
        if (th < 8) th = 8;
        return th;
    }
    void draw_bar(Painter& p) {
        if (scroll_max <= 0) return;
        int bx = x + w - BAR_W;
        p.rect(bx, y, BAR_W, h, p.theme.button_bg);
        int th = bar_thumb_h();
        int range = h - th;
        int ty = range > 0 ? y + scroll * range / scroll_max : y;
        p.rect(bx, ty, BAR_W, th, p.theme.accent);
    }
};

// ------------------------------------------------------------
// ScrollView — wadah scrollable generik; konten = satu widget anak.
// ------------------------------------------------------------
class ScrollView : public Scrollable {
public:
    Widget* child;
    ScrollView(int width, int height) : child(0) {
        w = width; h = height;
        set_scroll_max(0);
    }
    virtual ~ScrollView() { if (child) delete child; }
    void set_child(Widget* c) {
        child = c;
        set_scroll_max(c ? c->h : 0);
        if (c) mark_dirty();
    }
    virtual int dirty_child_count() override { return child ? 1 : 0; }
    virtual Widget* dirty_child(int i) override { (void)i; return child; }
    // Konten (mis. Image zoom) bisa berubah ukuran tanpa event → scrollbar
    // muncul/hilang. Recompute sebelum panen damage agar strip ikut ter-render.
    virtual void settle() override {
        if (!child) return;
        int old = scroll_max;
        set_scroll_max(child->h);
        if (scroll_max != old) mark_dirty();
    }
    virtual void on_content_click(int mx, int my) override {
        if (!child) { if (click_cb) click_cb(userdata); return; }
        child->x = x; child->y = y - scroll;
        Widget* c = child->pick(mx, my);
        if (c) c->on_click(mx, my);
    }
    virtual void draw(Painter& p) override {
        if (!child) { p.rect(x, y, w, h, p.theme.button_bg); draw_bar(p); return; }
        child->x = x; child->y = y - scroll;
        // draw pertama hanya untuk arrange (VBox menghitung h-nya di sini);
        // keduanya ter-clip viewport agar isi yang lebih panjang dari view
        // tidak bocor keluar. Lalu hitung ulang scroll_max (bar mungkin
        // muncul → konten menyempit) dan gambar ulang dengan lebar benar.
        p.set_clip(x, y, w, h);
        child->draw(p);
        set_scroll_max(child->h);
        p.set_clip(x, y, content_w(), h);
        child->draw(p);
        p.clear_clip();
        draw_bar(p);
    }
};

// ------------------------------------------------------------
// ListView — daftar item vertikal, row 20px, pilih + scroll.
// ------------------------------------------------------------
class ListView : public Scrollable {
public:
    enum { MAX_ITEMS = 32 };
    char* items[MAX_ITEMS];
    int n;
    int selected, hover_row;
    ui_click_cb change_cb;
    void* change_data;

    ListView(int width, int height) : n(0), selected(-1), hover_row(-1),
                                      change_cb(0), change_data(0) {
        w = width; h = height;
        for (int i = 0; i < MAX_ITEMS; i++) items[i] = 0;
        set_scroll_max(0);
    }
    virtual ~ListView() { for (int i = 0; i < n; i++) _ui_free(items[i]); }
    void add_item(const char* label) {
        if (n >= MAX_ITEMS) return;
        items[n++] = _ui_strdup(label);
        set_scroll_max(n * ROW_H);
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = -1;
        if (my >= y && my < y + h) {
            r = (scroll + my - y) / ROW_H;
            if (r < 0 || r >= n) r = -1;
        }
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        (void)mx;
        int r = (scroll + my - y) / ROW_H;
        if (r >= 0 && r < n) {
            selected = r;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    virtual void draw(Painter& p) override {
        int cw = content_w();
        p.set_clip(x, y, cw, h);
        for (int i = 0; i < n; i++) {
            int ry = y + i * ROW_H - scroll;
            if (ry + ROW_H <= y || ry >= y + h) continue;
            if (i == selected) p.rect(x, ry, cw, ROW_H, p.theme.button_bg);
            else if (i == hover_row) p.rect(x, ry, cw, ROW_H, p.theme.button_hover);
            p.text(items[i], x + 4, ry + 2, p.theme.fg);
        }
        p.clear_clip();
        draw_bar(p);
    }
};

// ------------------------------------------------------------
// Table — header tetap 24px + baris 20px yang bisa di-scroll.
// Setiap sel teks dipotong ke kolomnya (per-sel clip).
// ------------------------------------------------------------
class Table : public Scrollable {
public:
    enum { HEADER_H = 24, MAX_COLS = 8, MAX_ROWS = 64 };
    char* col[MAX_COLS];
    int col_w[MAX_COLS];
    int ncols;
    char* cells[MAX_ROWS][MAX_COLS];
    int nrows;
    int selected, hover_row;
    ui_click_cb change_cb;
    void* change_data;

    Table(int width, int height) : ncols(0), nrows(0), selected(-1),
                                   hover_row(-1), change_cb(0), change_data(0) {
        w = width; h = height;
        for (int c = 0; c < MAX_COLS; c++) col[c] = 0;
        for (int r = 0; r < MAX_ROWS; r++)
            for (int c = 0; c < MAX_COLS; c++) cells[r][c] = 0;
        set_scroll_view(0, h - HEADER_H - BAR_W);
    }
    virtual ~Table() {
        for (int c = 0; c < ncols; c++) _ui_free(col[c]);
        for (int r = 0; r < nrows; r++)
            for (int c = 0; c < ncols; c++) _ui_free(cells[r][c]);
    }
    void add_column(const char* title, int width) {
        if (ncols >= MAX_COLS) return;
        col[ncols] = _ui_strdup(title);
        col_w[ncols] = width;
        ncols++;
        mark_dirty();
    }
    void add_row(const char* const* vals, int n) {
        if (nrows >= MAX_ROWS || n > MAX_COLS) return;
        for (int c = 0; c < n; c++) cells[nrows][c] = _ui_strdup(vals[c]);
        for (int c = n; c < ncols; c++) cells[nrows][c] = 0;
        nrows++;
        set_scroll_view(nrows * ROW_H, h - HEADER_H - BAR_W);
        mark_dirty();
    }
    void clear() {
        for (int r = 0; r < nrows; r++)
            for (int c = 0; c < ncols; c++) { _ui_free(cells[r][c]); cells[r][c] = 0; }
        nrows = 0;
        set_scroll_view(0, h - HEADER_H - BAR_W);
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = -1;
        if (my >= y + HEADER_H && my < y + h) {
            r = (scroll + my - (y + HEADER_H)) / ROW_H;
            if (r < 0 || r >= nrows) r = -1;
        }
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        (void)mx;
        int r = (scroll + my - (y + HEADER_H)) / ROW_H;
        if (r >= 0 && r < nrows) {
            selected = r;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    virtual void draw(Painter& p) override {
        int cw = content_w();
        // header tetap
        p.rect(x, y, cw, HEADER_H, p.theme.button_bg);
        int cx = x + 2;
        for (int c = 0; c < ncols; c++) {
            p.text(col[c], cx, y + 4, p.theme.button_fg);
            cx += col_w[c];
        }
        p.rect(x, y + HEADER_H - 1, cw, 1, p.theme.fg);
        // baris (scroll), setiap sel dipotong ke kolomnya
        p.set_clip(x, y + HEADER_H, cw, h - HEADER_H);
        for (int r = 0; r < nrows; r++) {
            int ry = y + HEADER_H + r * ROW_H - scroll;
            if (ry + ROW_H <= y + HEADER_H || ry >= y + h) continue;
            if (r == selected) p.rect(x, ry, cw, ROW_H, p.theme.button_bg);
            else if (r == hover_row) p.rect(x, ry, cw, ROW_H, p.theme.button_hover);
            int cxx = x + 2;
            for (int c = 0; c < ncols; c++) {
                p.set_clip(cxx, y + HEADER_H, col_w[c] - 2, h - HEADER_H);
                if (cells[r][c]) p.text(cells[r][c], cxx, ry + 2, p.theme.fg);
                p.set_clip(x, y + HEADER_H, cw, h - HEADER_H);
                cxx += col_w[c];
            }
        }
        p.clear_clip();
        draw_bar(p);
    }
};

// ------------------------------------------------------------
// TreeView — node ber-indent depth*12, marker '+'/'-' untuk
// expand/collapse (font 8x16 tanpa segitiga), pilih node.
// ------------------------------------------------------------
class TreeView : public Scrollable {
public:
    struct Node { char* label; int depth; bool expanded; };
    enum { MAX_NODES = 32 };
    Node nodes[MAX_NODES];
    int n;
    int selected, hover_row;
    ui_click_cb change_cb;
    void* change_data;

    TreeView(int width, int height) : n(0), selected(-1), hover_row(-1),
                                      change_cb(0), change_data(0) {
        w = width; h = height;
        set_scroll_max(0);
    }
    virtual ~TreeView() { for (int i = 0; i < n; i++) _ui_free(nodes[i].label); }
    void add_node(const char* label, int depth, int expanded) {
        if (n >= MAX_NODES) return;
        nodes[n].label = _ui_strdup(label);
        nodes[n].depth = depth;
        nodes[n].expanded = expanded;
        n++;
        recompute_scroll();
        mark_dirty();
    }
    void set_change(ui_click_cb cb, void* u) { change_cb = cb; change_data = u; }
    virtual void set_hover(bool on) override { if (!on && hover_row >= 0) { hover_row = -1; mark_dirty(); } }

    // Node i terlihat bila ancestor terdekatnya (node j<i depth lebih kecil)
    // sedang expanded. Flat pre-order.
    bool visible_node(int i) const {
        for (int j = i - 1; j >= 0; j--)
            if (nodes[j].depth < nodes[i].depth) return nodes[j].expanded;
        return true;
    }
    bool has_children(int i) const {
        for (int j = i + 1; j < n; j++) {
            if (nodes[j].depth <= nodes[i].depth) return false;
            if (nodes[j].depth == nodes[i].depth + 1) return true;
        }
        return false;
    }
    void recompute_scroll() {
        int vis = 0;
        for (int i = 0; i < n; i++) if (visible_node(i)) vis++;
        set_scroll_max(vis * ROW_H);
    }
    // Map vis_row (baris tampil ke-N) -> index node, atau -1.
    int node_at_vis(int vis_row) const {
        int v = 0;
        for (int i = 0; i < n; i++) {
            if (!visible_node(i)) continue;
            if (v == vis_row) return i;
            v++;
        }
        return -1;
    }
    virtual bool track_hover(int mx, int my) override {
        (void)mx;
        int r = -1;
        if (my >= y && my < y + h)
            r = node_at_vis((scroll + my - y) / ROW_H);
        if (r == hover_row) return false;
        hover_row = r;
        mark_dirty();
        return true;
    }
    virtual void on_content_click(int mx, int my) override {
        int idx = node_at_vis((scroll + my - y) / ROW_H);
        if (idx < 0) return;
        int ind = x + nodes[idx].depth * 12;
        if (mx >= ind && mx < ind + 12) {
            nodes[idx].expanded = !nodes[idx].expanded;
            recompute_scroll();
            mark_dirty();
        } else if (mx >= ind + 12) {
            selected = idx;
            mark_dirty();
            if (change_cb) change_cb(change_data);
        }
    }
    virtual void draw(Painter& p) override {
        int cw = content_w();
        p.set_clip(x, y, cw, h);
        int vis = 0;
        for (int i = 0; i < n; i++) {
            if (!visible_node(i)) continue;
            int ry = y + vis * ROW_H - scroll;
            vis++;
            if (ry + ROW_H <= y || ry >= y + h) continue;
            if (i == selected) p.rect(x, ry, cw, ROW_H, p.theme.button_bg);
            else if (i == hover_row) p.rect(x, ry, cw, ROW_H, p.theme.button_hover);
            int ind = x + nodes[i].depth * 12;
            if (has_children(i))
                p.text(nodes[i].expanded ? "-" : "+", ind, ry + 2, p.theme.accent);
            p.text(nodes[i].label, ind + 12, ry + 2, p.theme.fg);
        }
        p.clear_clip();
        draw_bar(p);
    }
};

// ------------------------------------------------------------
// Tab — strip tab 26px + panel aktif. Panel dimiliki (didelete)
// oleh Tab. Area panel = (y+26, w × h-26), di-clip.
// ------------------------------------------------------------
class Tab : public Widget {
public:
    enum { MAX_TABS = 8, STRIP_H = 26 };
    char* titles[MAX_TABS];
    Widget* panels[MAX_TABS];
    int n, active;

    Tab(int width, int height) : n(0), active(0) {
        w = width; h = height;
        for (int i = 0; i < MAX_TABS; i++) { titles[i] = 0; panels[i] = 0; }
    }
    virtual ~Tab() {
        for (int i = 0; i < n; i++) { _ui_free(titles[i]); delete panels[i]; }
    }
    void add(const char* title, Widget* panel) {
        if (n >= MAX_TABS) return;
        titles[n] = _ui_strdup(title);
        panels[n] = panel;
        n++;
        mark_dirty();
    }
    virtual int dirty_child_count() override { return n; }
    virtual Widget* dirty_child(int i) override { return panels[i]; }
    int title_at(int mx) const {
        if (n == 0 || mx < x || mx >= x + w) return -1;
        int tw = w / n;
        int i = (mx - x) / tw;
        return (i >= 0 && i < n) ? i : -1;
    }
    virtual Widget* pick(int mx, int my) override {
        if (!visible) return 0;
        if (my >= y && my < y + STRIP_H)
            return (mx >= x && mx < x + w) ? this : 0;
        if (active < n && panels[active])
            return panels[active]->pick(mx, my);
        return 0;
    }
    virtual void on_click(int mx, int my) override {
        if (my >= y && my < y + STRIP_H) {
            int i = title_at(mx);
            if (i >= 0 && i != active) { active = i; mark_dirty(); }
            return;
        }
        if (active < n && panels[active]) {
            Widget* c = panels[active]->pick(mx, my);
            if (c) c->on_click(mx, my);
        }
    }
    virtual void draw(Painter& p) override {
        int tw = n ? w / n : w;
        for (int i = 0; i < n; i++) {
            int tx = x + i * tw;
            bool act = (i == active);
            p.rect(tx, y, tw, STRIP_H, act ? p.theme.button_bg : p.theme.bg);
            if (act) p.rect(tx, y + STRIP_H - 2, tw, 2, p.theme.accent);
            int tl = _ui_strlen(titles[i]) * 8;
            p.text(titles[i], tx + (tw - tl) / 2, y + (STRIP_H - 16) / 2, p.theme.fg);
        }
        if (active < n && panels[active]) {
            Widget* pl = panels[active];
            pl->x = x; pl->y = y + STRIP_H; pl->w = w; pl->h = h - STRIP_H;
            p.set_clip(x, y + STRIP_H, w, h - STRIP_H);
            pl->draw(p);
            p.clear_clip();
        }
    }
};

// ------------------------------------------------------------
// Toolbar — bar tombol full-width (ditaruh via Window::add_bar).
// ------------------------------------------------------------
class Toolbar : public Widget {
public:
    struct Btn { char* label; ui_click_cb cb; void* data; };
    enum { MAX_BTNS = 16 };
    Btn btns[MAX_BTNS];
    int n, hover_idx;

    Toolbar(int width) : n(0), hover_idx(-1) {
        w = width; h = 28;
    }
    virtual ~Toolbar() { for (int i = 0; i < n; i++) _ui_free(btns[i].label); }
    void add_button(const char* label, ui_click_cb cb, void* u) {
        if (n >= MAX_BTNS) return;
        btns[n].label = _ui_strdup(label);
        btns[n].cb = cb; btns[n].data = u;
        n++;
    }
    void mark_item(int i) {
        if (i < 0 || i >= n) return;
        int bw = n ? w / n : w;
        mark_area(x + i * bw, y, bw, h);
    }
    virtual void set_hover(bool on) override { if (!on && hover_idx >= 0) { mark_item(hover_idx); hover_idx = -1; } }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        if (my >= y && my < y + h && n) {
            i = (mx - x) / (w / n);
            if (i < 0 || i >= n) i = -1;
        }
        if (i == hover_idx) return false;
        mark_item(hover_idx);       // item lama
        hover_idx = i;
        mark_item(hover_idx);       // item baru
        return true;
    }
    virtual void on_click(int mx, int my) override {
        if (!n || my < y || my >= y + h) return;
        int i = (mx - x) / (w / n);
        if (i >= 0 && i < n && btns[i].cb) btns[i].cb(btns[i].data);
    }
    virtual void draw(Painter& p) override {
        int bw = n ? w / n : w;
        p.rect(x, y, w, h, p.theme.bg);
        for (int i = 0; i < n; i++) {
            int bx = x + i * bw;
            if (i == hover_idx) p.rect(bx + 2, y + 3, bw - 4, h - 6, p.theme.button_hover);
            int bl = _ui_strlen(btns[i].label) * 8;
            p.text(btns[i].label, bx + (bw - bl) / 2, y + (h - 16) / 2, p.theme.fg);
        }
    }
};

// ------------------------------------------------------------
// Menu (popup) + MenuBar (bar title full-width).
// Menu::on_click & MenuBar::on_click/draw butuh Window lengkap
// (popup handling) → didefinisikan setelah class Window.
// ------------------------------------------------------------
// ------------------------------------------------------------
// Dialog — overlay modal tengah-window (Phase 9). Non-blocking: cb(index)
// dipanggil saat tombol ditekan, index = -1 bila dibatalkan (ESC).
// Klik di luar dialog diabaikan (modal memblok input latar).
// ------------------------------------------------------------
class Dialog : public Widget {
public:
    enum { MAX_BTNS = 4 };
    char* title;
    char* text;
    char* btns[MAX_BTNS];
    int n_btns;
    int hover_btn;
    ui_dialog_cb cb;
    void* data;
    Window* win;

    Dialog(Window* w, const char* t, const char* tx,
           const char* const* b, int n, ui_dialog_cb c, void* d)
        : title(_ui_strdup(t ? t : "")), text(_ui_strdup(tx ? tx : "")),
          n_btns(n < MAX_BTNS ? n : MAX_BTNS), hover_btn(-1),
          cb(c), data(d), win(w) {
        for (int i = 0; i < MAX_BTNS; i++) btns[i] = 0;
        for (int i = 0; i < n_btns; i++) btns[i] = _ui_strdup(b[i] ? b[i] : "");
        // Ukuran dari isi: elemen terpanjang, min 220px.
        int nlines = 1;
        for (int i = 0; text[i]; i++) if (text[i] == '\n') nlines++;
        int wid = 220;
        int cand = _ui_strlen(title) * 8 + 24;
        if (cand > wid) wid = cand;
        int start = 0, i = 0;
        for (;;) {
            if (text[i] == '\n' || text[i] == '\0') {
                int len = (i - start) * 8 + 24;
                if (len > wid) wid = len;
                if (text[i] == '\0') break;
                start = i + 1;
            }
            i++;
        }
        int bw = 0;
        for (int i = 0; i < n_btns; i++) bw += btn_w(i) + 6;
        if (bw - 6 + 24 > wid) wid = bw - 6 + 24;
        this->w = wid;
        this->h = 12 + 16 + 4 + nlines * 16 + 12 + 28 + 12;
    }
    virtual ~Dialog() {
        _ui_free(title); _ui_free(text);
        for (int i = 0; i < MAX_BTNS; i++) _ui_free(btns[i]);
    }
    int btn_row_y() const { return y + h - 40; }
    int btn_total() const {
        int t = 0;
        for (int i = 0; i < n_btns; i++) t += btn_w(i) + 6;
        return t - 6;
    }
    int btn_x(int i) const {
        int bx = x + (w - btn_total()) / 2;
        for (int j = 0; j < i; j++) bx += btn_w(j) + 6;
        return bx;
    }
    int btn_w(int i) const { return _ui_strlen(btns[i]) * 8 + 20; }
    int hit_button(int mx, int my) const {
        if (my < btn_row_y() || my >= btn_row_y() + 28) return -1;
        for (int i = 0; i < n_btns; i++)
            if (mx >= btn_x(i) && mx < btn_x(i) + btn_w(i)) return i;
        return -1;
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y, w, h, p.theme.button_bg);
        p.rect(x, y, w, 1, p.theme.fg);
        p.rect(x, y + h - 1, w, 1, p.theme.fg);
        p.rect(x, y, 1, h, p.theme.fg);
        p.rect(x + w - 1, y, 1, h, p.theme.fg);
        p.text(title, x + 12, y + 12, p.theme.button_fg);
        p.text(text, x + 12, y + 32, p.theme.fg);
        int by = btn_row_y();
        for (int i = 0; i < n_btns; i++) {
            int bx = btn_x(i);
            p.rect(bx, by, btn_w(i), 28,
                   i == hover_btn ? p.theme.button_hover : p.theme.bg);
            p.text(btns[i], bx + 10, by + 6, p.theme.fg);
        }
    }
    void mark_btn(int i) {
        if (i < 0 || i >= n_btns) return;
        mark_area(btn_x(i), btn_row_y(), btn_w(i), 28);
    }
    virtual bool track_hover(int mx, int my) override {
        int i = hit_button(mx, my);
        if (i == hover_btn) return false;
        mark_btn(hover_btn);
        hover_btn = i;
        mark_btn(hover_btn);
        return true;
    }
    // out-of-class: butuh Window lengkap (close_dialog)
    virtual void on_click(int mx, int my) override;
};

class Menu : public Widget {
public:
    struct Item { char* label; ui_click_cb cb; void* data; };
    enum { MAX_ITEMS = 16 };
    Item items[MAX_ITEMS];
    int n, hover_idx;
    Window* win;

    Menu(Window* w) : n(0), hover_idx(-1), win(w) {
        this->w = 140; h = 4;
    }
    virtual ~Menu() { for (int i = 0; i < n; i++) _ui_free(items[i].label); }
    void add_item(const char* label, ui_click_cb cb, void* u) {
        if (n >= MAX_ITEMS) return;
        items[n].label = _ui_strdup(label);
        items[n].cb = cb; items[n].data = u;
        n++;
        h = n * 20 + 4;
        mark_dirty();
    }
    void mark_item(int i) {
        if (i < 0 || i >= n) return;
        mark_area(x + 1, y + 2 + i * 20, w - 2, 20);
    }
    virtual void set_hover(bool on) override { if (!on && hover_idx >= 0) { mark_item(hover_idx); hover_idx = -1; } }
    virtual bool track_hover(int mx, int my) override {
        int i = (my - y - 2) / 20;
        if (i < 0 || i >= n || mx < x || mx >= x + w) i = -1;
        if (i == hover_idx) return false;
        mark_item(hover_idx);
        hover_idx = i;
        mark_item(hover_idx);
        return true;
    }
    virtual void draw(Painter& p) override {
        p.rect(x, y, w, h, p.theme.button_bg);
        p.rect(x, y, w, 1, p.theme.fg);
        p.rect(x, y + h - 1, w, 1, p.theme.fg);
        p.rect(x, y, 1, h, p.theme.fg);
        p.rect(x + w - 1, y, 1, h, p.theme.fg);
        for (int i = 0; i < n; i++) {
            if (i == hover_idx) p.rect(x + 1, y + 2 + i * 20, w - 2, 20, p.theme.button_hover);
            p.text(items[i].label, x + 6, y + 3 + i * 20, p.theme.fg);
        }
    }
    // out-of-class: butuh Window lengkap (close_popup)
    virtual void on_click(int mx, int my) override;
};

class MenuBar : public Widget {
public:
    struct Title { char* label; Menu* menu; };
    enum { MAX_TITLES = 8 };
    Title titles[MAX_TITLES];
    int n, hover_idx;
    Window* win;

    MenuBar(Window* w, int width) : n(0), hover_idx(-1), win(w) {
        this->w = width; h = 24;
    }
    virtual ~MenuBar() {
        for (int i = 0; i < n; i++) { _ui_free(titles[i].label); delete titles[i].menu; }
    }
    Menu* add_menu(const char* title) {
        if (n >= MAX_TITLES) return 0;
        titles[n].label = _ui_strdup(title);
        titles[n].menu = new Menu(win);
        n++;
        return titles[n - 1].menu;
    }
    virtual bool is_menu_bar() override { return true; }
    void mark_title(int i) {
        if (i < 0 || i >= n) return;
        int tw = n ? w / n : w;
        mark_area(x + i * tw, y, tw, h);
    }
    virtual void set_hover(bool on) override { if (!on && hover_idx >= 0) { mark_title(hover_idx); hover_idx = -1; } }
    int title_at(int mx) const {
        if (n == 0 || mx < x || mx >= x + w) return -1;
        int tw = w / n;
        int i = (mx - x) / tw;
        return (i >= 0 && i < n) ? i : -1;
    }
    virtual bool track_hover(int mx, int my) override {
        int i = -1;
        if (my >= y && my < y + h) i = title_at(mx);
        if (i == hover_idx) return false;
        mark_title(hover_idx);
        hover_idx = i;
        mark_title(hover_idx);
        return true;
    }
    // out-of-class: butuh Window lengkap (popup switch/open/close)
    virtual void on_click(int mx, int my) override;
    virtual void draw(Painter& p) override;
};

// ------------------------------------------------------------
// Window — canvas (via libgui) + pohon widget + event loop
// ------------------------------------------------------------
class Window {
public:
    gui_window_t* gw;
    Theme theme;
    Layout* root;
    bool running;
    int mouse_x, mouse_y;
    Widget* hovered;
    Widget* focused;   // fokus keyboard intra-window (TextBox)
    Widget* grabbed;   // widget yang memegang drag (left-down sampai release)
    Widget* popup;     // overlay (menu dropdown) digambar paling atas (Phase 8)
    Widget* top_bars[4];   // bar full-width (MenuBar/Toolbar), di atas root
    int n_bars;
    int bar_h;         // tinggi kumulatif bar → offset root
    // Phase 9 — Desktop Services
    Dialog* dialog;        // modal aktif (0 = none); dimiliki window
    char* notify_text;     // toast (0 = none); dimiliki window
    uint64_t notify_until; // sys_uptime() deadline auto-expire
    Widget* drag_src;      // widget draggable yang sedang diseret (0 = none)
    const char* drag_payload;
    int drag_x, drag_y;
    int cur_cursor;        // bentuk kursor yang sudah di-set ke kernel
    struct Shortcut { uint8_t mods, key; ui_click_cb cb; void* data; };
    Shortcut shortcuts[16];
    int n_shortcuts;
    // Phase 10: tick periodik tiap iterasi event loop. Callback return 1 =
    // ada perubahan → toolkit render (jam/task manager refresh tanpa event).
    ui_tick_cb tick_cb;
    void* tick_data;
    // Phase 5: dirty rect render — region yang perlu digambar ulang frame ini.
    int dirty_valid;
    int dirty_x, dirty_y, dirty_w, dirty_h;

    Window(uint32_t width, uint32_t height)
        : gw(gui_create_window(width, height)), root(0),
          running(gw != 0), mouse_x(0), mouse_y(0), hovered(0),
          focused(0), grabbed(0), popup(0), n_bars(0), bar_h(0),
          dialog(0), notify_text(0), notify_until(0),
          drag_src(0), drag_payload(0), drag_x(0), drag_y(0),
          cur_cursor(UI_CURSOR_ARROW), n_shortcuts(0),
          tick_cb(0), tick_data(0),
          dirty_valid(0), dirty_x(0), dirty_y(0), dirty_w(0), dirty_h(0) {
        for (int i = 0; i < 4; i++) top_bars[i] = 0;
        for (int i = 0; i < 16; i++) { shortcuts[i].cb = 0; shortcuts[i].data = 0; }
    }

    ~Window() {
        sys_kwm_set_cursor(UI_CURSOR_ARROW);   // jangan tinggalkan I-beam/tangan
        if (dialog) delete dialog;
        if (notify_text) _ui_free(notify_text);
        if (root) delete root;
        if (gw) gui_destroy(gw);
    }

    void set_theme(const ui_theme_t* t) {
        if (!t) return;
        theme.bg = t->bg; theme.fg = t->fg; theme.accent = t->accent;
        theme.button_bg = t->button_bg; theme.button_fg = t->button_fg;
        theme.button_hover = t->button_hover;
        damage_full();   // tema mengubah warna SEMUA widget → broad by design
    }

    // Bar full-width (MenuBar/Toolbar) di puncak window, di atas root.
    void add_bar(Widget* b) {
        if (n_bars >= 4) return;
        b->x = 0; b->y = bar_h;
        bar_h += b->h;
        top_bars[n_bars++] = b;
        if (root) root->y = 8 + bar_h;
    }

    // Root selalu VBox bermargin 8px — widget pertama sekalipun layout.
    void add(Widget* w) {
        if (!root) { root = new VBox(8); root->x = 8; root->y = 8 + bar_h; }
        root->add(w);
    }

    Widget* pick_bar(int mx, int my) {
        for (int i = n_bars - 1; i >= 0; i--)
            if (top_bars[i]->pick(mx, my)) return top_bars[i];
        return 0;
    }

    // MenuBar::draw mewarnai judul yang popup-nya terbuka (win->popup) —
    // state itu berubah di sini, jadi bar harus ikut ditandai dirty.
    void mark_menubars() {
        for (int i = 0; i < n_bars; i++)
            if (top_bars[i] && top_bars[i]->is_menu_bar()) top_bars[i]->mark_dirty();
    }
    void open_popup(Widget* m, int ox, int oy) {
        if (popup && popup != m) popup->set_hover(false);
        bool changed = (popup != m);
        popup = m;
        m->x = ox; m->y = oy;
        if (changed) mark_menubars();
        damage_overlay(m);
        render();
    }
    void close_popup() {
        if (popup) {
            if (hovered == popup) hovered = 0;
            popup->set_hover(false);
        }
        popup = 0;
        mark_menubars();
    }

    // Kembalikan true bila hovered berubah (memicu redraw). Saat popup
    // terbuka, root TIDAK di-hover — hanya popup & bar (untuk switch menu).
    bool track_hover() {
        Widget* n = 0;
        if (dialog) {
            if (dialog->pick(mouse_x, mouse_y)) n = dialog;   // modal: hanya dialog
        } else if (popup) {
            if (popup->pick(mouse_x, mouse_y)) n = popup;
            else n = pick_bar(mouse_x, mouse_y);
        } else {
            n = pick_bar(mouse_x, mouse_y);
            if (!n && root) n = root->pick(mouse_x, mouse_y);
        }
        bool changed = false;
        if (n != hovered) {
            if (hovered) hovered->set_hover(false);
            hovered = n;
            if (hovered) hovered->set_hover(true);
            changed = true;
        }
        if (popup) { if (popup->track_hover(mouse_x, mouse_y)) changed = true; }
        if (hovered) { if (hovered->track_hover(mouse_x, mouse_y)) changed = true; }
        // Phase 9: sinkronkan bentuk kursor kernel dgn widget yang di-hover.
        int want = hovered ? hovered->cursor_kind : UI_CURSOR_ARROW;
        if (want != cur_cursor) {
            cur_cursor = want;
            sys_kwm_set_cursor(want);
        }
        return changed;
    }

    void set_focus(Widget* n) {
        if (n == focused) return;
        if (focused) focused->set_focus(false);
        focused = n;
        if (focused) focused->set_focus(true);
    }

    // --- Shortcut (Phase 9): registry per-window, cek di KEY_PRESS. ---
    void add_shortcut(uint32_t mods, uint8_t key, ui_click_cb cb, void* u) {
        if (n_shortcuts >= 16) return;
        shortcuts[n_shortcuts].mods = (uint8_t)(mods & 0x07);
        shortcuts[n_shortcuts].key = key;
        shortcuts[n_shortcuts].cb = cb;
        shortcuts[n_shortcuts].data = u;
        n_shortcuts++;
    }

    // --- Notification toast (Phase 9) ---
    void notify(const char* text, uint32_t ms) {
        char* n = _ui_strdup(text ? text : "");
        if (!n) return;
        _ui_free(notify_text);
        notify_text = n;
        notify_until = sys_uptime() + ms;
        damage_notify();
        render();
    }
    bool notify_hit(int mx, int my) const {
        if (!notify_text) return false;
        return mx >= (int)gw->width - 218 && mx < (int)gw->width - 8 &&
               my >= 8 && my < 36;
    }
    void notify_dismiss() {
        _ui_free(notify_text);
        notify_text = 0;
        damage_notify();   // hapus toast lama
        render();
    }

    // --- Dialog modal (Phase 9) ---
    void open_dialog(Dialog* d) {
        if (dialog && dialog != d) delete dialog;
        dialog = d;
        d->x = ((int)gw->width - d->w) / 2;
        d->y = ((int)gw->height - d->h) / 2 - 20;   // sedikit di atas tengah
        if (d->x < 0) d->x = 0;
        if (d->y < 0) d->y = 0;
        damage_overlay(d);
        render();
    }
    void close_dialog() {
        if (hovered == dialog) hovered = 0;
        damage_overlay(dialog);   // hapus dialog + shadow
        delete dialog;
        dialog = 0;
        render();
    }

    // --- Settings (Phase 9): persist theme ke KyuzenFS "settings.ui" ---
    int settings_save() {
        int fd = sys_open("settings.ui", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) return 0;
        sys_write_fd(fd, &theme, sizeof(ui_theme_t));
        sys_close(fd);
        return 1;
    }
    int settings_load() {
        int fd = sys_open("settings.ui", O_RDONLY);
        if (fd < 0) return 0;
        ui_theme_t t;
        int n = sys_read_fd(fd, &t, sizeof(ui_theme_t));
        sys_close(fd);
        if (n != (int)sizeof(ui_theme_t)) return 0;      // file bukan theme valid
        if (!t.bg && !t.fg && !t.accent && !t.button_bg &&
            !t.button_fg && !t.button_hover) return 0;    // blob kosong
        set_theme(&t);
        return 1;
    }

    void draw_notify(Painter& p) {
        int nx = (int)gw->width - 218, ny = 8;
        p.rect(nx, ny, 210, 28, p.theme.button_bg);
        p.rect(nx, ny, 210, 1, p.theme.fg);
        p.rect(nx, ny + 27, 210, 1, p.theme.fg);
        p.rect(nx, ny, 1, 28, p.theme.fg);
        p.rect(nx + 209, ny, 1, 28, p.theme.fg);
        p.text(notify_text, nx + 8, ny + 6, p.theme.fg);
    }
    void draw_drag_ghost(Painter& p) {
        int tw = drag_payload ? _ui_strlen(drag_payload) * 8 + 12 : 24;
        int gx = drag_x + 4, gy = drag_y + 4;
        p.rect(gx, gy, tw, 18, p.theme.button_hover);
        p.rect(gx, gy, tw, 1, p.theme.accent);
        p.rect(gx, gy + 17, tw, 1, p.theme.accent);
        p.rect(gx, gy, 1, 18, p.theme.accent);
        p.rect(gx + tw - 1, gy, 1, 18, p.theme.accent);
        if (drag_payload) p.text(drag_payload, gx + 6, gy + 2, p.theme.fg);
    }

    // --- Phase 5: damage rect (satu bbox). Over-report BOLEH, under TIDAK. ---
    void damage_rect(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)gw->width)  x1 = (int)gw->width;
        if (y1 > (int)gw->height) y1 = (int)gw->height;
        if (x1 <= x0 || y1 <= y0) return;
        if (!dirty_valid) {
            dirty_valid = 1;
            dirty_x = x0; dirty_y = y0; dirty_w = x1 - x0; dirty_h = y1 - y0;
            return;
        }
        int dx1 = dirty_x + dirty_w, dy1 = dirty_y + dirty_h;
        if (x0 < dirty_x) dirty_x = x0;
        if (y0 < dirty_y) dirty_y = y0;
        if (x1 > dx1) dx1 = x1;
        if (y1 > dy1) dy1 = y1;
        dirty_w = dx1 - dirty_x; dirty_h = dy1 - dirty_y;
    }
    void damage_widget(Widget* w) { if (w) damage_rect(w->x, w->y, w->w, w->h); }
    void damage_full() { damage_rect(0, 0, (int)gw->width, (int)gw->height); }
    // Phase 8: kumpulkan rect kotor dari widget yang benar-benar berubah saja
    // (bukan seluruh pohon). Tetap satu bbox via damage_rect.
    void collect_dirty(Widget* w) {
        if (!w) return;
        w->settle();    // Layout: reflow sebelum baca damage (old+new bounds)
        int dx, dy, dw, dh;
        if (w->take_dirty(dx, dy, dw, dh)) damage_rect(dx, dy, dw, dh);
        int n = w->dirty_child_count();
        for (int i = 0; i < n; i++) collect_dirty(w->dirty_child(i));
    }
    void damage_dirty_widgets() {
        for (int i = 0; i < n_bars; i++) collect_dirty(top_bars[i]);
        collect_dirty(root);
        collect_dirty(popup);
        collect_dirty(dialog);
    }
    // Buang damage tertunda tanpa menggambar (mis. setelah render awal).
    void forget_dirty(Widget* w) {
        if (!w) return;
        int a, b, c, d; w->take_dirty(a, b, c, d);
        int n = w->dirty_child_count();
        for (int i = 0; i < n; i++) forget_dirty(w->dirty_child(i));
    }
    void forget_dirty_widgets() {
        for (int i = 0; i < n_bars; i++) forget_dirty(top_bars[i]);
        forget_dirty(root); forget_dirty(popup); forget_dirty(dialog);
    }
    // Union bounds bar + root (recurse layout). Untuk tick/klik yang callback-nya
    // bisa mengubah widget mana pun tanpa Window tahu rect persisnya.
    void damage_all_widgets() {
        int x0 = 0x7fffffff, y0 = 0x7fffffff, x1 = -0x7fffffff, y1 = -0x7fffffff;
        for (int i = 0; i < n_bars; i++)
            if (top_bars[i]) top_bars[i]->collect_bounds(x0, y0, x1, y1);
        if (root) root->collect_bounds(x0, y0, x1, y1);
        if (x1 > x0 && y1 > y0) damage_rect(x0, y0, x1 - x0, y1 - y0);
        else damage_full();
    }
    void damage_notify() { damage_rect((int)gw->width - 218, 8, 210, 28); }
    // Overlay (popup/dialog) + drop-shadow libui (4 ring, offset +3, extend 4px).
    void damage_overlay(Widget* ov) {
        if (ov) damage_rect(ov->x - 4, ov->y - 1, ov->w + 8, ov->h + 8);
    }
    void damage_ghost(int gx, int gy) {
        int tw = drag_payload ? _ui_strlen(drag_payload) * 8 + 12 : 24;
        damage_rect(gx, gy, tw + 12, 26);
    }

    void render() {
        // Phase 8: panen flag dirty widget SETIAP render, apa pun jalur
        // pemanggilnya — mark yang dibuat callback setelah render() lain
        // (mis. dialog close) tidak boleh tertunda.
        damage_dirty_widgets();
        if (!dirty_valid) return;   // tak ada perubahan → tak ada upload
        int rx = dirty_x, ry = dirty_y, rw = dirty_w, rh = dirty_h;
        dirty_valid = 0;
        Painter p(gw, theme);
        p.set_render_clip(rx, ry, rw, rh);   // bg + widget di-clip ke dirty
        p.rect(0, 0, (int)gw->width, (int)gw->height, theme.bg);
        for (int i = 0; i < n_bars; i++) top_bars[i]->draw(p);
        if (root) root->draw(p);
        if (popup) { p.shadow(popup->x, popup->y, popup->w, popup->h); popup->draw(p); }
        if (dialog) {                     // modal di atas popup
            p.shadow(dialog->x, dialog->y, dialog->w, dialog->h);
            dialog->draw(p);
        }
        if (notify_text) draw_notify(p);  // toast paling atas
        if (drag_src) draw_drag_ghost(p); // ghost drag
        gui_flush(gw);
    }

    void run() {
        if (!gw) return;
        kyuzen_event_t ev;
        damage_full();   // render awal: seluruh window
        render();
        forget_dirty_widgets();   // bersihkan mark reflow dari render awal
        while (running) {
            // Phase 9: auto-expire notifikasi. Loop bangun ~60/s via
            // sys_yield + timer IRQ → cukup cek tiap iterasi, tanpa timer infra.
            if (notify_text && sys_uptime() >= notify_until) notify_dismiss();
            // Phase 10: tick periodik — jam/task manager render hanya saat berubah.
            if (tick_cb && tick_cb(tick_data)) render();
            if (sys_get_event(&ev)) {
                switch (ev.type) {
                case EVENT_MOUSE_MOVE:
                    mouse_x = ev.param1; mouse_y = ev.param2;
                    if (drag_src) {
                        damage_ghost(drag_x, drag_y);   // posisi lama
                        drag_x = mouse_x; drag_y = mouse_y;
                        damage_ghost(drag_x, drag_y);   // posisi baru
                        render();
                    } else {
                        if (grabbed && grabbed->on_drag(mouse_x, mouse_y)) {
                            damage_widget(grabbed);     // mis. scrollbar/scroll
                            render();
                        }
                        if (track_hover()) {
                            // Widget hover/bar yang berubah menandai rect-nya
                            // sendiri; render() memanennya (bukan seluruh widget).
                            render();
                        }
                    }
                    break;
                case EVENT_MOUSE_CLICK:
                    if (ev.param1 == 0 && ev.param2 == 1) {   // left down
                        if (notify_text && notify_hit(mouse_x, mouse_y)) {
                            notify_dismiss();   // klik toast → tutup segera
                        } else if (dialog) {
                            // Modal: klik di luar dialog diabaikan (blok latar).
                            if (dialog->pick(mouse_x, mouse_y))
                                dialog->on_click(mouse_x, mouse_y);   // bisa close
                            if (dialog) damage_overlay(dialog);        // hover tombol
                            render();
                        } else if (popup) {
                            Widget* old_pop = popup;
                            if (popup->pick(mouse_x, mouse_y)) {
                                popup->on_click(mouse_x, mouse_y);   // item menu
                            } else {
                                Widget* bar = pick_bar(mouse_x, mouse_y);
                                if (bar) {
                                    if (bar->is_menu_bar())
                                        bar->on_click(mouse_x, mouse_y);   // switch/close
                                    else { close_popup(); bar->on_click(mouse_x, mouse_y); }
                                } else {
                                    close_popup();   // klik di luar → dismiss
                                }
                            }
                            damage_overlay(old_pop);    // popup lama
                            damage_overlay(popup);      // popup baru (0 = no-op)
                            render();
                        } else {
                            Widget* picked = pick_bar(mouse_x, mouse_y);
                            if (!picked && root) picked = root->pick(mouse_x, mouse_y);
                            if (picked && picked->draggable) {
                                // DnD: klik-tahan pada widget draggable mulai
                                // drag; click_cb-nya tidak dipanggil.
                                drag_src = picked;
                                drag_payload = picked->dnd_payload;
                                drag_x = mouse_x; drag_y = mouse_y;
                            } else {
                                grabbed = picked;
                                set_focus(grabbed && grabbed->focusable() ? grabbed : 0);
                                if (grabbed) grabbed->on_click(mouse_x, mouse_y);
                            }
                            // callback (on_click) bisa mengubah widget mana pun;
                            // yg berubah menandai dirty diri masing-masing; render()
                            // memanennya menjadi satu bbox.
                            render();
                        }
                    } else if (ev.param1 == 0 && ev.param2 == 0) {   // left up
                        if (drag_src) {
                            Widget* t = pick_bar(mouse_x, mouse_y);
                            if (!t && root) t = root->pick(mouse_x, mouse_y);
                            if (t && t->drop_target && t->drop_cb)
                                t->drop_cb(t->drop_data, drag_payload, mouse_x, mouse_y);
                            damage_ghost(drag_x, drag_y);   // hapus ghost
                            drag_src = 0; drag_payload = 0; // drop_cb marks widgets
                        } else if (grabbed) {
                            damage_widget(grabbed);
                            grabbed->on_release(); grabbed = 0;
                        }
                        render();
                    }
                    break;
                case EVENT_KEY_PRESS:
                    if (ev.param1 == 27) {
                        // ESC jarang → full-window aman (menu/dialog/keluar).
                        damage_full();
                        if (popup) close_popup();   // ESC tutup menu dulu
                        else if (dialog) {
                            ui_dialog_cb c = dialog->cb; void* d = dialog->data;
                            close_dialog();
                            if (c) c(d, -1);        // -1 = batal
                        } else running = false;
                        render();
                    } else if (dialog) {
                        render();   // modal menelan ketikan selain ESC (tak berubah)
                    } else {
                        // Shortcut registry dulu, baru dispatch ke widget fokus.
                        bool handled = false;
                        uint32_t mods = (uint32_t)ev.param2;
                        for (int i = 0; i < n_shortcuts; i++) {
                            if ((mods & 0x07) == (shortcuts[i].mods & 0x07) &&
                                (uint8_t)ev.param1 == shortcuts[i].key) {
                                if (shortcuts[i].cb) shortcuts[i].cb(shortcuts[i].data);
                                handled = true;
                                break;
                            }
                        }
                        if (!handled && focused)
                            focused->on_key((uint8_t)ev.param1,
                                            (uint32_t)ev.param3,
                                            (uint32_t)ev.param2);
                        // Callback shortcut / focused->on_key bisa ubah widget mana
                        // pun; yg berubah menandai dirty; render() memanennya.
                        render();
                    }
                    break;
                case EVENT_SCROLL:
                    if (hovered && hovered->on_scroll(ev.param1)) {
                        damage_widget(hovered);   // viewport scroll area
                        render();
                    }
                    break;
                case EVENT_WIN_CLOSE:
                    running = false;
                    break;
                default:
                    break;
                }
            }
            sys_yield();
        }
    }
};

// Out-of-class: butuh Window lengkap (popup handling).
void Dialog::on_click(int mx, int my) {
    int i = hit_button(mx, my);
    if (i < 0) return;              // klik body dialog → tetap modal, abaikan
    ui_dialog_cb c = cb;
    void* d = data;
    if (win) win->close_dialog();   // hapus dialog dulu (delete this)
    if (c) c(d, i);                 // lalu fire cb — jangan sentuh member lagi
}
void Menu::on_click(int mx, int my) {
    int idx = (my - y - 2) / 20;
    if (idx >= 0 && idx < n) {
        ui_click_cb cb = items[idx].cb;
        void* d = items[idx].data;
        if (win) win->close_popup();
        if (cb) cb(d);
    }
}
void MenuBar::on_click(int mx, int my) {
    int i = title_at(mx);
    if (i < 0) { if (win) win->close_popup(); return; }
    if (win && win->popup == titles[i].menu) { win->close_popup(); return; }
    if (win) win->open_popup(titles[i].menu, x + i * (n ? w / n : w), y + h);
}
void MenuBar::draw(Painter& p) {
    int tw = n ? w / n : w;
    for (int i = 0; i < n; i++) {
        int tx = x + i * tw;
        bool open = win && win->popup == titles[i].menu;
        p.rect(tx, y, tw, h, (open || i == hover_idx) ? p.theme.button_hover : p.theme.button_bg);
        p.text(titles[i].label, tx + 8, y + 4, p.theme.button_fg);
    }
}

} // namespace ui

// ============================================================
// Public C ABI — bridge ke toolkit C++. Handle opaque: void* di
// balik ui_window_t/ui_widget_t adalah pointer objek C++ (ui::*).
// ============================================================
extern "C" {

ui_window_t* ui_window_create(uint32_t width, uint32_t height) {
    return reinterpret_cast<ui_window_t*>(new ui::Window(width, height));
}

void ui_window_destroy(ui_window_t* win) {
    delete reinterpret_cast<ui::Window*>(win);
}

void ui_window_set_theme(ui_window_t* win, const ui_theme_t* theme) {
    reinterpret_cast<ui::Window*>(win)->set_theme(theme);
}

void ui_window_add(ui_window_t* win, ui_widget_t* widget) {
    reinterpret_cast<ui::Window*>(win)->add(reinterpret_cast<ui::Widget*>(widget));
}

void ui_window_run(ui_window_t* win) {
    reinterpret_cast<ui::Window*>(win)->run();
}

// Hentikan event loop window (mis. `logout` di terminal). Aman dipanggil dari
// dalam callback: loop memeriksa `running` tiap iterasi.
void ui_window_request_close(ui_window_t* win) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (w) w->running = false;
}

// Phase 10: judul window (titlebar + taskbar).
void ui_window_set_title(ui_window_t* win, const char* title) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (w && w->gw) gui_set_window_title(w->gw, title);
}

void ui_window_focus(ui_window_t* win, ui_widget_t* widget) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (w) w->set_focus(reinterpret_cast<ui::Widget*>(widget));
}

// Phase 10: callback periodik tiap iterasi loop (~60/s). Return 1 = berubah →
// toolkit render (jam/task manager refresh tanpa event mouse/keyboard).
void ui_window_set_tick(ui_window_t* win, ui_tick_cb cb, void* userdata) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (w) { w->tick_cb = cb; w->tick_data = userdata; }
}

ui_widget_t* ui_label_create(ui_window_t* win, const char* text) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Label(text));
}

void ui_label_set_text(ui_widget_t* widget, const char* text) {
    reinterpret_cast<ui::Label*>(widget)->set_text(text);
}

ui_widget_t* ui_button_create(ui_window_t* win, const char* text) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Button(text));
}

void ui_button_set_click(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Widget*>(widget)->set_click(cb, userdata);
}

ui_widget_t* ui_textbox_create(ui_window_t* win, int width) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::TextBox(width));
}

void ui_textbox_set_text(ui_widget_t* widget, const char* text) {
    reinterpret_cast<ui::TextBox*>(widget)->set_text(text);
}

const char* ui_textbox_text(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextBox*>(widget)->text;
}

void ui_textbox_set_enter(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::TextBox* tb = reinterpret_cast<ui::TextBox*>(widget);
    tb->enter_cb = cb; tb->enter_data = userdata;
}

ui_widget_t* ui_checkbox_create(ui_window_t* win, const char* label) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::CheckBox(label));
}

void ui_checkbox_set_checked(ui_widget_t* widget, int checked) {
    reinterpret_cast<ui::CheckBox*>(widget)->set_checked(checked != 0);
}

int ui_checkbox_checked(ui_widget_t* widget) {
    return reinterpret_cast<ui::CheckBox*>(widget)->checked ? 1 : 0;
}

void ui_checkbox_set_toggle(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::CheckBox* cbx = reinterpret_cast<ui::CheckBox*>(widget);
    cbx->toggle_cb = cb; cbx->toggle_data = userdata;
}

ui_widget_t* ui_slider_create(ui_window_t* win, int min, int max) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Slider(min, max));
}

void ui_slider_set_value(ui_widget_t* widget, int value) {
    reinterpret_cast<ui::Slider*>(widget)->set_value(value);
}

int ui_slider_value(ui_widget_t* widget) {
    return reinterpret_cast<ui::Slider*>(widget)->val;
}

void ui_slider_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::Slider* s = reinterpret_cast<ui::Slider*>(widget);
    s->change_cb = cb; s->change_data = userdata;
}

ui_widget_t* ui_progressbar_create(ui_window_t* win, int width) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::ProgressBar(width));
}

void ui_progressbar_set_value(ui_widget_t* widget, int value) {
    reinterpret_cast<ui::ProgressBar*>(widget)->set_value(value);
}

ui_widget_t* ui_image_create(ui_window_t* win, const char* filename, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Image(filename, w, h));
}

// Phase 10: zoom — target display size = natural PNG × percent/100.
void ui_image_set_scale(ui_widget_t* widget, int percent) {
    reinterpret_cast<ui::Image*>(widget)->set_scale(percent);
}

// Phase 10: ganti file PNG yang ditampilkan (viewer galeri), reset zoom.
void ui_image_set_file(ui_widget_t* widget, const char* filename) {
    reinterpret_cast<ui::Image*>(widget)->set_file(filename);
}

// --- TextEdit (Phase 10) ---
ui_widget_t* ui_textedit_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::TextEdit(w, h));
}

void ui_textedit_set_text(ui_widget_t* widget, const char* text) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    te->len = 0;
    while (text[te->len] && te->len < ui::TextEdit::MAX_TEXT - 1) {
        te->text[te->len] = text[te->len];
        te->len++;
    }
    te->text[te->len] = '\0';
    te->cur = te->len;
    te->mark_dirty();
}

const char* ui_textedit_text(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->text;
}

void ui_textedit_set_readonly(ui_widget_t* widget, int ro) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    te->readonly = ro != 0;
    te->mark_dirty();
}

// Terminal shell: Enter diserahkan ke app (submit), edit terkunci di baris
// perintah terakhir. Output historis tidak bisa diubah.
void ui_textedit_set_enter(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::TextEdit*>(widget)->set_enter(cb, userdata);
}

void ui_textedit_append(ui_widget_t* widget, const char* text) {
    reinterpret_cast<ui::TextEdit*>(widget)->append(text);
}

void ui_textedit_set_prompt_style(ui_widget_t* widget, const char* prefix, uint32_t color) {
    reinterpret_cast<ui::TextEdit*>(widget)->set_prompt_style(prefix, color);
}

void ui_textedit_clear(ui_widget_t* widget) {
    reinterpret_cast<ui::TextEdit*>(widget)->clear();
}

ui_widget_t* ui_vbox_create(ui_window_t* win, int spacing) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::VBox(spacing));
}

// Phase 10: HBox — susun anak horizontal (grid tombol kalkulator).
ui_widget_t* ui_hbox_create(ui_window_t* win, int spacing) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::HBox(spacing));
}

// Phase 10: paksa ukuran widget (tombol kalkulator seragam dalam grid).
void ui_widget_set_size(ui_widget_t* widget, int w, int h) {
    ui::Widget* wid = reinterpret_cast<ui::Widget*>(widget);
    wid->mark_dirty();          // bounds lama
    wid->w = w; wid->h = h;
    wid->mark_dirty();          // bounds baru
}

void ui_layout_add(ui_widget_t* layout, ui_widget_t* child) {
    reinterpret_cast<ui::Layout*>(layout)->add(reinterpret_cast<ui::Widget*>(child));
}

// --- Phase 8: bar full-width (MenuBar/Toolbar) ---
void ui_window_add_bar(ui_window_t* win, ui_widget_t* bar) {
    reinterpret_cast<ui::Window*>(win)->add_bar(reinterpret_cast<ui::Widget*>(bar));
}

// --- ScrollView ---
ui_widget_t* ui_scrollview_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::ScrollView(w, h));
}

void ui_scrollview_set_child(ui_widget_t* widget, ui_widget_t* child) {
    reinterpret_cast<ui::ScrollView*>(widget)->set_child(reinterpret_cast<ui::Widget*>(child));
}

// --- ListView ---
ui_widget_t* ui_listview_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::ListView(w, h));
}

void ui_listview_add_item(ui_widget_t* widget, const char* label) {
    reinterpret_cast<ui::ListView*>(widget)->add_item(label);
}

int ui_listview_selected(ui_widget_t* widget) {
    return reinterpret_cast<ui::ListView*>(widget)->selected;
}

void ui_listview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::ListView*>(widget)->set_change(cb, userdata);
}

// --- Table ---
ui_widget_t* ui_table_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Table(w, h));
}

void ui_table_add_column(ui_widget_t* widget, const char* title, int width) {
    reinterpret_cast<ui::Table*>(widget)->add_column(title, width);
}

void ui_table_add_row(ui_widget_t* widget, const char* const* cells, int n) {
    reinterpret_cast<ui::Table*>(widget)->add_row(cells, n);
}

// Phase 10: kosongkan semua baris (refresh daftar file Explorer).
void ui_table_clear(ui_widget_t* widget) {
    reinterpret_cast<ui::Table*>(widget)->clear();
}

int ui_table_selected(ui_widget_t* widget) {
    return reinterpret_cast<ui::Table*>(widget)->selected;
}

void ui_table_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Table*>(widget)->set_change(cb, userdata);
}

// --- TreeView ---
ui_widget_t* ui_treeview_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::TreeView(w, h));
}

void ui_treeview_add_node(ui_widget_t* widget, const char* label, int depth, int expanded) {
    reinterpret_cast<ui::TreeView*>(widget)->add_node(label, depth, expanded != 0);
}

int ui_treeview_selected(ui_widget_t* widget) {
    return reinterpret_cast<ui::TreeView*>(widget)->selected;
}

void ui_treeview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::TreeView*>(widget)->set_change(cb, userdata);
}

// --- Tab ---
ui_widget_t* ui_tab_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Tab(w, h));
}

void ui_tab_add(ui_widget_t* widget, const char* title, ui_widget_t* panel) {
    reinterpret_cast<ui::Tab*>(widget)->add(title, reinterpret_cast<ui::Widget*>(panel));
}

// --- MenuBar + Menu ---
ui_widget_t* ui_menubar_create(ui_window_t* win) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    return reinterpret_cast<ui_widget_t*>(new ui::MenuBar(w, (int)w->gw->width));
}

ui_widget_t* ui_menubar_add_menu(ui_widget_t* bar, const char* title) {
    return reinterpret_cast<ui_widget_t*>(
        reinterpret_cast<ui::MenuBar*>(bar)->add_menu(title));
}

void ui_menu_add_item(ui_widget_t* menu, const char* label, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Menu*>(menu)->add_item(label, cb, userdata);
}

// --- Toolbar ---
ui_widget_t* ui_toolbar_create(ui_window_t* win) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    return reinterpret_cast<ui_widget_t*>(new ui::Toolbar((int)w->gw->width));
}

void ui_toolbar_add_button(ui_widget_t* bar, const char* label, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Toolbar*>(bar)->add_button(label, cb, userdata);
}

// ============================================================
// Phase 9 — Desktop Services (Clipboard, Shortcut, Dialog,
// Notification, Drag & Drop, Cursor, Settings)
// ============================================================

// --- Clipboard ---
void ui_clipboard_set_text(const char* text) {
    clipboard_set(text);
}
const char* ui_clipboard_get_text(void) {
    return clipboard_get();
}
void ui_clipboard_clear(void) {
    clipboard_clear();
}

// --- Shortcut ---
void ui_window_add_shortcut(ui_window_t* win, uint32_t mods, uint8_t key,
                            ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Window*>(win)->add_shortcut(mods, key, cb, userdata);
}

// --- Dialog ---
void ui_dialog_show(ui_window_t* win, const char* title, const char* text,
                    const char* const* buttons, int n_buttons,
                    ui_dialog_cb cb, void* userdata) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    ui::Dialog* d = new ui::Dialog(w, title, text, buttons, n_buttons, cb, userdata);
    w->open_dialog(d);
}

// --- Notification ---
void ui_window_notify(ui_window_t* win, const char* text, uint32_t ms) {
    reinterpret_cast<ui::Window*>(win)->notify(text, ms);
}

// --- Drag & Drop ---
void ui_widget_set_draggable(ui_widget_t* widget, const char* payload) {
    reinterpret_cast<ui::Widget*>(widget)->set_draggable(payload);
}
void ui_widget_set_drop_target(ui_widget_t* widget, ui_drop_cb cb, void* userdata) {
    reinterpret_cast<ui::Widget*>(widget)->set_drop_target(cb, userdata);
}

// --- Cursor ---
void ui_widget_set_cursor(ui_widget_t* widget, int kind) {
    reinterpret_cast<ui::Widget*>(widget)->set_cursor(kind);
}

// --- Settings ---
int ui_settings_save(ui_window_t* win) {
    return reinterpret_cast<ui::Window*>(win)->settings_save();
}
int ui_settings_load(ui_window_t* win) {
    return reinterpret_cast<ui::Window*>(win)->settings_load();
}

} // extern "C"
