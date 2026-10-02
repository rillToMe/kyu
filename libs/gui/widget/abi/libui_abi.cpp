// libs/widget/abi/libui_abi.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "runtime/platform.hpp"
#include "runtime/memory.hpp"
#include "core/theme.hpp"
#include "core/painter.hpp"
#include "core/widget.hpp"
#include "primitives/label.hpp"
#include "primitives/fttext.hpp"
#include "primitives/button.hpp"
#include "primitives/textbox.hpp"
#include "primitives/checkbox.hpp"
#include "primitives/radio.hpp"
#include "primitives/slider.hpp"
#include "primitives/switch.hpp"
#include "primitives/progressbar.hpp"
#include "primitives/combobox.hpp"
#include "primitives/separator.hpp"
#include "primitives/image.hpp"
#include "editor/textedit.hpp"
#include "layout/layout.hpp"
#include "layout/vbox.hpp"
#include "layout/hbox.hpp"
#include "containers/scrollable.hpp"
#include "containers/scrollview.hpp"
#include "containers/gridview.hpp"
#include "containers/listview.hpp"
#include "containers/table.hpp"
#include "containers/treeview.hpp"
#include "containers/tab.hpp"
#include "containers/grid.hpp"
#include "containers/section.hpp"
#include "chrome/toolbar.hpp"
#include "chrome/menu.hpp"
#include "chrome/menubar.hpp"
#include "chrome/statusbar.hpp"
#include "dialog/dialog.hpp"
#include "dialog/promptdialog.hpp"
#include "window/window.hpp"
#include "services/clipboard.hpp"
#include "xml/xml.hpp"
#include "libui_xml.h"

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

// Phase A: tema dari mode × aksen (non-breaking; ui_theme_t tidak berubah).
void ui_window_set_theme_config(ui_window_t* win, const ui_theme_config_t* cfg) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (w) w->set_config(cfg);
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

// --- FtText: area teks FreeType (callback milik aplikasi; toolkit
// tetap FT-free — rasterisasi terjadi di sisi pemanggil) ---
ui_widget_t* ui_fttext_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::FtText(w, h));
}

void ui_fttext_set_draw(ui_widget_t* widget, ui_fttext_draw_cb cb, void* userdata) {
    ui::FtText* t = reinterpret_cast<ui::FtText*>(widget);
    if (t) t->set_draw(cb, userdata);
}

void ui_fttext_refresh(ui_widget_t* widget) {
    ui::FtText* t = reinterpret_cast<ui::FtText*>(widget);
    if (t) t->refresh();
}

// Browser viewport: klik kiri + koordinat (hit-test link). Tanpa ini app
// hanya dapat click_cb generik tanpa posisi.
void ui_fttext_set_click(ui_widget_t* widget, ui_pos_click_cb cb, void* userdata) {
    ui::FtText* t = reinterpret_cast<ui::FtText*>(widget);
    if (t) t->set_pos_click(cb, userdata);
}

ui_widget_t* ui_button_create(ui_window_t* win, const char* text) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Button(text));
}

void ui_button_set_click(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Widget*>(widget)->set_click(cb, userdata);
}

// Phase B: varian visual + enabled generik (aditif, non-breaking).
void ui_button_set_variant(ui_widget_t* widget, int variant) {
    ui::Button* b = reinterpret_cast<ui::Button*>(widget);
    if (b) b->set_variant(variant);
}

void ui_button_set_icon(ui_widget_t* widget, int icon) {
    ui::Button* b = reinterpret_cast<ui::Button*>(widget);
    if (b) b->set_icon(icon);
}

void ui_widget_set_enabled(ui_widget_t* widget, int enabled) {
    ui::Widget* w = reinterpret_cast<ui::Widget*>(widget);
    if (!w) return;
    w->set_enabled(enabled != 0);
    // Fokus yang sedang dipegang widget yang baru di-disable dilepas.
    if (!w->enabled && w->owner && w->owner->focused == w)
        w->owner->set_focus(0);
}

// Menu konteks: klik kanan + koordinat window-local (lihat include/libui.h).
void ui_widget_set_right_click(ui_widget_t* widget, ui_pos_click_cb cb, void* userdata) {
    ui::Widget* wid = reinterpret_cast<ui::Widget*>(widget);
    if (wid) wid->set_right_click(cb, userdata);
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

void ui_textbox_select_all(ui_widget_t* widget) {
    ui::TextBox* tb = reinterpret_cast<ui::TextBox*>(widget);
    if (tb) tb->select_all();
}

// Phase C: error state (state + rendering; validasi milik aplikasi).
void ui_textbox_set_error(ui_widget_t* widget, int is_error) {
    ui::TextBox* tb = reinterpret_cast<ui::TextBox*>(widget);
    if (tb) tb->set_error(is_error != 0);
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

void ui_checkbox_set_toggle(ui_widget_t* widget, ui_click_cb cb, void* userdata) {    ui::CheckBox* cbx = reinterpret_cast<ui::CheckBox*>(widget);
    cbx->toggle_cb = cb; cbx->toggle_data = userdata;
}

// --- Radio (Phase D) ---
ui_widget_t* ui_radio_create(ui_window_t* win, const char* label) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Radio(label));
}

