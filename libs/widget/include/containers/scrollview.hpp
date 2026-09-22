// libs/widget/include/containers/scrollview.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_CONTAINERS_SCROLLVIEW_HPP
#define KWIDGET_CONTAINERS_SCROLLVIEW_HPP

#include "containers/scrollable.hpp"

namespace ui {

// ------------------------------------------------------------
// ScrollView — wadah scrollable generik; konten = satu widget anak.
// ------------------------------------------------------------
class ScrollView : public Scrollable {
public:
    Widget* child;
    // Phase 11: mode "lihat gambar" — scroll 2 arah (bar horizontal hanya saat
    // perlu), anak di tengah saat lebih kecil dari view, dan titik tengah view
    // dipertahankan saat ukuran anak berubah (zoom). Default OFF supaya app lain
    // (settings/widget_demo/notepad) tidak berubah satu piksel pun.
    bool pan;
    int hscroll, hscroll_max;
    bool hbar_drag;
    int hbar_grab_x, hbar_grab_scroll;
    // Phase 12: mode pan juga bisa diseret (drag-to-pan) — perilaku viewer
    // gambar standar: tarik isi dengan tombol kiri. Hanya aktif kalau isi
    // memang lebih besar dari view (kalau muat, tidak ada yang bisa digeser
    // dan klik konten tetap diteruskan ke anak seperti sebelumnya).
    bool pan_drag;
    int pan_grab_x, pan_grab_y, pan_grab_sx, pan_grab_sy;
    int last_cw, last_ch, last_vw, last_vh;   // ukuran terakhir (anchor zoom)

