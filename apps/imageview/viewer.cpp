// apps/imageview/viewer.cpp — implementasi UI ImageView.
//
// Tata letak (atas → bawah), memakai bar/container toolkit yang sudah ada:
//   [MenuBar: File | View]
//   [Toolbar: -  +  Fit  1:1  Prev  Next]
//   [ScrollView(pan) → ui::Image]      ← viewport gambar
//   [StatusBar: nama  1920x1080  PNG  2.4 MB | Fit 42%]
//
// Zoom/pan TIDAK mengalokasikan bitmap per level: ui_image_set_scale() hanya
// menghitung ulang ukuran target, dan Painter mem-blit nearest-neighbor dengan
// klip + damage rect (satu jalur damage seluruh toolkit).
#include "viewer.hpp"

namespace iv {

namespace {

// Ukuran window + tata letak (bar: MenuBar 24 + Toolbar 28, root VBox margin 8).
const int WIN_W      = 860;
const int WIN_H      = 620;
const int MARGIN     = 8;
const int SPACING    = 6;
const int MENUBAR_H  = 24;
const int TOOLBAR_H  = 28;
const int STATUS_H   = 20;
const int SCROLL_W   = WIN_W - MARGIN * 2;
// Sisa tinggi setelah margin atas, dua bar, statusbar, spasi, margin bawah.
const int SCROLL_H   = WIN_H - (MARGIN + MENUBAR_H + TOOLBAR_H) - STATUS_H
                       - SPACING - MARGIN;
// Ruang yang dipesan untuk scrollbar agar fit TIDAK memunculkan bar
// (kalau bar muncul saat fit, area mengecil dan fit berubah lagi).
const int FIT_PAD    = 6;

const int ZOOM_MIN   = 10;    // selaras ui_image_set_scale (10..400)
const int ZOOM_MAX   = 400;
const int ZOOM_STEP  = 10;

const char* const kErrButtons[] = { "Close" };

Viewer* self(void* ud) { return static_cast<Viewer*>(ud); }

// --- Thunk callback gaya C (toolkit memanggil void(*)(void*)) ---
void cb_zoom_in(void* ud)  { self(ud)->cmd_zoom_in(); }
void cb_zoom_out(void* ud) { self(ud)->cmd_zoom_out(); }
void cb_fit(void* ud)      { self(ud)->cmd_fit(); }
void cb_actual(void* ud)   { self(ud)->cmd_actual(); }
void cb_next(void* ud)     { self(ud)->cmd_next(); }
void cb_prev(void* ud)     { self(ud)->cmd_prev(); }
void cb_close(void* ud)    { self(ud)->cmd_close(); }
// Dialog error: tombol apa pun (atau ESC) menutup viewer.
void cb_err_dialog(void* ud, int index) {
    (void)index;
    self(ud)->cmd_close();
}

}  // namespace

Viewer::Viewer()
    : win_(0), image_(0), scroll_(0), status_(0),
      zoom_(100), fit_(true), failed_(true) {}

// ------------------------------------------------------------
// Konstruksi UI
// ------------------------------------------------------------
void Viewer::build_ui() {
    win_ = ui_window_create(WIN_W, WIN_H);
    if (!win_) return;      // OOM: run() memeriksa dan keluar dengan rapi
    ui_window_set_title(win_, "Image Viewer");
    // Tema default toolkit (charcoal) — sama dengan aplikasi GUI lain; tidak ada
    // tema kustom supaya ImageView terlihat native, bukan app terpisah gaya.

    // --- MenuBar ---
    ui_widget_t* mb = ui_menubar_create(win_);
    ui_widget_t* mf = ui_menubar_add_menu(mb, "File");
    ui_menu_add_item_acc(mf, "Next image", "N", cb_next, this);
    ui_menu_add_item_acc(mf, "Previous image", "P", cb_prev, this);
    ui_menu_add_sep(mf);
    ui_menu_add_item_acc(mf, "Close", "Esc", cb_close, this);

    ui_widget_t* mv = ui_menubar_add_menu(mb, "View");
    ui_menu_add_item_acc(mv, "Zoom in", "+", cb_zoom_in, this);
    ui_menu_add_item_acc(mv, "Zoom out", "-", cb_zoom_out, this);
    ui_menu_add_sep(mv);
    ui_menu_add_item_acc(mv, "Fit to window", "F", cb_fit, this);
    ui_menu_add_item_acc(mv, "Actual size", "1", cb_actual, this);
    ui_window_add_bar(win_, mb);

    // --- Toolbar ---
    ui_widget_t* tb = ui_toolbar_create(win_);
    ui_toolbar_add_button(tb, "-", cb_zoom_out, this);
    ui_toolbar_add_button(tb, "+", cb_zoom_in, this);
    ui_toolbar_add_button(tb, "Fit", cb_fit, this);
    ui_toolbar_add_button(tb, "1:1", cb_actual, this);
    ui_toolbar_add_button(tb, "Prev", cb_prev, this);
    ui_toolbar_add_button(tb, "Next", cb_next, this);
    ui_window_add_bar(win_, tb);

    // --- Viewport: ScrollView pan + Image ---
    // pan: scroll dua arah, anak di tengah saat lebih kecil dari view, titik
    // tengah view dipertahankan saat zoom, drag untuk menggeser, panah untuk
    // menggeser (mode pan = focusable).
    scroll_ = ui_scrollview_create(win_, SCROLL_W, SCROLL_H);
    ui_scrollview_set_pan(scroll_, 1);
    image_ = ui_image_create(win_, "", 0, 0);
    ui_scrollview_set_child(scroll_, image_);

    ui_widget_t* box = ui_vbox_create(win_, SPACING);
    ui_layout_add(box, scroll_);
    status_ = ui_statusbar_create(win_);
    ui_widget_set_size(status_, SCROLL_W, STATUS_H);
    ui_layout_add(box, status_);
    ui_window_add(win_, box);

    // --- Shortcut keyboard ---
    // '+'/'_' butuh Shift di keyboard US (kernel mengirim ASCII hasil
    // terjemahan + bit KEY_MOD_SHIFT), jadi entri shift didaftarkan terpisah;
    // numpad '+'/'-' datang tanpa modifier.
    ui_window_add_shortcut(win_, 0, '+', cb_zoom_in, this);
    ui_window_add_shortcut(win_, 0, '=', cb_zoom_in, this);
    ui_window_add_shortcut(win_, KEY_MOD_SHIFT, '+', cb_zoom_in, this);
    ui_window_add_shortcut(win_, 0, '-', cb_zoom_out, this);
    ui_window_add_shortcut(win_, KEY_MOD_SHIFT, '_', cb_zoom_out, this);
    ui_window_add_shortcut(win_, 0, 'f', cb_fit, this);
    ui_window_add_shortcut(win_, 0, 'F', cb_fit, this);
    ui_window_add_shortcut(win_, 0, '1', cb_actual, this);
    ui_window_add_shortcut(win_, 0, 'n', cb_next, this);
    ui_window_add_shortcut(win_, 0, 'N', cb_next, this);
    ui_window_add_shortcut(win_, 0, 'p', cb_prev, this);
    ui_window_add_shortcut(win_, 0, 'P', cb_prev, this);
}

// ------------------------------------------------------------
// Muat gambar
// ------------------------------------------------------------
void Viewer::apply_fit() {
    if (failed_) return;
    const int p = ui_image_set_fit(image_, SCROLL_W - FIT_PAD, SCROLL_H - FIT_PAD);
    if (p > 0) { zoom_ = p; fit_ = true; }
}

void Viewer::set_zoom(int percent) {
    if (failed_) return;
    if (percent < ZOOM_MIN) percent = ZOOM_MIN;
    if (percent > ZOOM_MAX) percent = ZOOM_MAX;
    zoom_ = percent;
    fit_ = false;
    ui_image_set_scale(image_, zoom_);
    refresh_status();
}

void Viewer::load_path(const char* path) {
    if (!doc_.open(path)) {
        // Berkas hilang / format belum didukung / header rusak. Window tetap
        // hidup dengan keadaan error yang jelas (status bar + dialog), bukan
        // panic/exit senyap. Viewport dikosongkan supaya gambar LAMA tidak
        // tertinggal di layar seolah masih berkas yang sedang dibuka.
        failed_ = true;
        zoom_ = 100;
        fit_ = false;
        ui_image_set_file(image_, "");
        ui_window_set_title(win_, "Image Viewer");
        refresh_status();
        show_error_dialog();
        return;
    }

    failed_ = false;
    ui_window_set_title(win_, doc_.name);
    // Muat piksel: toolkit mengambil satu buffer penuh-resolusi (buffer lama
    // dibebaskan di sini juga — tidak ada kebocoran saat Next/Previous).
    ui_image_set_file(image_, doc_.path);
    int iw = 0, ih = 0;
    ui_image_natural_size(image_, &iw, &ih);
    if (iw <= 0 || ih <= 0) {
        // Header lolos probe tapi isi berkas tidak bisa didekode (rusak/OOM).
        doc_.mark_decode_failed();
        failed_ = true;
        refresh_status();
        show_error_dialog();
        return;
    }
    // Metadata dari probe header tetap dipakai bila ada; kalau probe tak
    // memberi dimensi sementara decode berhasil, isi dari hasil decode.
    if (doc_.width <= 0 || doc_.height <= 0) {
        doc_.width = iw;
        doc_.height = ih;
    }
    folder_.scan_for(doc_.path);
    apply_fit();
    refresh_status();
}

void Viewer::load_index(int i) {
    const char* p = folder_.path_at(i);
    if (p) load_path(p);
}

// ------------------------------------------------------------
// Perintah
// ------------------------------------------------------------
// Zoom kontinu (langkah 10%, clamp 10..400%) — bukan tabel preset: toolkit
// sudah punya ui_image_set_scale() berpersen, jadi tidak perlu level diskret.
// Zoom masuk dari keadaan Fit memakai persen fit saat ini sebagai titik awal.
void Viewer::cmd_zoom_in()  { if (!failed_) set_zoom(zoom_ + ZOOM_STEP); }
void Viewer::cmd_zoom_out() { if (!failed_) set_zoom(zoom_ - ZOOM_STEP); }
void Viewer::cmd_actual()   { set_zoom(100); }
void Viewer::cmd_fit()      { if (!failed_) { apply_fit(); refresh_status(); } }

void Viewer::cmd_next() {
    if (failed_ || !folder_.has_nav()) return;
    load_index(folder_.next_index());
}
void Viewer::cmd_prev() {
    if (failed_ || !folder_.has_nav()) return;
    load_index(folder_.prev_index());
}
void Viewer::cmd_close() { ui_window_request_close(win_); }

// ------------------------------------------------------------
// Status bar
// ------------------------------------------------------------
void Viewer::refresh_status() {
    char left[96];
    char right[40];
    if (failed_) {
        // Keadaan error harus terbaca di status bar juga, bukan hanya dialog
        // (dialog bisa ditutup tanpa keluar).
        const char* msg = doc_.path[0] ? "Cannot open" : "No image";
        int i = 0;
        for (; msg[i] && i < (int)sizeof(left) - 24; i++) left[i] = msg[i];
        if (doc_.name[0]) {
            left[i++] = ' '; left[i++] = ' '; left[i++] = ' ';
            for (int k = 0; doc_.name[k] && i < (int)sizeof(left) - 2; k++)
                left[i++] = doc_.name[k];
        }
        left[i] = '\0';
        ui_statusbar_set_text(status_, left, "-");
        return;
    }

    media_format_info(doc_.name, doc_.width, doc_.height, doc_.format,
                      doc_.size, left, sizeof(left));

    int n = 0;
    if (fit_) {
        const char* p = "Fit ";
        for (; *p; p++) right[n++] = *p;
    } else if (zoom_ == 100) {
        const char* p = "Actual size ";
        for (; *p; p++) right[n++] = *p;
    } else {
        const char* p = "Zoom ";
        for (; *p; p++) right[n++] = *p;
    }
    n += media_format_int(right + n, (int)sizeof(right) - n - 2, zoom_);
    right[n++] = '%';
    right[n] = '\0';

    if (folder_.has_nav()) {
        // Kedudukan di direktori ("3/12") — konteks Next/Previous terlihat.
        char merged[40];
        int m = media_format_int(merged, (int)sizeof(merged) - 8, folder_.index + 1);
        merged[m++] = '/';
        m += media_format_int(merged + m, (int)sizeof(merged) - m - 2, folder_.count);
        merged[m++] = ' '; merged[m++] = '|'; merged[m++] = ' ';
        for (int q = 0; right[q] && m < (int)sizeof(merged) - 1; q++) merged[m++] = right[q];
        merged[m] = '\0';
        ui_statusbar_set_text(status_, left, merged);
        return;
    }
    ui_statusbar_set_text(status_, left, right);
}

void Viewer::show_error_dialog() {
    ui_dialog_show(win_, doc_.error_title(), doc_.error_text(), kErrButtons, 1,
                   cb_err_dialog, this);
}

// ------------------------------------------------------------
int Viewer::run(const char* initial_path) {
    build_ui();
    if (!win_) return 1;      // window gagal dibuat (OOM / KWM penuh)
    load_path(initial_path);
    ui_window_run(win_);      // blocking; keluar via X / ESC / Close
    ui_window_destroy(win_);
    return 0;
}

}  // namespace iv