void ui_radio_set_selected(ui_widget_t* widget, int selected) {
    ui::Radio* r = reinterpret_cast<ui::Radio*>(widget);
    if (r) r->set_selected(selected != 0);
}

void ui_radio_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::Radio* r = reinterpret_cast<ui::Radio*>(widget);
    if (r) r->set_change(cb, userdata);
}

ui_radio_group_t* ui_radio_group_create(void) {
    return reinterpret_cast<ui_radio_group_t*>(new ui::RadioGroup());
}

void ui_radio_group_destroy(ui_radio_group_t* group) {
    delete reinterpret_cast<ui::RadioGroup*>(group);
}

void ui_radio_set_group(ui_widget_t* widget, ui_radio_group_t* group) {
    ui::Radio* r = reinterpret_cast<ui::Radio*>(widget);
    if (r) r->set_group(reinterpret_cast<ui::RadioGroup*>(group));
}

ui_widget_t* ui_radio_get_selected(ui_radio_group_t* group) {
    ui::RadioGroup* g = reinterpret_cast<ui::RadioGroup*>(group);
    if (!g || !g->selected) return 0;
    return reinterpret_cast<ui_widget_t*>(g->selected);
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

// --- ComboBox (Phase D) ---
ui_widget_t* ui_combobox_create(ui_window_t* win, int width) {
    (void)win;
    ui::ComboBox* c = new ui::ComboBox();
    if (width > 0) c->w = width;
    return reinterpret_cast<ui_widget_t*>(c);
}

int ui_combobox_add_item(ui_widget_t* widget, const char* label) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    return c ? c->add_item(label) : -1;
}

int ui_combobox_remove_item(ui_widget_t* widget, int index) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    return (c && c->remove_item(index)) ? 1 : 0;
}

void ui_combobox_clear(ui_widget_t* widget) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    if (c) c->clear();
}

int ui_combobox_count(ui_widget_t* widget) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    return c ? c->count() : 0;
}

int ui_combobox_selected(ui_widget_t* widget) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    return c ? c->selected : -1;
}

void ui_combobox_set_selected(ui_widget_t* widget, int index) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    if (c) c->set_selected(index);
}

void ui_combobox_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::ComboBox* c = reinterpret_cast<ui::ComboBox*>(widget);
    if (c) c->set_change(cb, userdata);
}

// --- Separator (Phase D) ---
ui_widget_t* ui_separator_create(ui_window_t* win, int orientation) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(
        new ui::Separator(orientation == UI_SEP_VERTICAL));
}

// --- Section: bagian berjudul dalam halaman ---
// Primitif yang membuat halaman pengaturan/form terbaca tanpa kartu: judul
// memakai peran tipografi `section` + garis tipis, isi disusun di bawahnya.
ui_widget_t* ui_section_create(ui_window_t* win, const char* title, int spacing) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Section(title, spacing));
}
void ui_section_set_title(ui_widget_t* widget, const char* title) {
    ui::Section* s = reinterpret_cast<ui::Section*>(widget);
    if (s) s->set_title(title);
}

// --- Tooltip (Phase D) ---
void ui_widget_set_tooltip(ui_widget_t* widget, const char* text) {
    ui::Widget* w = reinterpret_cast<ui::Widget*>(widget);
    if (w) w->set_tooltip(text);
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

// Phase 11: skala otomatis agar seluruh gambar masuk view (return persen).
int ui_image_set_fit(ui_widget_t* widget, int view_w, int view_h) {
    return reinterpret_cast<ui::Image*>(widget)->set_fit(view_w, view_h);
}

void ui_image_natural_size(ui_widget_t* widget, int* out_w, int* out_h) {
    ui::Image* im = reinterpret_cast<ui::Image*>(widget);
    if (out_w) *out_w = im->iw;
    if (out_h) *out_h = im->ih;
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
    te->scroll_top = 0;
    te->sel_anchor = -1;
    // Memuat berkas/membuka dokumen baru = titik awal baru: historis undo lama
    // menunjuk isi dokumen sebelumnya, jadi dibuang (kalau tidak, undo bisa
    // mencampur dua dokumen).
    te->ops_clear();
    te->ensure_cursor_visible();
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

void ui_textedit_set_prompt_style(ui_widget_t* widget, const char* prefix, color_t color) {
    reinterpret_cast<ui::TextEdit*>(widget)->set_prompt_style(prefix, color);
}

void ui_textedit_clear(ui_widget_t* widget) {
    reinterpret_cast<ui::TextEdit*>(widget)->clear();
}

// --- Phase 11: API editor (dipakai notepad) ------------------------------

void ui_textedit_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    te->change_cb = cb;
    te->change_data = userdata;
}

void ui_textedit_enable_undo(ui_widget_t* widget, int ops) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    te->ops_clear();
    te->undo_on = ops > 0;
}

