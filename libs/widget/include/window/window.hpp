// libs/widget/include/window/window.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_WINDOW_WINDOW_HPP
#define KWIDGET_WINDOW_WINDOW_HPP

#include "runtime/platform.hpp"
#include "runtime/memory.hpp"
#include "core/theme.hpp"
#include "core/painter.hpp"
#include "core/widget.hpp"
#include "layout/layout.hpp"
#include "layout/vbox.hpp"
#include "dialog/dialog.hpp"
#include "dialog/promptdialog.hpp"

namespace ui {

// Format file "settings.ui" — lihat include/libui.h (blok ui_settings_save).
// Tag 4 byte membedakan v1 (28 byte) dari file lama v0 (24 byte, tanpa tag).
constexpr int  SETTINGS_TAG_LEN = 4;
constexpr char SETTINGS_TAG[SETTINGS_TAG_LEN + 1] = "KTH1";
constexpr int  SETTINGS_V1_LEN = SETTINGS_TAG_LEN + (int)sizeof(ui_theme_t);

static bool tag_match(const char* b, const char* tag) {
    for (int i = 0; i < SETTINGS_TAG_LEN; i++)
        if (b[i] != tag[i]) return false;
    return true;
}

// Tema kosong = semua RGB nol (alpha selalu 255 di file, jadi tidak dihitung).
static bool theme_empty(const ui_theme_t* t) {
    color_t c[6] = { t->bg, t->fg, t->accent, t->button_bg, t->button_fg, t->button_hover };
    for (int i = 0; i < 6; i++)
        if (c[i].r || c[i].g || c[i].b) return false;
    return true;
}

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
    Shortcut shortcuts[32];      // Phase 11: notepad punya banyak accelerator
    int n_shortcuts;
    // Phase 10: tick periodik tiap iterasi event loop. Callback return 1 =
    // ada perubahan → toolkit render (jam/task manager refresh tanpa event).
    ui_tick_cb tick_cb;
    void* tick_data;
    // ESC global: bila callback-nya di-set, APLIKASI yang memutuskan (mis.
    // Notepad menutup bar cari dulu, baru keluar); bila 0, ESC menutup window
    // seperti perilaku lama.
    ui_click_cb escape_cb;
    void* escape_data;
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
          tick_cb(0), tick_data(0), escape_cb(0), escape_data(0),
          dirty_valid(0), dirty_x(0), dirty_y(0), dirty_w(0), dirty_h(0) {
        for (int i = 0; i < 4; i++) top_bars[i] = 0;
        for (int i = 0; i < 32; i++) { shortcuts[i].cb = 0; shortcuts[i].data = 0; }
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
        theme.set(t);   // sekalian hitung ulang permukaan turunan (derive())
        damage_full();   // tema mengubah warna SEMUA widget → broad by design
    }

    // Bar full-width (MenuBar/Toolbar) di puncak window, di atas root.
    void add_bar(Widget* b) {
        if (n_bars >= 4) return;
        b->x = 0; b->y = bar_h;
        bar_h += b->h;
        b->set_owner(this);
        top_bars[n_bars++] = b;
        if (root) root->y = 8 + bar_h;
    }

    // Root selalu VBox bermargin 8px — widget pertama sekalipun layout.
    void add(Widget* w) {
        if (!root) { root = new VBox(8); root->x = 8; root->y = 8 + bar_h; root->set_owner(this); }
        w->set_owner(this);
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
        // Popup menu terakhir (Help) bisa melewati tepi kanan window → geser.
        if (ox + (int)m->w > (int)gw->width - 2) ox = (int)gw->width - 2 - (int)m->w;
        if (ox < 0) ox = 0;
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
        if (n_shortcuts >= 32) return;
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
        if (!dialog) return;
        if (hovered == dialog) hovered = 0;
        damage_overlay(dialog);   // hapus dialog + shadow
        delete dialog;
        dialog = 0;
        render();
    }

