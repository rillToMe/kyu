// ============================================================
// viewer.c — Image Viewer (galeri PNG, libui).
//
// Tata letak (atas → bawah):
//   [MenuBar: Berkas | Tampilan]
//   [tombol: -  +  Pas  1:1  Prev  Next]
//   [sidebar daftar berkas | gambar]
//   [statusbar: n/total  nama  WxH | mode zoom]
//
// Phase 11: berkas yang dipilih/dibuka langsung di-FIT ke area gambar, jadi
// SELURUH gambar terlihat tanpa digeser manual. Scrollbar baru muncul kalau
// gambar di-zoom melebihi area (via mode pan ScrollView), dan membesarkan
// gambar menjaga titik tengah view (tidak melompat ke pojok kiri-atas).
//
// Dari Explorer (view.tmp) → buka berkas itu langsung; tutup → kembali ke
// Explorer. Shortcut: +/- zoom, F pas, 1 ukuran asli, N/P berkas berikut/
// sebelumnya, Esc keluar.
//
// Build: viewer.o + userlib.o + libgui.o + libui.o + png.o
// ============================================================
#include "userlib.h"
#include "libui.h"

#define WIN_W      640
#define WIN_H      470
#define MARGIN     8
#define SPACING    8
#define SIDEBAR_W  170
#define BTN_H      28
#define STATUS_H   20
#define MENUBAR_H  24
// Tinggi body = sisa window setelah margin atas, menubar, baris tombol,
// statusbar, dan dua spasi antar-baris.
#define BODY_H     (WIN_H - MARGIN - MENUBAR_H - BTN_H - SPACING - SPACING - STATUS_H)
#define IMG_W      (WIN_W - MARGIN * 2 - SIDEBAR_W - SPACING)
#define IMG_H      BODY_H
#define MAX_PNG    32
#define NAME_MAX   24
#define ZOOM_MIN   10
#define ZOOM_MAX   400
#define ZOOM_STEP  10

static ui_window_t* g_win;
static ui_widget_t* g_list_win;
static ui_widget_t* g_img;
static ui_widget_t* g_status;
static char g_names[MAX_PNG][NAME_MAX];
static char g_cur[NAME_MAX];
static int  g_npng = 0;
static int  g_sel = -1;        // index di daftar; -1 = berkas di luar daftar
static int  g_zoom = 100;
static int  g_fit = 1;         // 1 = skala dihitung otomatis (Pas)
static int  g_from_fileman = 0;

// ------------------------------------------------------------
// Helper teks (tanpa printf)
// ------------------------------------------------------------
static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// Jejak ke konsol (COM1): bukti headless (QEMU -display none) bahwa pemindaian
// berkas dan perintah zoom benar-benar jalan — pola yang sama dengan notepad.
static void trace(const char* tag, const char* arg, int num) {
    print((char*)"[viewer] ");
    print((char*)tag);
    if (arg) { print((char*)" "); print((char*)arg); }
    if (num >= 0) { print((char*)" = "); print_num((uint32_t)num); }
    print((char*)"\n");
}

static void scopy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = '\0';
}