int ui_textedit_undo(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->op_apply(-1) ? 1 : 0;
}

int ui_textedit_redo(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->op_apply(+1) ? 1 : 0;
}

int ui_textedit_can_undo(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    return te->op_pos > 0 ? 1 : 0;
}

int ui_textedit_can_redo(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    return te->op_pos < te->n_ops ? 1 : 0;
}

void ui_textedit_sel_all(ui_widget_t* widget) {
    reinterpret_cast<ui::TextEdit*>(widget)->sel_all();
}

int ui_textedit_has_sel(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->has_sel() ? 1 : 0;
}

int ui_textedit_sel_length(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    return te->has_sel() ? (te->sel_hi() - te->sel_lo()) : 0;
}

int ui_textedit_copy(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (!te->has_sel()) return 0;
    te->copy_sel();
    return 1;
}

int ui_textedit_cut(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (te->readonly || !te->has_sel()) return 0;
    te->cut_sel();
    return 1;
}

int ui_textedit_paste(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (te->readonly) return 0;
    const char* s = ui_clipboard_get_text();
    if (!s || !s[0]) return 0;
    te->paste_clip();
    return 1;
}

int ui_textedit_delete_sel(ui_widget_t* widget) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (te->readonly || !te->has_sel()) return 0;
    te->sel_delete();
    return 1;
}

int ui_textedit_insert(ui_widget_t* widget, const char* text) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (te->readonly || !text || !text[0]) return 0;
    te->insert_str(text);
    return 1;
}

int ui_textedit_length(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->len;
}

void ui_textedit_cursor(ui_widget_t* widget, int* line, int* col) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (line) *line = te->line_at(te->cur) + 1;          // 1-based, gaya Notepad
    if (col)  *col  = te->cur - te->line_start(te->cur) + 1;
}

void ui_textedit_set_cursor(ui_widget_t* widget, int idx) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    te->move_to(idx, false);
}

int ui_textedit_line_count(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->total_lines();
}

int ui_textedit_line_start_idx(ui_widget_t* widget, int line1) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (line1 < 1) line1 = 1;
    int i = 0, n = 1;
    while (n < line1 && i < te->len) {
        if (te->text[i] == '\n') n++;
        i++;
    }
    return i;
}

void ui_textedit_select(ui_widget_t* widget, int from, int to) {
    reinterpret_cast<ui::TextEdit*>(widget)->sel_set(from, to);
}

int ui_textedit_find(ui_widget_t* widget, const char* needle, int from, int ignore_case) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (!needle || !needle[0]) return -1;
    int n = 0; while (needle[n]) n++;
    if (from < 0) from = 0;
    for (int i = from; i + n <= te->len; i++) {
        int k = 0;
        for (; k < n; k++) {
            char a = te->text[i + k], b = needle[k];
            if (ignore_case) {
                if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            }
            if (a != b) break;
        }
        if (k == n) return i;
    }
    return -1;
}

int ui_textedit_replace_all(ui_widget_t* widget, const char* needle, const char* with) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    if (te->readonly || !needle || !needle[0]) return 0;
    int n = 0; while (needle[n]) n++;
    int count = 0;
    for (int i = 0; i + n <= te->len; ) {
        int k = 0;
        for (; k < n; k++) if (te->text[i + k] != needle[k]) break;
        if (k == n) {
            int wl = 0; if (with) while (with[wl]) wl++;
            te->apply_replace(i, n, with, wl, true);
            count++;
            i += wl > 0 ? wl : 0;
            if (wl == 0 && i > te->len) break;
        } else i++;
    }
    return count;
}

void ui_textedit_set_wrap(ui_widget_t* widget, int on) {
    ui::TextEdit* te = reinterpret_cast<ui::TextEdit*>(widget);
    te->wrap = on != 0;
    te->clamp_scroll();
    te->mark_dirty();
}