    ScrollView(int width, int height)
        : child(0), pan(false), hscroll(0), hscroll_max(0), hbar_drag(false),
          hbar_grab_x(0), hbar_grab_scroll(0),
          pan_drag(false), pan_grab_x(0), pan_grab_y(0), pan_grab_sx(0),
          pan_grab_sy(0),
          last_cw(0), last_ch(0), last_vw(0), last_vh(0) {
        w = width; h = height;
        set_scroll_max(0);
    }
    virtual ~ScrollView() { if (child) delete child; }
    void set_child(Widget* c) {
        child = c;
        update_scroll_maxes();
        if (c) mark_dirty();
    }
    void set_pan(int on) {
        bool v = on != 0;
        if (v == pan) return;
        pan = v;
        hscroll = 0;
        mark_dirty();
    }
    // Scrollbar horizontal hanya tampil kalau isi lebih lebar dari view.
    bool hbar_shown() const { return pan && hscroll_max > 0; }
    // Tinggi viewport yang benar-benar terlihat (dikurangi bar horizontal).
    int view_h() const { return h - (hbar_shown() ? BAR_W : 0); }
    int hbar_thumb_w() const {
        int vw = content_w();
        int content_w_px = vw + hscroll_max;
        if (content_w_px <= 0) return vw;
        int tw = vw * vw / content_w_px;
        if (tw < 8) tw = 8;
        return tw;
    }
    // Hitung ulang kedua max. Mode biasa = aturan lama (cuma vertikal) supaya
    // app lain identik; mode pan menghitung bar mana yang perlu tampil dulu.
    void update_scroll_maxes() {
        if (!child) { set_scroll_view(0, h); hscroll_max = 0; hscroll = 0; return; }
        if (!pan) { set_scroll_max(child->h); hscroll_max = 0; hscroll = 0; return; }
        int vw = w, vh = h;
        for (int i = 0; i < 2; i++) {          // vbar⇄hbar saling menyempitkan
            int x0 = child->w, y0 = child->h;
            bool vbar = y0 > vh;
            vw = w - (vbar ? BAR_W : 0);
            bool hbar = x0 > vw;
            vh = h - (hbar ? BAR_W : 0);
        }
        set_scroll_view(child->h, vh);
        hscroll_max = child->w - vw;
        if (hscroll_max < 0) hscroll_max = 0;
        if (hscroll > hscroll_max) hscroll = hscroll_max;
    }
    // Offset anak relatif terhadap viewport (center bila muat, else scroll).
    void child_offset(int& ox, int& oy) const {
        int vw = content_w(), vh = view_h();
        if (!pan) { ox = 0; oy = -scroll; return; }
        ox = child->w < vw ? (vw - child->w) / 2 : -hscroll;
        oy = child->h < vh ? (vh - child->h) / 2 : -scroll;
    }
    virtual int dirty_child_count() override { return child ? 1 : 0; }
    virtual Widget* dirty_child(int i) override { (void)i; return child; }
    // Konten (mis. Image zoom) bisa berubah ukuran tanpa event → scrollbar
    // muncul/hilang. Recompute sebelum panen damage agar strip ikut ter-render.
    virtual void settle() override {
        if (!child) return;
        int old = scroll_max, oldh = hscroll_max;
        update_scroll_maxes();
        if (pan && last_cw > 0) {
            int vw = content_w(), vh = view_h();
            if (child->w != last_cw || child->h != last_ch ||
                vw != last_vw || vh != last_vh) {
                // Zoom: titik tengah view dipertahankan, jadi gambar membesar dari
                // tengah (bukan melompat ke pojok kiri-atas). Saat isi lama lebih
                // KECIL dari view, posisi itu bukan `scroll` (anak di-center) —
                // tengah view = tengah isi, jadi pakai last_c?/2.
                int sx = last_cw < last_vw ? last_cw / 2 : hscroll + last_vw / 2;
                int sy = last_ch < last_vh ? last_ch / 2 : scroll + last_vh / 2;
                hscroll = sx * child->w / last_cw - vw / 2;
                scroll  = sy * child->h / last_ch - vh / 2;
                if (hscroll < 0) hscroll = 0;
                if (hscroll > hscroll_max) hscroll = hscroll_max;
                if (scroll < 0) scroll = 0;
                if (scroll > scroll_max) scroll = scroll_max;
            }
        }
        if (pan) {
            last_cw = child->w; last_ch = child->h;
            last_vw = content_w(); last_vh = view_h();
        }
        if (scroll_max != old || hscroll_max != oldh) mark_dirty();
    }
    // Konten bisa digeser pada kedua sumbu (syarat drag-to-pan).
    bool pannable() const {
        if (!pan || !child) return false;
        return hscroll_max > 0 || scroll_max > 0;
    }
    // Phase 12: focusable saat mode pan → panah/PgUp/PgDn/Home/End menggeser
    // tampilan (viewer gambar tanpa mouse). Widget non-pan tetap tidak
    // menyita fokus keyboard intra-window.
    virtual bool focusable() override { return pan; }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        (void)ascii; (void)mods;
        if (!pan) return;
        const int STEP = 40;
        int dsx = 0, dsy = 0;
        switch (scancode) {
        case 0x148: dsy = -STEP; break;              // Up
        case 0x150: dsy = STEP; break;               // Down
        case 0x14B: dsx = -STEP; break;              // Left
        case 0x14D: dsx = STEP; break;               // Right
        case 0x149: dsy = -view_h() / 2; break;      // PageUp
        case 0x151: dsy = view_h() / 2; break;       // PageDown
        case 0x147: dsx = -hscroll; dsy = -scroll; break;      // Home
        case 0x14F: dsx = hscroll_max - hscroll;               // End
                    dsy = scroll_max - scroll; break;
        default: return;
        }
        int ns = scroll + dsy;
        int nh = hscroll + dsx;
        if (ns < 0) ns = 0;
        if (ns > scroll_max) ns = scroll_max;
        if (nh < 0) nh = 0;
        if (nh > hscroll_max) nh = hscroll_max;
        if (ns == scroll && nh == hscroll) return;
        scroll = ns; hscroll = nh;
        mark_dirty();
    }
    virtual void on_content_click(int mx, int my) override {
        if (pannable()) {
            pan_drag = true;
            pan_grab_x = mx; pan_grab_y = my;
            pan_grab_sx = hscroll; pan_grab_sy = scroll;
            return;
        }
        if (!child) { if (click_cb) click_cb(userdata); return; }
        int ox, oy;
        child_offset(ox, oy);
        child->x = x + ox; child->y = y + oy;
        Widget* c = child->pick(mx, my);
        if (c) c->on_click(mx, my);
    }
    // Strip bawah = bar horizontal (diambil dulu sebelum bar vertikal).
    bool hbar_hit(int mx, int my) const {
        return pan && hbar_shown() && my >= y + h - BAR_W && my < y + h &&
               mx >= x && mx < x + content_w();
    }
    virtual void on_click(int mx, int my) override {
        if (hbar_hit(mx, my)) {
            hbar_drag = true;
            hbar_grab_x = mx;
            hbar_grab_scroll = hscroll;
            int tw = hbar_thumb_w();
            int range = content_w() - tw;
            if (range > 0) hscroll = (mx - x - tw / 2) * hscroll_max / range;
            if (hscroll < 0) hscroll = 0;
            if (hscroll > hscroll_max) hscroll = hscroll_max;
            mark_dirty();
            return;
        }
        Scrollable::on_click(mx, my);
    }
    virtual bool on_drag(int mx, int my) override {
        if (pan_drag) {
            int nh = pan_grab_sx - (mx - pan_grab_x);
            int ns = pan_grab_sy - (my - pan_grab_y);
            if (nh < 0) nh = 0;
            if (nh > hscroll_max) nh = hscroll_max;
            if (ns < 0) ns = 0;
            if (ns > scroll_max) ns = scroll_max;
            if (nh == hscroll && ns == scroll) return false;
            hscroll = nh; scroll = ns;
            mark_dirty();
            return true;
        }
        if (!hbar_drag) return Scrollable::on_drag(mx, my);
        int tw = hbar_thumb_w();
        int range = content_w() - tw;
        if (range <= 0) return true;
        hscroll = hbar_grab_scroll + (mx - hbar_grab_x) * hscroll_max / range;
        if (hscroll < 0) hscroll = 0;
        if (hscroll > hscroll_max) hscroll = hscroll_max;
        mark_dirty();
        return true;
    }
    virtual void on_release() override {
        pan_drag = false;
        hbar_drag = false;
        Scrollable::on_release();
    }
    void draw_hbar(Painter& p) {
        if (!hbar_shown()) return;
        int vw = content_w();
        int by = y + h - BAR_W;
        p.rect(x, by, vw, BAR_W, p.theme.button_bg);
        int tw = hbar_thumb_w();
        int range = vw - tw;
        int tx = range > 0 ? x + hscroll * range / hscroll_max : x;
        p.rect(tx, by, tw, BAR_W, p.theme.accent);
    }
    virtual void draw(Painter& p) override {
        if (!child) { p.rect(x, y, w, h, p.theme.button_bg); draw_bar(p); return; }
        int ox, oy;
        child_offset(ox, oy);
        child->x = x + ox; child->y = y + oy;
        // draw pertama hanya untuk arrange (VBox menghitung h-nya di sini);
        // keduanya ter-clip viewport agar isi yang lebih panjang dari view
        // tidak bocor keluar. Lalu hitung ulang scroll_max (bar mungkin
        // muncul → konten menyempit) dan gambar ulang dengan lebar benar.
        p.set_clip(x, y, w, h);
        child->draw(p);
        update_scroll_maxes();
        child_offset(ox, oy);
        child->x = x + ox; child->y = y + oy;
        p.set_clip(x, y, content_w(), view_h());
        child->draw(p);
        p.clear_clip();
        draw_bar(p);
        draw_hbar(p);
    }
};

} // namespace ui

#endif // KWIDGET_CONTAINERS_SCROLLVIEW_HPP
