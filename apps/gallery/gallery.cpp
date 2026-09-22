// apps/gallery/gallery.cpp — implementasi aplikasi Gallery.
//
// Tata letak (widget toolkit yang sudah ada; tanpa tema kustom → native):
//   [MenuBar: File | View]
//   [Toolbar: Open  Back  Refresh]
//   [GridView: kisi thumbnail + nama berkas]
//   [StatusBar: "3/12" atau "12 items" | nama  1920x1080  PNG  2.4 MB]
//
// Alur data (satu arah, tanpa widget per item):
//   scan direktori → media_entry_t[] → GridView (nama saja)
//                                      ↓ sel terlihat
//                          ThumbCache.get() → decode 1 gambar → perkecil
//                                      ↓
//                          ui_gridview_set_thumb() → digambar toolkit
#include "gallery.hpp"

namespace gal {

namespace {

// Ukuran window + tata letak (MenuBar 24 + Toolbar 28, root VBox margin 8).
const int WIN_W     = 760;
const int WIN_H     = 560;
const int MARGIN    = 8;
const int SPACING   = 6;
const int MENUBAR_H = 24;
const int TOOLBAR_H = 28;
const int STATUS_H  = 20;
const int GRID_W    = WIN_W - MARGIN * 2;
const int GRID_H    = WIN_H - (MARGIN + MENUBAR_H + TOOLBAR_H) - STATUS_H
                      - SPACING - MARGIN;
// Pitch sel + kotak thumbnail (thumbnail dibuat SEKALI pada ukuran ini).
const int CELL_W    = 146;
const int CELL_H    = 136;
const int THUMB_BOX = 110;

// Lokasi awal Gallery. Satu-satunya tempat yang perlu diubah kalau kelak
// KyuzenFS punya direktori media tetap (mis. /Pictures) — lihat catatan §23.
const char* const GALLERY_HOME = "/";

const char* const kErrButtons[] = { "Close" };

Gallery* self(void* ud) { return static_cast<Gallery*>(ud); }

void trace(const char* tag, const char* arg, int num) {
    print((char*)"[gallery] ");
    print((char*)tag);
    if (arg) { print((char*)" "); print((char*)arg); }
    if (num >= 0) { print((char*)" = "); print_num((uint32_t)num); }
    print((char*)"\n");
}

// --- Thunk callback gaya C ---
void cb_open(void* ud)     { self(ud)->cmd_open(); }
void cb_back(void* ud)     { self(ud)->cmd_back(); }
void cb_rescan(void* ud)   { self(ud)->cmd_rescan(); }
void cb_close(void* ud)    { self(ud)->cmd_close(); }
void cb_change(void* ud)   { self(ud)->on_selection_changed(); }
void cb_activate(void* ud) { self(ud)->on_activate(); }
void cb_dialog(void* ud, int index) { self(ud)->on_dialog_done(index); }

// Cache thumbnail melaporkan slot yang dibuang → Gallery mengosongkan sel itu
// (pointer lama tidak boleh dipakai lagi).
void cb_evict(void* ud, const char* name) {
    self(ud)->on_thumb_evicted(name);
}

int cb_tick(void* ud) { return self(ud)->tick(); }

}  // namespace

Gallery::Gallery()
    : win_(0), grid_(0), status_(0), count_(0), depth_(0), cursor_(0) {
    dir_[0] = '\0';
    for (int i = 0; i < MEDIA_MAX_ENTRIES; i++) state_[i] = CELL_DONE;
    thumbs_.on_evict(cb_evict, this);
}

// ------------------------------------------------------------
// Konstruksi UI
// ------------------------------------------------------------
void Gallery::build_ui() {
    win_ = ui_window_create(WIN_W, WIN_H);
    if (!win_) return;      // OOM: run() memeriksa dan keluar dengan rapi
    ui_window_set_title(win_, "Gallery");

    ui_widget_t* mb = ui_menubar_create(win_);
    ui_widget_t* mf = ui_menubar_add_menu(mb, "File");
    ui_menu_add_item_acc(mf, "Open", "Enter", cb_open, this);
    ui_menu_add_item_acc(mf, "Refresh", "R", cb_rescan, this);
    ui_menu_add_item_acc(mf, "Up one folder", "Back", cb_back, this);
    ui_menu_add_sep(mf);
    ui_menu_add_item_acc(mf, "Close", "Esc", cb_close, this);
    ui_widget_t* mv = ui_menubar_add_menu(mb, "View");
    ui_menu_add_item_acc(mv, "Open selected image", "Enter", cb_open, this);
    ui_menu_add_item_acc(mv, "Reload thumbnails", "R", cb_rescan, this);
    ui_window_add_bar(win_, mb);

    ui_widget_t* tb = ui_toolbar_create(win_);
    ui_toolbar_add_button(tb, "Open", cb_open, this);
    ui_toolbar_add_button(tb, "Back", cb_back, this);
    ui_toolbar_add_button(tb, "Refresh", cb_rescan, this);
    ui_window_add_bar(win_, tb);

    grid_ = ui_gridview_create(win_, GRID_W, GRID_H);
    ui_gridview_set_cell(grid_, CELL_W, CELL_H, THUMB_BOX);
    ui_gridview_set_empty_text(grid_, "No images in this folder");
    ui_gridview_set_change(grid_, cb_change, this);
    ui_gridview_set_activate(grid_, cb_activate, this);
    ui_window_focus(win_, grid_);     // panah/Enter langsung bekerja

    ui_widget_t* box = ui_vbox_create(win_, SPACING);
    ui_layout_add(box, grid_);
    status_ = ui_statusbar_create(win_);
    ui_widget_set_size(status_, GRID_W, STATUS_H);
    ui_layout_add(box, status_);
    ui_window_add(win_, box);

    // Tick: pemuatan thumbnail bertahap (1 gambar per iterasi loop).
    ui_window_set_tick(win_, cb_tick, this);

    // Enter datang sebagai ASCII '\n' (kernel: kbdus[0x1C]) — shortcut membuat
    // Open tetap bekerja walau fokus keyboard sedang bukan di kisi (mis. sesudah
    // mengklik tombol toolbar).
    ui_window_add_shortcut(win_, 0, '\n', cb_open, this);
    ui_window_add_shortcut(win_, 0, '\r', cb_open, this);
    ui_window_add_shortcut(win_, 0, 'o', cb_open, this);
    ui_window_add_shortcut(win_, 0, 'O', cb_open, this);
    ui_window_add_shortcut(win_, 0, 'r', cb_rescan, this);
    ui_window_add_shortcut(win_, 0, 'R', cb_rescan, this);
}

// ------------------------------------------------------------
// Pemindaian
// ------------------------------------------------------------
void Gallery::scan(const char* dir) {
    if (!dir || !dir[0]) dir = GALLERY_HOME;
    int n = 0;
    for (int i = 0; dir[i] && i < MEDIA_PATH_MAX - 1; i++) { dir_[i] = dir[i]; n = i + 1; }
    dir_[n] = '\0';

    // Nama yang sedang terpilih: dipakai untuk memulihkan posisi setelah
    // rescan, sehingga Refresh tidak melempar pengguna kembali ke item pertama.
    char keep[MEDIA_NAME_MAX];
    keep[0] = '\0';
    const int prev = selected_entry();
    if (prev >= 0) {
        int k = 0;
        for (; entries_[prev].name[k] && k < MEDIA_NAME_MAX - 1; k++)
            keep[k] = entries_[prev].name[k];
        keep[k] = '\0';
    }

    media_entry_t all[MEDIA_MAX_ENTRIES];
    const int total = media_scan(dir_, all, MEDIA_MAX_ENTRIES, MEDIA_UNKNOWN);

    count_ = 0;
    int skipped = 0;
    for (int i = 0; i < total && count_ < MEDIA_MAX_ENTRIES; i++) {
        // Hanya folder (untuk navigasi) + gambar yang BENAR-BENAR bisa
        // didekode. Format belum didukung (.jpg) tidak pernah ditawarkan —
        // Gallery tidak boleh menjanjikan apa yang tak bisa dibuka.
        const bool folder = (all[i].type == MEDIA_FOLDER);
        const bool image = (all[i].type == MEDIA_IMAGE);
        if (!folder && !(image && all[i].supported)) { skipped++; continue; }
        entries_[count_] = all[i];
        state_[count_] = CELL_PENDING;
        count_++;
    }

    // Daftar berkas berubah → semua thumbnail lama tidak relevan. Cache
    // dikosongkan tanpa callback: kisi dibersihkan di bawah ini.
    thumbs_.reset();
    ui_gridview_clear(grid_);
    for (int i = 0; i < count_; i++) {
        ui_gridview_add_item(grid_, entries_[i].name);
        // Folder tidak punya thumbnail: tandai selesai supaya tidak pernah
        // masuk antrean pemuatan.
        if (entries_[i].type == MEDIA_FOLDER) {
            state_[i] = CELL_DONE;
            ui_gridview_set_placeholder(grid_, i, UI_GRID_PH_EMPTY);
        }
    }
    cursor_ = 0;
    if (count_ > 0 && selected_entry() < 0) {
        const int again = keep[0] ? find_cell(keep) : -1;
        ui_gridview_set_selected(grid_, again >= 0 ? again : 0);
    }
    refresh_status();

    trace("scan", dir_, count_);
    if (skipped > 0) trace("skipped unsupported", 0, skipped);
}

int Gallery::selected_entry() const {
    const int i = ui_gridview_selected(grid_);
    return (i >= 0 && i < count_) ? i : -1;
}

// ------------------------------------------------------------
// Status bar
// ------------------------------------------------------------
void Gallery::refresh_status() {
    char left[96];
    int n = 0;
    const char* cnt = "items";
    n += media_format_int(left + n, (int)sizeof(left) - n - 12, count_);
    if (count_ == 1) cnt = "item";
    left[n++] = ' ';
    for (int k = 0; cnt[k] && n < (int)sizeof(left) - 2; k++) left[n++] = cnt[k];
    if (depth_ > 0) {
        const char* p = "   (subfolder)";
        for (int k = 0; p[k] && n < (int)sizeof(left) - 2; k++) left[n++] = p[k];
    }
    left[n] = '\0';

    const int sel = selected_entry();
    if (sel < 0) {
        // Tak ada yang dipilih: tampilkan direktori (konteks lebih berguna
        // daripada string kosong).
        ui_statusbar_set_text(status_, left, dir_);
        return;
    }

    // Metadata dari probe header (64 byte) — TIDAK men-decode ulang gambar.
    int w = 0, h = 0;
    const char* fmt = 0;
    media_probe_image(entries_[sel].path, &w, &h, &fmt);
    char right[96];
    media_format_info(entries_[sel].name, w, h, fmt, entries_[sel].size,
                      right, sizeof(right));
    // Kalau probe gagal (mis. berkas rusak), minimal nama + (folder) terbaca.
    if (!right[0]) media_format_info(entries_[sel].name, 0, 0,
                                     media_type_name((media_type_t)entries_[sel].type),
                                     0, right, sizeof(right));
    ui_statusbar_set_text(status_, left, right);
}

// ------------------------------------------------------------
// Thumbnail bertahap
// ------------------------------------------------------------
void Gallery::clear_cell(int i) {
    if (i < 0 || i >= count_) return;
    state_[i] = CELL_PENDING;
    ui_gridview_set_placeholder(grid_, i, UI_GRID_PH_LOADING);
}

int Gallery::find_cell(const char* name) const {
    if (!name) return -1;
    for (int i = 0; i < count_; i++) {
        int k = 0;
        for (; entries_[i].name[k] && name[k] && k < MEDIA_NAME_MAX - 1; k++)
            if (entries_[i].name[k] != name[k]) break;
        if (entries_[i].name[k] == name[k]) return i;
    }
    return -1;
}

void Gallery::on_thumb_evicted(const char* name) {
    // Cache membuang buffer yang tidak sedang tampil → sel itu kembali ke
    // keadaan "belum ada thumbnail" dan akan dimuat ulang bila terlihat lagi.
    const int i = find_cell(name);
    if (i >= 0) clear_cell(i);
}

void Gallery::load_visible() {
    if (count_ <= 0) return;
    int first = 0, last = -1;
    if (ui_gridview_visible_range(grid_, &first, &last) <= 0) return;

    // 1. Lindungi thumbnail yang SEDANG TAMPIL dari eviction (hanya sel
    //    terlihat; sel di luar viewport boleh dibuang kapan saja).
    const char* live[MEDIA_MAX_ENTRIES];
    int nlive = 0;
    for (int i = first; i <= last && nlive < MEDIA_MAX_ENTRIES; i++)
        if (state_[i] == CELL_DONE && entries_[i].type == MEDIA_IMAGE)
            live[nlive++] = entries_[i].name;
    thumbs_.mark_live(live, nlive);
}

int Gallery::tick() {
    if (count_ <= 0) return 0;
    int first = 0, last = -1;
    if (ui_gridview_visible_range(grid_, &first, &last) <= 0) return 0;

    load_visible();

    // 2. Muat SATU thumbnail per iterasi, mulai dari cursor_ yang berotasi —
    //    jadi kisi terisi dari kiri-atas tanpa membekukan UI (tiap frame
    //    mengerjakan satu gambar, bukan seluruh koleksi).
    const int span = last - first + 1;
    int start = cursor_;
    if (start < first || start > last) start = first;
    for (int k = 0; k < span; k++) {
        const int i = first + ((start - first + k) % span);
        if (state_[i] != CELL_PENDING) continue;
        if (entries_[i].type != MEDIA_IMAGE) continue;

        int w = 0, h = 0;
        const uint32_t* px = thumbs_.get(entries_[i].path, &w, &h);
        cursor_ = i + 1;
        if (px && w > 0 && h > 0) {
            ui_gridview_set_thumb(grid_, i, px, w, h);
            state_[i] = CELL_DONE;
        } else {
            // Gagal decode (rusak / OOM): placeholder error, tidak dicoba lagi.
            ui_gridview_set_placeholder(grid_, i, UI_GRID_PH_ERROR);
            state_[i] = CELL_ERROR;
            trace("thumb failed", entries_[i].name, -1);
        }
        return 1;      // ada perubahan → toolkit render
    }
    return 0;
}

// ------------------------------------------------------------
// Buka / navigasi
// ------------------------------------------------------------
void Gallery::launch_viewer(const char* path) {
    char elf[32];
    build_app_path(elf, (int)sizeof(elf), "imageview.elf");

    char pbuf[MEDIA_PATH_MAX];
    int i = 0;
    for (; path[i] && i < MEDIA_PATH_MAX - 1; i++) pbuf[i] = path[i];
    pbuf[i] = '\0';

    // Mekanisme launch yang ada: sys_spawn_argv (jalur argumen P0 Phase 2).
    // ImageView menerima path di argv[1] — tidak ada file hand-off sementara,
    // dan Gallery TETAP hidup (browser tidak ikut tertutup saat viewer dibuka).
    char* argv[2];
    argv[0] = elf;
    argv[1] = pbuf;
    const int pid = sys_spawn_argv(elf, 2, argv);
    if (pid < 0) {
        show_error("Could not open image",
                   "Image Viewer could not be started. The system may be "
                   "out of memory.");
        trace("spawn imageview FAILED", pbuf, -1);
        return;
    }
    trace("open", pbuf, pid);
}

bool Gallery::open_entry(int i) {
    if (i < 0 || i >= count_) return false;

    if (entries_[i].type == MEDIA_FOLDER) {
        if (depth_ >= DIR_STACK_MAX) {
            show_error("Folder nesting limit",
                       "This build of Gallery keeps track of at most 8 levels "
                       "of folders.");
            return false;
        }
        int k = 0;
        for (; dir_[k] && k < MEDIA_PATH_MAX - 1; k++) stack_[depth_][k] = dir_[k];
        stack_[depth_][k] = '\0';
        int m = 0;
        for (; entries_[i].name[m] && m < MEDIA_NAME_MAX - 1; m++)
            stack_name_[depth_][m] = entries_[i].name[m];
        stack_name_[depth_][m] = '\0';
        depth_++;
        scan(entries_[i].path);
        return true;
    }

    if (entries_[i].type == MEDIA_IMAGE && entries_[i].supported) {
        launch_viewer(entries_[i].path);
        return true;
    }

    // Sisanya (folder penuh, format belum didukung, isi folder tidak dikenal):
    // jelaskan apa adanya, jangan diam-diam tidak melakukan apa pun.
    show_error("Unsupported format",
               "KyuzenOS cannot display this file yet. Supported image "
               "formats: PNG, BMP.");
    return false;
}

// ------------------------------------------------------------
// Perintah
// ------------------------------------------------------------
void Gallery::cmd_open()   { open_entry(selected_entry()); }
void Gallery::cmd_rescan() { scan(dir_); }
void Gallery::cmd_close()  { ui_window_request_close(win_); }

void Gallery::cmd_back() {
    if (depth_ <= 0) return;      // sudah di akar: tak ada yang di atas
    depth_--;
    const char* came_from = stack_name_[depth_];
    char want[MEDIA_NAME_MAX];
    int k = 0;
    for (; came_from[k] && k < MEDIA_NAME_MAX - 1; k++) want[k] = came_from[k];
    want[k] = '\0';
    scan(stack_[depth_]);
    // Sorot folder yang baru saja ditinggalkan supaya posisi tidak "hilang".
    const int idx = find_cell(want);
    if (idx >= 0) ui_gridview_set_selected(grid_, idx);
}

void Gallery::on_selection_changed() { refresh_status(); }
void Gallery::on_activate() { open_entry(selected_entry()); }
void Gallery::on_dialog_done(int index) { (void)index; }

void Gallery::show_error(const char* title, const char* text) {
    ui_dialog_show(win_, title, text, kErrButtons, 1, cb_dialog, this);
}

// ------------------------------------------------------------
int Gallery::run(const char* initial_dir) {
    build_ui();
    if (!win_) return 1;
    scan(initial_dir && initial_dir[0] ? initial_dir : GALLERY_HOME);
    ui_window_run(win_);      // blocking; keluar via X / ESC / Close
    ui_window_destroy(win_);
    return 0;
}

}  // namespace gal