int ui_textedit_wrap(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->wrap ? 1 : 0;
}

int ui_textedit_scroll_rows(ui_widget_t* widget) {
    return reinterpret_cast<ui::TextEdit*>(widget)->disp_rows();
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

// Phase 11: mode "lihat gambar" (scroll 2 arah + center + anchor zoom).
void ui_scrollview_set_pan(ui_widget_t* widget, int on) {
    reinterpret_cast<ui::ScrollView*>(widget)->set_pan(on);
}

// Browser viewport: posisi window-local widget (hit-test link butuh origin
// FtText yang digeser ScrollView; tidak ada getter sebelum ini).
void ui_widget_pos(ui_widget_t* widget, int* out_x, int* out_y) {
    ui::Widget* w = reinterpret_cast<ui::Widget*>(widget);
    if (out_x) *out_x = w ? w->x : 0;
    if (out_y) *out_y = w ? w->y : 0;
}

void ui_scrollview_set_child(ui_widget_t* widget, ui_widget_t* child) {
    reinterpret_cast<ui::ScrollView*>(widget)->set_child(reinterpret_cast<ui::Widget*>(child));
}

// Browser viewport: baca/atur offset scroll vertikal (hit-test link butuh
// offset; keyboard PgUp/PgDn/Home/End lewat shortcut window + setter ini).
// Roda mouse + scrollbar drag tetap jalur Scrollable bawaan (tanpa ABI).
int ui_scrollview_scroll(ui_widget_t* widget) {
    ui::ScrollView* s = reinterpret_cast<ui::ScrollView*>(widget);
    return s ? s->scroll : 0;
}

void ui_scrollview_set_scroll(ui_widget_t* widget, int pos) {
    ui::ScrollView* s = reinterpret_cast<ui::ScrollView*>(widget);
    if (!s) return;
    if (pos < 0) pos = 0;
    if (pos > s->scroll_max) pos = s->scroll_max;
    if (pos != s->scroll) {
        s->scroll = pos;
        s->mark_dirty();
    }
}

// --- GridView ---
ui_widget_t* ui_gridview_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::GridView(w, h));
}

void ui_gridview_set_cell(ui_widget_t* widget, int cell_w, int cell_h, int thumb_box) {
    ui::GridView* g = reinterpret_cast<ui::GridView*>(widget);
    g->set_cell_size(cell_w, cell_h);
    g->set_thumb_box(thumb_box);
}

void ui_gridview_set_empty_text(ui_widget_t* widget, const char* text) {
    reinterpret_cast<ui::GridView*>(widget)->set_empty_text(text);
}

int ui_gridview_add_item(ui_widget_t* widget, const char* name) {
    return reinterpret_cast<ui::GridView*>(widget)->add_item(name);
}

void ui_gridview_clear(ui_widget_t* widget) {
    reinterpret_cast<ui::GridView*>(widget)->clear();
}

void ui_gridview_set_thumb(ui_widget_t* widget, int index, const uint32_t* px, int w, int h) {
    reinterpret_cast<ui::GridView*>(widget)->set_thumb(index, px, w, h);
}

void ui_gridview_set_placeholder(ui_widget_t* widget, int index, int state) {
    reinterpret_cast<ui::GridView*>(widget)->set_placeholder(index, state);
}

int ui_gridview_count(ui_widget_t* widget) {
    return reinterpret_cast<ui::GridView*>(widget)->n;
}

int ui_gridview_selected(ui_widget_t* widget) {
    return reinterpret_cast<ui::GridView*>(widget)->selected;
}

void ui_gridview_set_selected(ui_widget_t* widget, int index) {
    reinterpret_cast<ui::GridView*>(widget)->set_selected_cell(index, false);
}

void ui_gridview_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::GridView*>(widget)->set_change(cb, userdata);
}

void ui_gridview_set_activate(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::GridView*>(widget)->set_activate(cb, userdata);
}

void ui_gridview_ensure_visible(ui_widget_t* widget, int index) {
    reinterpret_cast<ui::GridView*>(widget)->ensure_visible(index);
}

int ui_gridview_cell_at(ui_widget_t* widget, int x, int y) {
    return reinterpret_cast<ui::GridView*>(widget)->cell_at(x, y);
}

int ui_gridview_visible_range(ui_widget_t* widget, int* first, int* last) {
    ui::GridView* g = reinterpret_cast<ui::GridView*>(widget);
    if (g->n <= 0) return 0;
    int f = 0, l = -1;
    g->visible_range(f, l);
    if (f > l) return 0;
    if (first) *first = f;
    if (last) *last = l;
    return l - f + 1;
}