static int same_str(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static int put_str(char* dst, int n, const char* s) {
    for (int i = 0; s[i]; i++) dst[n++] = s[i];
    return n;
}

static int put_num(char* dst, int n, int v) {
    char tmp[12];
    int k = 0;
    if (v < 0) { dst[n++] = '-'; v = -v; }
    if (v == 0) tmp[k++] = '0';
    while (v > 0) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    for (int i = k - 1; i >= 0; i--) dst[n++] = tmp[i];
    return n;
}

// ------------------------------------------------------------
// Status bar
// ------------------------------------------------------------
static void refresh_status(void) {
    char left[96];
    int n = 0;
    if (g_npng == 0) {
        n = put_str(left, n, "Tidak ada berkas PNG di /");
    } else {
        if (g_sel >= 0) {
            n = put_num(left, n, g_sel + 1);
            n = put_str(left, n, "/");
            n = put_num(left, n, g_npng);
            n = put_str(left, n, "   ");
        }
        n = put_str(left, n, g_cur);
        int iw = 0, ih = 0;
        ui_image_natural_size(g_img, &iw, &ih);
        if (iw > 0) {
            n = put_str(left, n, "   ");
            n = put_num(left, n, iw);
            n = put_str(left, n, "x");
            n = put_num(left, n, ih);
        }
    }
    left[n] = '\0';

    char right[32];
    n = 0;
    if (g_npng == 0) {
        n = put_str(right, n, "-");
    } else if (g_fit) {
        n = put_str(right, n, "Pas  ");
        n = put_num(right, n, g_zoom);
        n = put_str(right, n, "%");
    } else if (g_zoom == 100) {
        n = put_str(right, n, "Ukuran asli  100%");
    } else {
        n = put_str(right, n, "Zoom  ");
        n = put_num(right, n, g_zoom);
        n = put_str(right, n, "%");
    }
    right[n] = '\0';
    ui_statusbar_set_text(g_status, left, right);
}

// ------------------------------------------------------------
// Tampilan gambar
// ------------------------------------------------------------
// Skala otomatis: seluruh gambar masuk area view (tanpa geser manual).
static void apply_fit(void) {
    int p = ui_image_set_fit(g_img, IMG_W, IMG_H);
    if (p > 0) { g_zoom = p; g_fit = 1; }
}

static void set_zoom(int p) {
    if (g_npng == 0) return;
    if (p < ZOOM_MIN) p = ZOOM_MIN;
    if (p > ZOOM_MAX) p = ZOOM_MAX;
    g_zoom = p;
    g_fit = 0;
    ui_image_set_scale(g_img, g_zoom);
    refresh_status();
}

static void show_name(const char* name) {
    if (!name || !name[0]) return;
    scopy(g_cur, name, sizeof(g_cur));
    g_sel = -1;
    for (int i = 0; i < g_npng; i++) {
        if (same_str(g_names[i], g_cur)) {
            g_sel = i;
            ui_listview_set_selected(g_list_win, i);   // sorot di sidebar
            break;
        }
    }
    ui_image_set_file(g_img, g_cur);   // reset zoom 100%
    apply_fit();
    refresh_status();
    trace("buka", g_cur, g_zoom);
}

static void show_index(int i) {
    if (i < 0 || i >= g_npng) return;
    show_name(g_names[i]);
}

// ------------------------------------------------------------
// Callback kontrol
// ------------------------------------------------------------
static void zoom_in(void* u)    { (void)u; set_zoom(g_zoom + ZOOM_STEP); }
static void zoom_out(void* u)   { (void)u; set_zoom(g_zoom - ZOOM_STEP); }
static void fit_now(void* u)    { (void)u; if (g_npng) { apply_fit(); refresh_status(); } }
static void actual_size(void* u){ (void)u; set_zoom(100); }

static void next_file(void* u) {
    (void)u;
    if (g_npng > 1) show_index(g_sel < 0 ? 0 : (g_sel + 1) % g_npng);
}
static void prev_file(void* u) {
    (void)u;
    if (g_npng > 1) show_index(g_sel < 0 ? 0 : (g_sel + g_npng - 1) % g_npng);
}
static void on_quit(void* u) { (void)u; ui_window_request_close(g_win); }

static void on_select(void* userdata) {
    (void)userdata;
    int sel = ui_listview_selected(g_list_win);
    if (sel >= 0 && sel < g_npng && sel != g_sel) show_index(sel);
}

// ------------------------------------------------------------
// Pemindaian berkas
// ------------------------------------------------------------
static int is_png(const char* name) {
    int n = slen(name);
    return n > 4 && name[n-4]=='.' && name[n-3]=='p' &&
           name[n-2]=='n' && name[n-1]=='g';
}

static void scan_pngs(void) {
    file_info_t fi[MAX_PNG];
    int total = sys_get_file_list("/", fi, MAX_PNG);
    g_npng = 0;
    for (int i = 0; i < total && g_npng < MAX_PNG; i++) {
        if (fi[i].is_folder || !is_png(fi[i].filename)) continue;
        scopy(g_names[g_npng], fi[i].filename, NAME_MAX);
        ui_listview_add_item(g_list_win, g_names[g_npng]);
        g_npng++;
    }
    trace("png di /", 0, g_npng);
}

static void add_btn(ui_widget_t* bar, const char* label, ui_click_cb cb) {
    ui_widget_t* b = ui_button_create(g_win, label);
    ui_button_set_click(b, cb, 0);
    ui_layout_add(bar, b);
}

// ------------------------------------------------------------
void main(void) {
    char target[64];
    target[0] = '\0';
    if (sys_file_exists("view.tmp")) {
        uint32_t ts = sys_file_size("view.tmp");
        if (ts > 0 && ts < 63) {
            sys_read_file_to_buffer("view.tmp", target, sizeof(target));
            target[ts] = '\0';
            g_from_fileman = 1;
        }
        fs_delete("view.tmp");   // konsumsi flag sekali pakai
    }

    g_win = ui_window_create(WIN_W, WIN_H);
    if (!g_win) { sys_exit(); }
    ui_window_set_title(g_win, "Image Viewer");

    // --- MenuBar ---
    ui_widget_t* mb = ui_menubar_create(g_win);
    ui_widget_t* mf = ui_menubar_add_menu(mb, "Berkas");
    ui_menu_add_item_acc(mf, "Berkas berikutnya", "N", next_file, 0);
    ui_menu_add_item_acc(mf, "Berkas sebelumnya", "P", prev_file, 0);
    ui_menu_add_sep(mf);
    ui_menu_add_item_acc(mf, "Keluar", "Esc", on_quit, 0);

    ui_widget_t* mv = ui_menubar_add_menu(mb, "Tampilan");
    ui_menu_add_item_acc(mv, "Zoom +", "+", zoom_in, 0);
    ui_menu_add_item_acc(mv, "Zoom -", "-", zoom_out, 0);
    ui_menu_add_sep(mv);
    ui_menu_add_item_acc(mv, "Pas ke Jendela", "F", fit_now, 0);
    ui_menu_add_item_acc(mv, "Ukuran Asli (1:1)", "1", actual_size, 0);
    ui_window_add_bar(g_win, mb);

    // --- Baris tombol ---
    ui_widget_t* rowbar = ui_hbox_create(g_win, 6);
    add_btn(rowbar, "-",    zoom_out);
    add_btn(rowbar, "+",    zoom_in);
    add_btn(rowbar, "Pas",  fit_now);
    add_btn(rowbar, "1:1",  actual_size);
    add_btn(rowbar, "Prev", prev_file);
    add_btn(rowbar, "Next", next_file);

    // --- Sidebar daftar berkas + area gambar ---
    ui_widget_t* body = ui_hbox_create(g_win, SPACING);
    g_list_win = ui_listview_create(g_win, SIDEBAR_W, BODY_H);
    ui_listview_set_change(g_list_win, on_select, 0);
    ui_layout_add(body, g_list_win);

    ui_widget_t* sv = ui_scrollview_create(g_win, IMG_W, IMG_H);
    ui_scrollview_set_pan(sv, 1);      // scroll 2 arah + center + anchor zoom
    g_img = ui_image_create(g_win, "", 0, 0);   // kosong sampai dipilih
    ui_scrollview_set_child(sv, g_img);
    ui_layout_add(body, sv);

    // --- Root layout: tombol / body / statusbar ---
    ui_widget_t* box = ui_vbox_create(g_win, SPACING);
    ui_layout_add(box, rowbar);
    ui_layout_add(box, body);
    g_status = ui_statusbar_create(g_win);
    ui_widget_set_size(g_status, WIN_W - MARGIN * 2, STATUS_H);
    ui_layout_add(box, g_status);
    ui_window_add(g_win, box);

    // --- Shortcut ---
    // '+'/'_' butuh Shift di keyboard US, jadi mods-nya ikut dicocokkan
    // (kernel mengirim P1 hasil terjemahan shift + bit KEY_MOD_SHIFT).
    ui_window_add_shortcut(g_win, 0, '=', zoom_in, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_SHIFT, '+', zoom_in, 0);
    ui_window_add_shortcut(g_win, 0, '-', zoom_out, 0);
    ui_window_add_shortcut(g_win, KEY_MOD_SHIFT, '_', zoom_out, 0);
    ui_window_add_shortcut(g_win, 0, 'f', fit_now, 0);
    ui_window_add_shortcut(g_win, 0, 'F', fit_now, 0);
    ui_window_add_shortcut(g_win, 0, '1', actual_size, 0);
    ui_window_add_shortcut(g_win, 0, 'n', next_file, 0);
    ui_window_add_shortcut(g_win, 0, 'N', next_file, 0);
    ui_window_add_shortcut(g_win, 0, 'p', prev_file, 0);
    ui_window_add_shortcut(g_win, 0, 'P', prev_file, 0);

    scan_pngs();
    if (g_from_fileman && target[0]) show_name(target);
    else if (g_npng > 0) show_index(0);
    refresh_status();

    ui_window_run(g_win);   // blocking; keluar via X / ESC
    ui_window_destroy(g_win);

    if (g_from_fileman) {
        char p[32];
        build_app_path(p, sizeof(p), "fileman.elf");
        sys_exec(p);
    } else {
        sys_exit();
    }
}