    // Selesaikan prompt: tutup dialog DULU (jadi callback bebas membuka dialog
    // baru), isi input disalin ke buffer lokal sebelum objek dialog dihapus.
    void close_prompt(class PromptDialog* d, int ok) {
        if (dialog != d) return;
        char buf[256];
        int i = 0;
        if (ok) { for (; d->input[i] && i < 255; i++) buf[i] = d->input[i]; }
        buf[i] = '\0';
        ui_prompt_cb c = d->pcb;
        void* dd = d->pdata;
        close_dialog();
        if (c) c(dd, ok ? buf : 0);
    }

    // --- Settings (Phase 9): persist theme ke KyuzenFS "settings.ui" ---
    // Dua versi format (lihat include/libui.h):
    //   v1 — tag "KTH1" + 6 x color_t = 28 byte  (yang ditulis sekarang)
    //   v0 — 6 x uint32 0x00RRGGBB = 24 byte      (file lama; tetap dibaca)
    // Ukuran payload beda (24 vs 28) + tag, jadi versinya bisa dibedakan
    // tanpa menyentuh struct ABI.
    int settings_save() {
        int fd = sys_open("settings.ui", O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) return 0;
        char buf[SETTINGS_V1_LEN];
        memcpy(buf, SETTINGS_TAG, SETTINGS_TAG_LEN);
        ui_theme_t out;
        theme.to_abi(&out);   // 6 x color_t (struct = blob: semua field uint8)
        memcpy(buf + SETTINGS_TAG_LEN, &out, sizeof(out));
        int n = sys_write_fd(fd, buf, SETTINGS_V1_LEN);
        sys_close(fd);
        return n == SETTINGS_V1_LEN;
    }
    int settings_load() {
        int fd = sys_open("settings.ui", O_RDONLY);
        if (fd < 0) return 0;
        char buf[SETTINGS_V1_LEN];
        int n = sys_read_fd(fd, buf, SETTINGS_V1_LEN);
        sys_close(fd);
        ui_theme_t t;
        if (n == SETTINGS_V1_LEN && tag_match(buf, SETTINGS_TAG)) {
            memcpy(&t, buf + SETTINGS_TAG_LEN, sizeof(t));       // v1
        } else if (n == (int)sizeof(ui_theme_t)) {
            // v0: theme ditulis sebagai 6 x uint32 0x00RRGGBB.
            uint32_t legacy[6];
            memcpy(legacy, buf, sizeof(legacy));
            color_t c[6];
            for (int i = 0; i < 6; i++) c[i] = color_from_u32(legacy[i], FORMAT_ARGB);
            t.bg = c[0]; t.fg = c[1]; t.accent = c[2];
            t.button_bg = c[3]; t.button_fg = c[4]; t.button_hover = c[5];
        } else {
            return 0;   // ukuran tak dikenal → bukan theme
        }
        if (theme_empty(&t)) return 0;     // blob kosong (semua RGB nol)
        set_theme(&t);
        return 1;
    }

    void draw_notify(Painter& p) {
        int nx = (int)gw->width - 218, ny = 8;
        p.rect(nx, ny, 210, 28, p.theme.panel);
        p.rect(nx, ny, 210, 1, p.theme.mborder);
        p.rect(nx, ny + 27, 210, 1, p.theme.mborder);
        p.rect(nx, ny, 1, 28, p.theme.mborder);
        p.rect(nx + 209, ny, 1, 28, p.theme.mborder);
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
                            // on_cancel() dijalankan SEBELUM dialog dihapus
                            // (prompt menyalin isi input di sini).
                            dialog->on_cancel();
                            close_dialog();
                            if (c) c(d, -1);        // -1 = batal
                        } else if (escape_cb) {
                            escape_cb(escape_data);   // aplikasi yang memutuskan
                        } else running = false;
                        render();
                    } else if (dialog) {
                        // Ketikan diteruskan ke dialog (prompt input teks).
                        // dialog bisa terhapus di dalam on_key (Enter = OK) →
                        // periksa dulu sebelum menyentuhnya lagi.
                        Dialog* d = dialog;
                        d->on_key((uint8_t)ev.param1, (uint32_t)ev.param3, (uint32_t)ev.param2);
                        if (dialog == d) damage_overlay(d);
                        render();
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

} // namespace ui

#endif // KWIDGET_WINDOW_WINDOW_HPP