// --- ListView ---
ui_widget_t* ui_listview_create(ui_window_t* win, int w, int h) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::ListView(w, h));
}

void ui_listview_add_item(ui_widget_t* widget, const char* label) {
    reinterpret_cast<ui::ListView*>(widget)->add_item(label);
}

// Baris daftar KAYA (aditif): judul + deskripsi + ikon depan + chevron.
// Ini yang membuat baris pengaturan/navigasi bisa dibangun dari primitif
// daftar alih-alih setiap app merakit sendiri dari label + tombol.
// Return index, atau -1 bila daftar penuh.
int ui_listview_add_row(ui_widget_t* widget, const char* title,
                        const char* description, int icon, int chevron) {
    ui::ListView* lv = reinterpret_cast<ui::ListView*>(widget);
    if (!lv) return -1;
    return lv->add_row(title, description, icon, chevron != 0);
}
void ui_listview_set_row_icon(ui_widget_t* widget, int index, int icon) {
    ui::ListView* lv = reinterpret_cast<ui::ListView*>(widget);
    if (lv) lv->set_row_icon(index, icon);
}
void ui_listview_set_row_trailing(ui_widget_t* widget, int index, int icon) {
    ui::ListView* lv = reinterpret_cast<ui::ListView*>(widget);
    if (lv) lv->set_row_trailing(index, icon);
}
void ui_listview_set_row_disabled(ui_widget_t* widget, int index, int disabled) {
    ui::ListView* lv = reinterpret_cast<ui::ListView*>(widget);
    if (lv) lv->set_row_disabled(index, disabled != 0);
}

int ui_listview_selected(ui_widget_t* widget) {
    return reinterpret_cast<ui::ListView*>(widget)->selected;
}

// Phase 11: pilih baris dari kode (dipakai viewer saat dibuka dari Explorer).
void ui_listview_set_selected(ui_widget_t* widget, int index) {
    reinterpret_cast<ui::ListView*>(widget)->set_selected(index);
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

void ui_table_set_empty_text(ui_widget_t* widget, const char* text) {
    reinterpret_cast<ui::Table*>(widget)->set_empty_text(text);
}

int ui_table_row_at(ui_widget_t* widget, int y) {
    return reinterpret_cast<ui::Table*>(widget)->row_at(y);
}

void ui_table_set_selected(ui_widget_t* widget, int index) {
    reinterpret_cast<ui::Table*>(widget)->set_selected(index);
}

void ui_table_set_row_icon(ui_widget_t* widget, int row, const uint32_t* px, int w, int h) {
    reinterpret_cast<ui::Table*>(widget)->set_row_icon(row, px, w, h);
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

// Phase C: nonaktifkan judul tab tanpa mengubah arsitektur tab.
void ui_tab_set_enabled(ui_widget_t* widget, int index, int enabled) {
    ui::Tab* t = reinterpret_cast<ui::Tab*>(widget);
    if (t) t->set_tab_enabled(index, enabled);
}

// --- Grid (Phase D) ---
ui_widget_t* ui_grid_create(ui_window_t* win, int rows, int cols, int gap) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Grid(rows, cols, gap));
}

int ui_grid_put(ui_widget_t* grid, ui_widget_t* child, int row, int col) {
    ui::Grid* g = reinterpret_cast<ui::Grid*>(grid);
    ui::Widget* c = reinterpret_cast<ui::Widget*>(child);
    if (!g || !c) return 0;
    return g->put(c, row, col, 1, 1);
}

int ui_grid_put_span(ui_widget_t* grid, ui_widget_t* child, int row, int col,
                     int row_span, int col_span) {
    ui::Grid* g = reinterpret_cast<ui::Grid*>(grid);
    ui::Widget* c = reinterpret_cast<ui::Widget*>(child);
    if (!g || !c) return 0;
    return g->put(c, row, col, row_span, col_span);
}

void ui_grid_set_col(ui_widget_t* grid, int col, int mode, int px) {
    ui::Grid* g = reinterpret_cast<ui::Grid*>(grid);
    if (g) g->set_col(col, mode, px);
}

void ui_grid_set_row(ui_widget_t* grid, int row, int mode, int px) {
    ui::Grid* g = reinterpret_cast<ui::Grid*>(grid);
    if (g) g->set_row(row, mode, px);
}

void ui_grid_set_padding(ui_widget_t* grid, ui_padding_t pad) {
    ui::Grid* g = reinterpret_cast<ui::Grid*>(grid);
    if (g) g->set_padding(pad.left, pad.top, pad.right, pad.bottom);
}

void ui_grid_set_align(ui_widget_t* grid, int align) {
    ui::Grid* g = reinterpret_cast<ui::Grid*>(grid);
    if (g) g->set_align(align);
}

// --- MenuBar + Menu ---
// Menu popup mandiri (menu konteks). Dimiliki pemanggil: tidak ada parent
// layout, jadi umurnya sampai proses selesai (app cukup membuat sekali).
ui_widget_t* ui_menu_create(ui_window_t* win) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    return reinterpret_cast<ui_widget_t*>(new ui::Menu(w));
}

void ui_window_popup_menu(ui_window_t* win, ui_widget_t* menu, int x, int y) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (!w || !menu) return;
    w->open_popup(reinterpret_cast<ui::Widget*>(menu), x, y);
}

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

// Phase 11: item dengan kolom accelerator rata kanan ("Simpan     Ctrl+S").
void ui_menu_add_item_acc(ui_widget_t* menu, const char* label, const char* acc,
                          ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Menu*>(menu)->add_item_acc(label, acc, cb, userdata);
}

// Garis pemisah antar kelompok item (gaya menu Windows).
void ui_menu_add_sep(ui_widget_t* menu) {
    reinterpret_cast<ui::Menu*>(menu)->add_sep();
}

void ui_menu_set_checked(ui_widget_t* menu, int index, int checked) {
    reinterpret_cast<ui::Menu*>(menu)->set_checked(index, checked);
}

// Item disabled digambar redup & tidak bereaksi terhadap klik (gaya Windows:
// Undo/Redo kelabu saat tidak ada historis).
void ui_menu_set_enabled(ui_widget_t* menu, int index, int enabled) {
    reinterpret_cast<ui::Menu*>(menu)->set_enabled(index, enabled);
}

// --- StatusBar ---
ui_widget_t* ui_statusbar_create(ui_window_t* win) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::StatusBar());
}

void ui_statusbar_set_text(ui_widget_t* widget, const char* left, const char* right) {
    reinterpret_cast<ui::StatusBar*>(widget)->set_text(left, right);
}

// --- Toolbar ---
ui_widget_t* ui_toolbar_create(ui_window_t* win) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    return reinterpret_cast<ui_widget_t*>(new ui::Toolbar((int)w->gw->width));
}

void ui_toolbar_add_button(ui_widget_t* bar, const char* label, ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Toolbar*>(bar)->add_button(label, cb, userdata);
}

// Tombol toolbar ber-ikon (aditif). icon = UI_ICON_*; 0 = teks saja.
void ui_toolbar_add_button_icon(ui_widget_t* bar, const char* label, int icon,
                                ui_click_cb cb, void* userdata) {
    reinterpret_cast<ui::Toolbar*>(bar)->add_button_icon(label, icon, cb, userdata);
}

// --- Switch (toggle on/off) ---
// Checkbox = bagian dari form (dikirim bersama tombol simpan).
// Switch  = pengaturan yang berlaku SEGERA (nyala/mati tanpa tombol).
ui_widget_t* ui_switch_create(ui_window_t* win, const char* label) {
    (void)win;
    return reinterpret_cast<ui_widget_t*>(new ui::Switch(label));
}
void ui_switch_set_on(ui_widget_t* widget, int on) {
    ui::Switch* s = reinterpret_cast<ui::Switch*>(widget);
    if (s) s->set_on(on != 0, false);   // programatik = diam
}
int ui_switch_on(ui_widget_t* widget) {
    ui::Switch* s = reinterpret_cast<ui::Switch*>(widget);
    return s ? (s->is_on() ? 1 : 0) : 0;
}
void ui_switch_set_change(ui_widget_t* widget, ui_click_cb cb, void* userdata) {
    ui::Switch* s = reinterpret_cast<ui::Switch*>(widget);
    if (s) s->set_change(cb, userdata);
}

// --- Ikon (pengukuran) ---
// Ukuran optik standar ikon pada peran tertentu, supaya app bisa menyusun
// layout yang sejajar dengan widget toolkit.
int ui_icon_size(int role) {
    const ui::Metrics m;
    switch (role) {
    case UI_ICON_SIZE_SM: return m.icon_sm;
    case UI_ICON_SIZE_LG: return m.icon_lg;
    case UI_ICON_SIZE_XL: return m.icon_xl;
    case UI_ICON_SIZE_MD:
    default: return m.icon_md;
    }
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

// --- Prompt (dialog + kolom input teks) ---
// Dipakai sebagai pengganti dialog berkas: File > Buka / Simpan Sebagai.
// cb(userdata, text): text = isi kolom saat OK, atau 0 bila dibatalkan (ESC /
// tombol Batal). Pointer text hanya valid selama callback.
void ui_prompt_show(ui_window_t* win, const char* title, const char* text,
                    const char* initial, ui_prompt_cb cb, void* userdata) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    w->open_dialog(new ui::PromptDialog(w, title, text, initial, cb, userdata));
}

// --- Tampil/sembunyi widget (layout melewati anak yang tersembunyi) ---
void ui_widget_set_visible(ui_widget_t* widget, int visible) {
    ui::Widget* wid = reinterpret_cast<ui::Widget*>(widget);
    if (wid) wid->set_visible(visible != 0);
}

// --- ESC global (override perilaku default "ESC = tutup window") ---
void ui_window_set_escape(ui_window_t* win, ui_click_cb cb, void* userdata) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    w->escape_cb = cb;
    w->escape_data = userdata;
}

// Hook tombol aplikasi (dipanggil hanya saat tidak ada widget fokus keyboard).
void ui_window_set_key(ui_window_t* win, ui_key_cb cb, void* userdata) {
    ui::Window* w = reinterpret_cast<ui::Window*>(win);
    if (w) { w->key_cb = cb; w->key_data = userdata; }
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

// --- XML deklaratif (Phase E) ---
// Satu-satunya jembatan C untuk lapisan xml (cast tetap hanya di file abi).
static void xml_copy_error(ui_xml_error_t* out, const ui::xml::Error* e) {
    if (!out || !e) return;
    out->code = e->code;
    out->line = e->line;
    out->column = e->column;
    int i = 0;
    while (i < 47 && e->element[i]) { out->element[i] = e->element[i]; i++; }
    out->element[i] = '\0';
    i = 0;
    while (i < 39 && e->attribute[i]) { out->attribute[i] = e->attribute[i]; i++; }
    out->attribute[i] = '\0';
}

ui_xml_doc_t* ui_xml_parse(const char* data, unsigned size, ui_xml_error_t* err) {
    ui::xml::Error e;
    ui::xml::Document* d = ui::xml::parse(data, size, &e);
    xml_copy_error(err, &e);
    return reinterpret_cast<ui_xml_doc_t*>(d);
}

void ui_xml_doc_destroy(ui_xml_doc_t* doc) {
    ui::xml::free_doc(reinterpret_cast<ui::xml::Document*>(doc));
}

ui_xml_ctx_t* ui_xml_ctx_create(ui_window_t* win) {
    if (!win) return 0;
    ui::xml::Context* c = new ui::xml::Context;
    if (!c) return 0;
    c->win = reinterpret_cast<ui::Window*>(win);
    c->nids = 0;
    c->ngroups = 0;
    c->nroots = 0;
    c->committed = false;
    c->has_theme = false;
    c->theme_mode = 0;
    c->theme_accent = 0;
    c->theme_custom = 0;
    return reinterpret_cast<ui_xml_ctx_t*>(c);
}

void ui_xml_ctx_destroy(ui_xml_ctx_t* ctx) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    if (!c) return;
    if (!c->committed) {
        ui::xml::rollback(c);   // roots detached + grup dibuang
    } else {
        // Roots milik window; grup dilepas aman (dtor grup melepas anggota).
        for (int i = 0; i < c->ngroups; i++) delete c->groups[i].g;
    }
    delete c;
}

int ui_xml_inflate(ui_xml_ctx_t* ctx, const ui_xml_doc_t* doc, ui_xml_error_t* err) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    const ui::xml::Document* d = reinterpret_cast<const ui::xml::Document*>(doc);
    if (!c || !d) {
        if (err) {
            err->code = UI_XML_EMPTY; err->line = 0; err->column = 0;
            err->element[0] = '\0'; err->attribute[0] = '\0';
        }
        return 0;
    }
    ui::xml::Error e;
    if (!ui::xml::inflate(c, d, &e)) {
        xml_copy_error(err, &e);
        return 0;   // inflate() sudah rollback: window tak tersentuh
    }
    ui::xml::commit(c);
    if (err) {
        err->code = UI_XML_OK; err->line = 0; err->column = 0;
        err->element[0] = '\0'; err->attribute[0] = '\0';
    }
    return 1;
}

// Inflasi detached: seperti inflate, tanpa commit. Caller mengambil roots
// (root_count/root_at), me-parenting sendiri, lalu release (wajib sebelum
// destroy, kalau tidak roots ikut terbuang).
int ui_xml_inflate_detached(ui_xml_ctx_t* ctx, const ui_xml_doc_t* doc,
                            ui_xml_error_t* err) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    const ui::xml::Document* d = reinterpret_cast<const ui::xml::Document*>(doc);
    if (!c || !d) {
        if (err) {
            err->code = UI_XML_EMPTY; err->line = 0; err->column = 0;
            err->element[0] = '\0'; err->attribute[0] = '\0';
        }
        return 0;
    }
    ui::xml::Error e;
    if (!ui::xml::inflate(c, d, &e)) {
        xml_copy_error(err, &e);
        return 0;
    }
    if (err) {
        err->code = UI_XML_OK; err->line = 0; err->column = 0;
        err->element[0] = '\0'; err->attribute[0] = '\0';
    }
    return 1;
}

int ui_xml_root_count(ui_xml_ctx_t* ctx) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    if (!c) return 0;
    return c->nroots;
}

ui_widget_t* ui_xml_root_at(ui_xml_ctx_t* ctx, int index) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    if (!c || index < 0 || index >= c->nroots) return 0;
    return reinterpret_cast<ui_widget_t*>(c->roots[index]);
}

void ui_xml_release(ui_xml_ctx_t* ctx) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    if (!c) return;
    // Roots jadi milik caller (sudah/akan di-parenting sendiri).
    // Grup tetap milik konteks (dihapus aman saat destroy via two-way
    // cleanup: dtor grup melepas anggota hidup, dtor radio keluar grup).
    c->nroots = 0;
}

ui_widget_t* ui_xml_find(ui_xml_ctx_t* ctx, const char* id) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    if (!c || !id) return 0;
    return reinterpret_cast<ui_widget_t*>(ui::xml::find(c, id));
}

// Binding callback native bertipe (tanpa kode di XML).
// CLICK: button->click; checkbox/radio/combo/slider/list->change.
// CHANGE: checkbox->toggle; radio/combo/slider/list->change; textbox->enter.
// Pasangan lain (label/separator/progress/image/kontainer/button-CHANGE) = 0.
int ui_xml_bind(ui_xml_ctx_t* ctx, const char* id, int event,
                ui_click_cb cb, void* userdata) {
    ui::xml::Context* c = reinterpret_cast<ui::xml::Context*>(ctx);
    if (!c || !id || !cb) return 0;
    ui::Widget* w = 0;
    int kind = 0;
    for (int i = 0; i < c->nids; i++) {
        const char* a = c->ids[i].id;
        int k = 0;
        while (a[k] && a[k] == id[k]) k++;
        if (a[k] == id[k]) { w = c->ids[i].w; kind = c->ids[i].kind; break; }
    }
    if (!w) return 0;
    if (event == UI_XML_ON_CLICK) {
        switch (kind) {
        case ui::xml::K_BUTTON:
            w->click_cb = cb; w->userdata = userdata;
            return 1;
        case ui::xml::K_CHECKBOX:
            reinterpret_cast<ui::CheckBox*>(w)->toggle_cb = cb;
            reinterpret_cast<ui::CheckBox*>(w)->toggle_data = userdata;
            return 1;
        case ui::xml::K_RADIO:
            reinterpret_cast<ui::Radio*>(w)->set_change(cb, userdata);
            return 1;
        case ui::xml::K_COMBO:
            reinterpret_cast<ui::ComboBox*>(w)->set_change(cb, userdata);
            return 1;
        case ui::xml::K_SLIDER:
            reinterpret_cast<ui::Slider*>(w)->change_cb = cb;
            reinterpret_cast<ui::Slider*>(w)->change_data = userdata;
            return 1;
        case ui::xml::K_LISTVIEW:
            reinterpret_cast<ui::ListView*>(w)->set_change(cb, userdata);
            return 1;
        default:
            return 0;
        }
    }
    if (event == UI_XML_ON_CHANGE) {
        switch (kind) {
        case ui::xml::K_CHECKBOX:
            reinterpret_cast<ui::CheckBox*>(w)->toggle_cb = cb;
            reinterpret_cast<ui::CheckBox*>(w)->toggle_data = userdata;
            return 1;
        case ui::xml::K_RADIO:
            reinterpret_cast<ui::Radio*>(w)->set_change(cb, userdata);
            return 1;
        case ui::xml::K_COMBO:
            reinterpret_cast<ui::ComboBox*>(w)->set_change(cb, userdata);
            return 1;
        case ui::xml::K_SLIDER:
            reinterpret_cast<ui::Slider*>(w)->change_cb = cb;
            reinterpret_cast<ui::Slider*>(w)->change_data = userdata;
            return 1;
        case ui::xml::K_LISTVIEW:
            reinterpret_cast<ui::ListView*>(w)->set_change(cb, userdata);
            return 1;
        case ui::xml::K_TEXTBOX:
            reinterpret_cast<ui::TextBox*>(w)->enter_cb = cb;
            reinterpret_cast<ui::TextBox*>(w)->enter_data = userdata;
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

} // extern "C"
