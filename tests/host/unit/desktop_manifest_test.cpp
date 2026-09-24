// Host test desktop Phase 8: Launcher + CrashNotice + Taskbar
// (system/desktop) serta Event/WM/System/geometri (libdesktop).
//
// Backend + modul dikompilasi langsung (lihat rule `make test-desktop`);
// syscall di-stub dengan FS/antrean/window palsu. Tanpa QEMU, tanpa libgui
// (draw() tidak dipanggil — Canvas cukup sebagai deklarasi).
#include <cassert>
#include <cstdio>

// Test ini tidak memakai libc host untuk string (gaya desktop.c): helper
// lokal di bawah. <cstring> dihindari — header string kernel vs host bisa
// bentrok linkage di TU C++.

// Deklarasi syscall C (linkage C, seperti backend framework).
extern "C" {
#include "userlib.h"
#include "libgui.h"
}

#include <kyuzen/desktop/event.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/system.hpp>
#include <kyuzen/desktop/window_manager.hpp>

#include "crash_notice.hpp"
#include "desktop_shell.hpp"
#include "launcher.hpp"
#include "taskbar.hpp"
#include "theme.hpp"
#include "app_icons.hpp"
#include "wallpaper.hpp"
#include "app_preview.hpp"

using namespace kyuzen::desktop;
using namespace desktop_impl;

static bool color_eq(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static int t_slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
static bool t_streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}
static void t_strcpy(char* d, const char* s) {
    int i = 0;
    while ((d[i] = s[i])) i++;
}
static void t_memcpy(char* d, const char* s, int n) {
    for (int i = 0; i < n; i++) d[i] = s[i];
}
static void t_strncpy(char* d, const char* s, int cap) {
    int i = 0;
    while (i < cap - 1 && s[i]) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

// --- FS palsu (launcher) ---
struct FakeFile {
    const char* name;
    const char* data;
};
static FakeFile FS[] = {
    {"fileman.elf", 0},
    {"fileman.app", "name=Explorer\ncolor=0x1565C0\n"},
    {"calc.elf", 0},
    {"calc.app", "name=Kalkulator\r\ncolor=0x2E7D32\r\n"},
    {"desktop.elf", 0},
    {"desktop.app", "name=Desktop\nhidden=1\nwallpaper=city-town.png\n"},
    {"badptr.elf", 0},
    {"demo.elf", 0},
    {"demo.app", "name=Demo\nicon=demo.png\n"},
    {"demo.png", "PNG"},
    {"default.png", "PNG"},
    {"city-town.png", "PNG"},
    {"kyuzen.png", 0},
    {"notes.txt", 0},
    {"meadow.png", "PNG"},  // wallpaper kedua untuk uji poll (live reload)
    {"/wallpaper.ui", 0},  // override Settings (0 = absen; diisi per-uji)
};
#define FS_N ((int)(sizeof(FS) / sizeof(FS[0])))

static int find_file(const char* n) {
    for (int i = 0; i < FS_N; i++)
        if (t_streq(FS[i].name, n)) return i;
    return -1;
}
static const char* strip_apps(const char* p) {
    if (p[0] == '/' && p[1] == 'a' && p[2] == 'p' && p[3] == 'p' &&
        p[4] == 's' && p[5] == '/')
        return p + 6;
    return p;
}

// --- status stub yang bisa diskrip ---
static uint64_t g_now = 100000;
static int g_spawns = 0;
static char g_last_spawn[32];
static int g_crash_mode = 0;  // 0 = boot normal, 1 = ada laporan baru
static int g_kw_fail = 0;     // 1 = sys_kwm_get_windows gagal
static int g_kw_focus2 = 0;   // ubah fokus window ke-2 (deteksi perubahan)
static int g_last_activate = -999;
static uint32_t g_time[6] = {2026, 9, 21, 21, 35, 0};  // RTC palsu
static int g_img_fail = 0;  // 1 = img_decode selalu gagal
static int g_zombies = 0;       // child keluar yang menunggu reap (WNOHANG)
static int g_waitpid_calls = 0;  // hitung pemanggilan sys_waitpid

// Sink fill_rect palsu. Canvas host tidak bisa dipakai untuk mengamati gambar
// (Impl-nya milik Application), jadi jalur blit pixel (RLE) diuji lewat sink
// ini — Canvas di app memenuhi kontrak yang sama (fill_rect(Rect, Color)).
struct RectSink {
    int n;
    Rect last;
    Color last_c;
    void fill_rect(const Rect& r, Color c) {
        n++;
        last = r;
        last_c = c;
    }
};

struct RawEv {
    int type, p1, p2, p3, win;
};
static RawEv g_evs[10];
static int g_nev = 0;
static int g_evidx = 0;

extern "C" {

int sys_get_file_list(char* path, file_info_t* buf, int max) {
    (void)path;
    int n = 0;
    for (int i = 0; i < FS_N && n < max; i++) {
        t_strcpy(buf[n].filename, FS[i].name);
        buf[n].size = FS[i].data ? (uint32_t)t_slen(FS[i].data) : 0;
        buf[n].is_folder = 0;
        n++;
    }
    return n;
}
int sys_file_exists(char* f) {
    int i = find_file(strip_apps(f));
    return i >= 0 && FS[i].data != 0;
}
int sys_read_file_to_buffer(char* f, char* buf, uint32_t cap) {
    int i = find_file(strip_apps(f));
    if (i < 0 || !FS[i].data) return 0;
    uint32_t len = (uint32_t)t_slen(FS[i].data);
    if (len > cap) return 0;
    t_memcpy(buf, FS[i].data, len);
    return 1;
}
uint64_t sys_uptime(void) {
    return g_now;
}
int sys_spawn(char* f) {
    t_strncpy(g_last_spawn, f, sizeof(g_last_spawn) - 1);
    g_last_spawn[sizeof(g_last_spawn) - 1] = '\0';
    g_spawns++;
    return 0;
}
// Zombie-exhaustion fix: on_poll sweeps exited children via WNOHANG.
// Model: g_zombies zombie menunggu; tiap reap mengembalikan satu pid,
// lalu -1 (tak ada child). options WNOHANG=1 diteruskan dan dihormati.
int sys_waitpid(int pid, int* status, int options) {
    (void)pid;
    g_waitpid_calls++;
    if (options != 0 && options != 1) return -1;
    if (g_zombies > 0) {
        g_zombies--;
        if (status) *status = 0;
        return 42;
    }
    return -1;
}
void sys_yield(void) {}
void sys_exit_code(int code) {
    (void)code;
    while (true) {
    }
}
void print(char* s) {
    (void)s;
}
void print_num(uint32_t v) {
    (void)v;
}
// Canvas backend tak berjalan di host (draw() tak dipanggil test) —
// cukup simbolnya ada; bodi tak pernah dieksekusi.
void gui_draw_rect(gui_window_t* w, int x, int y, int cw, int ch,
                   color_t c) {
    (void)w;
    (void)x;
    (void)y;
    (void)cw;
    (void)ch;
    (void)c;
}
void gui_draw_text(gui_window_t* w, const char* t, int x, int y,
                   color_t c) {
    (void)w;
    (void)t;
    (void)x;
    (void)y;
    (void)c;
}
void gui_flush(gui_window_t* w) {
    (void)w;
}
void gui_damage_rect(gui_window_t* w, int x, int y, int cw, int ch) {
    (void)w;
    (void)x;
    (void)y;
    (void)cw;
    (void)ch;
}
uint32_t sys_file_size(char* f) {
    int i = find_file(strip_apps(f));
    if (i < 0) return 0;
    // Gambar palsu: ukuran pixmap kecil; teks: panjang data.
    if (t_streq(FS[i].name, "demo.png") || t_streq(FS[i].name, "default.png"))
        return 100;
    if (t_streq(FS[i].name, "city-town.png")) return 200;
    if (t_streq(FS[i].name, "meadow.png")) return 200;
    return FS[i].data ? (uint32_t)t_slen(FS[i].data) : 0;
}
void sys_get_time(uint32_t* t) {
    for (int i = 0; i < 6; i++) t[i] = g_time[i];
}
// CATATAN Phase 9.5: sys_alloc/sys_free TIDAK dipakai modul desktop inti,
// TETAPI launcher font UI (libs/text, font blob + glyph cache) membutuhkannya
// kembali — stub malloc-backed di sini (jujur: heap host, bukan uheap).
// Bila modul non-font memanggilnya, link tetap lolos — batas ini dijaga
// oleh review, bukan linker.
#include <cstdlib>
void* sys_alloc(uint32_t n) { return malloc(n ? n : 1); }
void sys_free(void* p) { free(p); }
void* sys_realloc(void* p, uint32_t o, uint32_t n) {
    (void)o;
    if (n == 0) {
        free(p);
        return 0;
    }
    return realloc(p, n);
}
// png_decode palsu: 4x4 putih untuk PNG yang dikenal, null sisanya
// (atau selalu null bila g_img_fail — jalur fallback total).
static uint32_t g_fake_px[16];
uint32_t* png_decode(const char* filename, int* out_w, int* out_h) {
    if (g_img_fail) return 0;
    const char* b = filename;
    for (int i = 0; filename[i]; i++)
        if (filename[i] == '/') b = &filename[i + 1];
    if (!t_streq(b, "demo.png") && !t_streq(b, "default.png") &&
        !t_streq(b, "city-town.png") && !t_streq(b, "meadow.png"))
        return 0;
    for (int i = 0; i < 16; i++) g_fake_px[i] = 0xFFFFFFFFu;
    *out_w = 4;
    *out_h = 4;
    return g_fake_px;
}
void png_free(uint32_t* buf) {
    (void)buf;
}
int sys_crash_notice(crash_notice_t* out) {
    if (!g_crash_mode) return 0;
    out->pending = 1;
    out->crash_count = 3;
    out->uptime_ms = 12345;
    out->vector = 14;
    out->task_id = 2;
    t_strcpy(out->task_name, "init");
    t_strcpy(out->path, "/crash-report.txt");
    return 1;
}
int sys_kwm_get_windows(kwm_window_info_t* b, int m) {
    if (g_kw_fail) return -1;
    if (m < 2) return 0;
    b[0].win_id = 1;
    b[0].active = 1;
    b[0].focused = 0;
    b[0].flags = 0x1;  // desktop
    b[0].title[0] = '\0';
    b[1].win_id = 2;
    b[1].active = 1;
    b[1].focused = g_kw_focus2 ? 0 : 1;
    b[1].flags = 0;  // window biasa (BUKAN desktop). Eksplisit: field ini
                     // tadinya tak diinisialisasi sehingga hasil tes bisa
                     // bergantung sampah stack (bit0 = flag desktop).
    t_strcpy(b[1].title, "term");
    return 2;
}
int sys_kwm_activate_window(int id) {
    g_last_activate = id;
    return 0;
}
int sys_get_event(kyuzen_event_t* e) {
    if (g_evidx >= g_nev) return 0;
    e->type = (uint32_t)g_evs[g_evidx].type;
    e->param1 = g_evs[g_evidx].p1;
    e->param2 = g_evs[g_evidx].p2;
    e->param3 = g_evs[g_evidx].p3;
    e->win_id = g_evs[g_evidx].win;
    g_evidx++;
    return 1;
}

}  // extern "C"

static Point pt(int x, int y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}

int main(void) {
    // --- geometri framework ---
    {
        Rect r{10, 20, 100, 50};
        assert(r.contains(pt(50, 40)) && !r.contains(pt(200, 20)));
        Rect o{50, 40, 100, 100};
        Rect f{500, 500, 10, 10};
        assert(r.intersects(o) && !r.intersects(f));
        assert(color_eq(rgb(1, 2, 3), Color{1, 2, 3, 255}));
    }

    // --- parse_color: hex, desimal, sampah ---
    assert(color_eq(parse_color("0x1565C0"), rgb(0x15, 0x65, 0xC0)));
    assert(color_eq(parse_color("0XABCDEF"), rgb(0xAB, 0xCD, 0xEF)));
    assert(color_eq(parse_color("255"), rgb(0, 0, 255)));
    assert(color_eq(parse_color("xyz"), rgb(0, 0, 0)));
    assert(parse_color("0x1565C0").a == 255);

    // --- discovery: hanya *.elf; desktop.elf hidden via manifest ---
    Launcher launcher;
    assert(launcher.discover() == true);
    assert(launcher.count() == 4);
    assert(t_streq(launcher.entry(0).label, "Explorer"));
    assert(t_streq(launcher.entry(0).elf, "/apps/fileman.elf"));
    assert(color_eq(launcher.entry(0).color, rgb(0x15, 0x65, 0xC0)));
    assert(t_streq(launcher.entry(0).icon, ""));
    assert(t_streq(launcher.entry(1).label, "Kalkulator"));
    assert(color_eq(launcher.entry(1).color, rgb(0x2E, 0x7D, 0x32)));
    assert(t_streq(launcher.entry(2).label, "badptr"));
    assert(t_streq(launcher.entry(2).elf, "/apps/badptr.elf"));
    assert(color_eq(launcher.entry(2).color, APP_DEFAULT));
    assert(t_streq(launcher.entry(3).label, "Demo"));
    assert(t_streq(launcher.entry(3).icon, "demo.png"));
    assert(launcher.discover() == false);  // tanpa perubahan FS

    // --- grid + hit ikon ---
    assert(Launcher::grid_cols(640) == (640 - ICON_X0) / CELL_W);
    assert(launcher.grid_cap(640, 480) == launcher.count());
    assert(Launcher::grid_cols(100) == 1);
    assert(launcher.grid_cap(100, 80) >= 1);
    assert(launcher.find_icon(pt(30, 30), 6, 3) == 0);  // dalam ikon 0
    assert(launcher.find_icon(pt(0, 0), 6, 3) == -1);   // luar ikon
    {
        Rect r0 = Launcher::icon_rect(0, 6);
        assert(r0.x == ICON_X0 && r0.y == ICON_Y0);
        assert(r0.width == ICON_SZ && r0.height == ICON_SZ);
        assert(r0.contains(pt(30, 30)) && !r0.contains(pt(0, 0)));
    }

    // --- event: terjemahan antrean mentah ---
    {
        g_evs[0] = {2, 100, 200, 0, 0};  // MOVE
        g_evs[1] = {3, 0, 1, 0, 0};      // CLICK kiri tekan
        g_evs[2] = {1, 65, 1, 0, 0};     // KEY 'A' + shift
        g_evs[3] = {6, 0, 0, 0, 3};      // WIN_CLOSE
        g_evs[4] = {4, 1, 0, 0, 0};      // SCROLL → None
        g_evs[5] = {7, 0, 0, 0, 0};      // WALLPAPER_RELOAD (syscall 84 LEGACY)
        g_evs[6] = {8, 1, 0, 0, 0};      // HOT_RELOAD(WALLPAPER) (syscall 85)
        g_evs[7] = {8, 2, 0, 0, 0};      // HOT_RELOAD(FONT) (syscall 85)
        g_evs[8] = {8, 99, 0, 0, 0};     // HOT_RELOAD tak dikenal -> None aman
        g_nev = 9;
        g_evidx = 0;
        EventPoller poller;
        Event e = no_event();
        assert(poller.poll(e) && e.type == EventType::MouseMove);
        assert(e.pos.x == 100 && e.pos.y == 200);
        assert(poller.poll(e) && e.type == EventType::MouseButton);
        assert(e.button == 0 && e.pressed);
        assert(poller.poll(e) && e.type == EventType::Key);
        assert(e.key == 65 && e.modifiers == 1);
        assert(poller.poll(e) && e.type == EventType::Quit);
        assert(e.window_id == 3);
        assert(poller.poll(e) && e.type == EventType::None);
        assert(poller.poll(e) && e.type == EventType::WallpaperReload);
        assert(poller.poll(e) && e.type == EventType::HotReload);
        assert(e.hot_target == (uint32_t)KZ_HOT_RELOAD_WALLPAPER);
        assert(poller.poll(e) && e.type == EventType::HotReload);
        assert(e.hot_target == (uint32_t)KZ_HOT_RELOAD_FONT);
        assert(poller.poll(e) && e.type == EventType::HotReload);
        assert(e.hot_target == 99);  // kernel tak pernah mengirim ini
                                     // (validasi target); shell mengabaikannya
        assert(!poller.poll(e));  // antrean habis
    }

    // --- window manager: salin + filter milik taskbar, aktivasi slot ---
    {
        WindowManager wm;
        assert(!wm.activate(0));  // id kosong ditolak aman
        assert(wm.activate(2));   // terjemahan slot+1 → slot mentah
        assert(g_last_activate == 1);

        Taskbar bar;
        assert(bar.poll(wm, launcher) == true);  // kosong → 2 window
        assert(bar.count() == 2);
        assert(bar.entry(1).focused);
        assert(bar.entry(0).is_desktop);
        // Slot tampil: hanya "term" (desktop difilter).
        assert(bar.slots() == 1);
        assert(bar.slot_window(0) == 1);
        assert(bar.slot_app(0) == 0);  // "term" tak cocok manifest -> default
        // Geometri slot 0 di 640x480: x=8, strip y=436.
        {
            Rect s0 = Taskbar::slot_rect(0, 640, 480);
            assert(s0.x == TB_PAD && s0.y == 480 - TB_H);
            assert(s0.width == TB_SLOT_W && s0.height == TB_H);
            Rect i0 = Taskbar::slot_icon_rect(0, 640, 480);
            assert(i0.width == TB_ICON_PX && i0.height == TB_ICON_PX);
            assert(i0.x == s0.x + (TB_SLOT_W - TB_ICON_PX) / 2);
        }
        assert(bar.find_slot(pt(10, 440), 640, 480) == 0);
        assert(bar.find_slot(pt(200, 440), 640, 480) == -1);  // area kosong
        assert(bar.find_slot(pt(10, 10), 640, 480) == -1);    // atas strip
        // Jam numerik dari RTC palsu.
        assert(t_streq(bar.clock_time(), "21:35"));
        assert(t_streq(bar.clock_date(), "21 09 2026"));
        // Hover: masuk slot -> true; sama -> false; pergi -> true + -1.
        assert(bar.update_hover(pt(10, 440), 640, 480) == true);
        assert(bar.hovered() == 0);
        assert(bar.update_hover(pt(10, 440), 640, 480) == false);
        assert(bar.update_hover(pt(300, 440), 640, 480) == true);
        assert(bar.hovered() == -1);
        assert(bar.poll(wm, launcher) == false);  // tanpa perubahan
        g_kw_focus2 = 1;
        assert(bar.poll(wm, launcher) == true);  // fokus berubah
        assert(!bar.entry(1).focused);
        g_kw_focus2 = 0;
        // Menit berganti -> Partial (jam saja).
        g_time[4] = 36;
        assert(bar.poll(wm, launcher) == true);
        assert(t_streq(bar.clock_time(), "21:36"));
        g_time[4] = 35;
        assert(bar.poll(wm, launcher) == true);  // kembali (tetap lapor)
        g_kw_fail = 1;
        assert(bar.poll(wm, launcher) == false);  // gagal → false, state utuh
        assert(bar.count() == 2);
        g_kw_fail = 0;
    }

    // --- Phase 9: resolusi ikon terpusat ---
    {
        char p[32];
        resolve_icon_path("", p, sizeof(p));
        assert(t_streq(p, "/default.png"));
        resolve_icon_path("demo.png", p, sizeof(p));
        assert(t_streq(p, "/demo.png"));
        resolve_icon_path("/x.png", p, sizeof(p));
        assert(t_streq(p, "/x.png"));
        // scale_nearest 2x2 -> 4x4: tiap blok 2x2 = sumber terdekat.
        uint32_t src[4] = {0xFF000001u, 0xFF000002u, 0xFF000003u,
                           0xFF000004u};
        uint32_t dst[16];
        scale_nearest(src, 2, 2, dst, 4, 4);
        assert(dst[0] == src[0] && dst[3] == src[1]);
        assert(dst[12] == src[2] && dst[15] == src[3]);
        assert(dst[5] == src[0] && dst[10] == src[3]);
        // scale_icon = box + unsharp: tepi abu jadi lebih kontras (100/140 ->
        // 93/146), sedangkan upscale murni tidak dipertajam (sama dengan
        // scale_nearest).
        uint32_t s4[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                s4[y * 4 + x] = (x < 2) ? 0xFF646464u : 0xFF8C8C8Cu;
        uint32_t soft[4], hard[4];
        scale_nearest(s4, 4, 4, soft, 2, 2);
        scale_icon(s4, 4, 4, hard, 2, 2);
        assert(soft[0] == 0xFF646464u && soft[1] == 0xFF8C8C8Cu);
        assert(hard[0] == 0xFF5D5D5Du && hard[1] == 0xFF929292u);
        uint32_t up2[16];
        scale_icon(src, 2, 2, up2, 4, 4);
        for (int i = 0; i < 16; i++) assert(up2[i] == dst[i]);
        // blit_px: RLE per baris — A A B B -> 2 fill_rect (w=2) + warna tepat.
        uint32_t row[4] = {0xFF102030u, 0xFF102030u, 0xFF405060u,
                           0xFF405060u};
        RectSink bs;
        bs.n = 0;
        blit_px(bs, 5, 7, row, 4, 1);
        assert(bs.n == 2);
        assert(bs.last.x == 7 && bs.last.y == 7 && bs.last.width == 2);
        assert(color_eq(bs.last_c, rgb(0x40, 0x50, 0x60)));
        // Cache: kustom ada, hilang -> default, semua gagal -> null.
        IconCache icons;
        assert(icons.icon_for("/demo.png") != 0);
        assert(icons.icon_for("/demo.png")->size == ICON_CACHE_PX);
        assert(icons.icon_for("/tak-ada.png") != 0);  // fallback default
        g_img_fail = 1;
        IconCache noimg;
        assert(noimg.icon_for("/tak-ada.png") == 0);
        g_img_fail = 0;
    }

    // --- Phase 9: cocok judul launcher + path ikon entri ---
    {
        char p[32];
        launcher.icon_path(3, p);
        assert(t_streq(p, "/demo.png"));
        launcher.icon_path(0, p);
        assert(t_streq(p, "/default.png"));
        const AppEntry* f = launcher.find_by_title("Explorer");
        assert(f && t_streq(f->elf, "/apps/fileman.elf"));
        const AppEntry* c = launcher.find_by_title("calc");  // basename elf
        assert(c && t_streq(c->label, "Kalkulator"));
        assert(launcher.find_by_title("term") == 0);
        assert(launcher.find_by_title("") == 0);
    }

    // --- Phase 9: wallpaper (pilih + fallback + base) ---
    {
        assert(Wallpaper::pick_builtin("island.png") == 0);
        assert(Wallpaper::pick_builtin("meadow.png") == 5);
        assert(Wallpaper::pick_builtin("asing.png") == -1);
        assert(Wallpaper::pick_builtin("") == -1);
        char wp[32];
        Wallpaper::config_path(wp, sizeof(wp), "island.png");
        assert(t_streq(wp, "/island.png"));
        Wallpaper wall;
        assert(!wall.has_image());
        assert(wall.load(640, 480) == true);  // desktop.app -> city-town.png
        assert(wall.has_image());
        // Blit foto = RLE fill_rect per baris, dibatasi region (dipakai render
        // Partial untuk memulihkan bekas kartu preview). Piksel uji seragam ->
        // satu run per baris.
        RectSink sink;
        sink.n = 0;
        Rect reg{16, 8, 32, 16};
        wall.draw_photo_into(sink, reg);
        assert(sink.n == 16);
        assert(sink.last.x == 16 && sink.last.y == 8 + 15);
        assert(sink.last.width == 32 && sink.last.height == 1);
        assert(color_eq(sink.last_c, rgb(255, 255, 255)));
        // Region melewati tepi layar: dipotong, tak digambar di luar.
        RectSink clip;
        clip.n = 0;
        Rect over{-10, -10, 40, 40};
        wall.draw_photo_into(clip, over);
        assert(clip.n == 30);  // hanya 30 baris terlihat (y 0..29)
        assert(clip.last.x == 0 && clip.last.y == 29 && clip.last.width == 30);
        // Region di luar layar: tak menggambar apa pun.
        RectSink empty;
        empty.n = 0;
        wall.draw_photo_into(empty, Rect{5000, 5000, 10, 10});
        assert(empty.n == 0);
        // Regresi BSOD INT 6 (Phase 9.5): buffer layar 1080p (8MB) sekarang
        // dari allocator standar — heap tumbuh, jadi load() tetap sukses.
        Wallpaper big;
        assert(big.load(1920, 1080) == true);
        RectSink bigsink;
        bigsink.n = 0;
        big.draw_photo_into(bigsink, Rect{0, 0, 1920, 1080});
        assert(bigsink.n == 1080);  // satu run per baris
        assert(bigsink.last.y == 1079 && bigsink.last.width == 1920);
        g_img_fail = 1;
        Wallpaper noimg;
        assert(noimg.load(640, 480) == false);  // fallback gradasi
        RectSink none;
        none.n = 0;
        noimg.draw_photo_into(none, Rect{0, 0, 64, 64});
        assert(none.n == 0);  // tanpa foto: draw_bg memakai gradasi
        g_img_fail = 0;
    }

    // --- Live reload wallpaper (poll): ganti -> true, sama -> false,
    // invalid -> false + gambar lama tetap. Pointer FS diganti sementara
    // (array FS sendiri mutable; literal tak disentuh) lalu dipulihkan.
    {
        Wallpaper live;
        assert(live.load(640, 480) == true);  // desktop.app -> city-town.png
        assert(t_streq(live.selected(), "city-town.png"));
        assert(live.poll(640, 480) == false);  // tak berubah: tanpa kerja
        FS[5].data = "name=Desktop\nhidden=1\nwallpaper=meadow.png\n";
        assert(live.poll(640, 480) == true);  // ganti -> reload
        assert(live.has_image());
        assert(t_streq(live.selected(), "meadow.png"));
        assert(live.poll(640, 480) == false);  // sudah sinkron
        FS[5].data = "name=Desktop\nhidden=1\nwallpaper=city-town.png\n";
        assert(live.poll(640, 480) == true);  // kembali -> reload lagi
        assert(t_streq(live.selected(), "city-town.png"));
        FS[5].data = "name=Desktop\nhidden=1\nwallpaper=asing.png\n";
        assert(live.poll(640, 480) == false);  // invalid: ditolak
        assert(live.has_image());  // gambar lama tetap tampil
        assert(t_streq(live.selected(), "city-town.png"));
        FS[5].data = "name=Desktop\nhidden=1\nwallpaper=city-town.png\n";
        assert(live.poll(640, 480) == false);  // pulih: sinkron lagi
    }

    // --- Override /wallpaper.ui mengalahkan manifest ---
    // (Berkas non-modul: tak ditulis ulang kernel saat boot, tidak seperti
    // /apps/desktop.app — jadi pilihan Settings selamat dari reboot.)
    {
        int wi = find_file("/wallpaper.ui");
        assert(wi >= 0);
        Wallpaper over;
        assert(over.load(640, 480) == true);  // manifest city-town
        assert(t_streq(over.selected(), "city-town.png"));
        FS[wi].data = "meadow.png\n";
        assert(over.poll(640, 480) == true);  // override menang
        assert(t_streq(over.selected(), "meadow.png"));
        FS[wi].data = "asing.png\n";  // invalid -> diabaikan
        assert(over.poll(640, 480) == true);  // jatuh ke manifest
        assert(t_streq(over.selected(), "city-town.png"));
        FS[wi].data = 0;  // pulihkan: absen lagi
        assert(over.poll(640, 480) == false);  // sinkron, tanpa kerja
    }

    // --- Phase 9: gambar launcher (ikon PNG via RLE ke canvas window) ---
    {
        IconCache icons;
        Canvas canvas;  // host: Impl kosong (fill_rect no-op) — hitung hasil
        assert(launcher.draw(canvas, icons, 640, 480) == 4);
        assert(launcher.draw(canvas, icons, 0, 0) == 1);  // kapasitas minimum
    }

    // --- Phase 9: taskbar + preview benar-benar menggambar ke canvas ---
    {
        WindowManager wm;
        Taskbar bar;
        assert(bar.poll(wm, launcher));
        IconCache icons;
        Canvas canvas;  // host: Impl kosong (fill_rect no-op)
        // Satu window tampil ("term") -> satu ikon bergambar digambar.
        assert(bar.draw(canvas, icons, 640, 480) == 1);
        AppPreview pv;
        Rect anchor = Taskbar::slot_rect(0, 640, 480);
        pv.show(bar.entry(1), bar.slot_app(0), anchor, 640, 480);
        assert(pv.drawn_rect().width == 0);  // belum digambar
        pv.draw(canvas, icons);
        assert(pv.drawn_rect().width == PV_W && pv.drawn_rect().height == PV_H);
        // Kartu hilang: rect terakhir DIPERTAHANKAN (dipakai render Partial
        // untuk memulihkan latar), lalu dikosongkan setelah draw berikutnya.
        Rect was = pv.drawn_rect();
        pv.hide();
        assert(pv.drawn_rect().width == PV_W);
        pv.draw(canvas, icons);
        assert(pv.drawn_rect().width == 0);
        assert(was.x == pv.drawn_rect().x && was.y == pv.drawn_rect().y);
    }

    // --- Phase 9: kartu preview (geometri + show/hide/hit) ---
    {
        WindowInfo w;
        w.id = 7;
        t_strcpy(w.title, "term");
        w.focused = 1;
        w.is_desktop = false;
        Rect anchor = Taskbar::slot_rect(0, 640, 480);
        Rect c = AppPreview::card_rect(anchor, 640, 480);
        assert(c.width == PV_W && c.height == PV_H);
        assert(c.y == anchor.y - PV_GAP - PV_H);  // di atas taskbar
        assert(c.x >= 0 && c.x + PV_W <= 640);
        AppPreview pv;
        assert(!pv.visible());
        pv.show(w, 0, anchor, 640, 480);
        assert(pv.visible() && pv.window_id() == 7);
        assert(pv.hit(pt(c.x + 4, c.y + 4)));
        assert(!pv.hit(pt(0, 0)));
        pv.hide();
        assert(!pv.visible() && !pv.hit(pt(c.x + 4, c.y + 4)));
        // Jepit kanan: anchor di tepi kanan layar.
        Rect far;
        far.x = 600;
        far.y = 436;
        far.width = 40;
        far.height = 44;
        Rect cc = AppPreview::card_rect(far, 640, 480);
        assert(cc.x + PV_W <= 640);
    }

    // --- notifikasi crash: probe/timeout/klik ---
    {
        CrashNotice notice;
        assert(!notice.probe());  // boot normal: tak tampil
        assert(!notice.visible());

        g_crash_mode = 1;
        assert(notice.probe());
        assert(notice.visible());
        assert(!notice.update(g_now));  // belum timeout

        int card_x = 1280 - NOTIF_W - NOTIF_MARGIN;
        // Klik luar kartu: tutup, klik diteruskan.
        assert(notice.on_click(pt(card_x - 1, NOTIF_MARGIN + 10), 1280) ==
               NoticeClick::Close);
        assert(!notice.visible());

        assert(notice.probe());  // tampil lagi
        g_spawns = 0;
        // Klik dalam kartu: tutup + buka File Manager, dikonsumsi.
        assert(notice.on_click(pt(card_x + 10, NOTIF_MARGIN + 10), 1280) ==
               NoticeClick::CloseAndOpenFm);
        assert(!notice.visible());
        assert(g_spawns == 1);
        assert(t_streq(g_last_spawn, "/apps/fileman.elf"));
        // Sudah tertutup: tak ada aksi ganda.
        assert(notice.on_click(pt(card_x + 10, NOTIF_MARGIN + 10), 1280) ==
               NoticeClick::None);
        assert(g_spawns == 1);

        // Timeout menutup + melapor sekali.
        assert(notice.probe());
        g_now += NOTIF_MS + 1;
        assert(notice.update(g_now) == true);
        assert(!notice.visible());
        assert(notice.update(g_now) == false);
        g_crash_mode = 0;
    }
    assert(NOTIF_MS >= 3000 && NOTIF_MS <= 15000);

    // --- Icon box + grid kolom-mayor (anti-overlap ala Windows) ---
    {
        assert(BOX_W == 84 && BOX_H == 100 && BOX_LBL_MAX == 10);
        assert(LABEL_FONT_PX == 13);  // freetype era (b8f86df); dulu 11 bitmap
        assert(Launcher::grid_rows(480) == (480 - TB_H - ICON_Y0) / CELL_H);
        assert(Launcher::grid_rows(0) == 1);
        int c = 0, r = 0;
        Launcher::box_grid_pos(0, 6, 3, &c, &r);
        assert(c == 0 && r == 0);
        Launcher::box_grid_pos(1, 6, 3, &c, &r);
        assert(c == 0 && r == 1);  // ke bawah dulu ...
        Launcher::box_grid_pos(2, 6, 3, &c, &r);
        assert(c == 0 && r == 2);
        Launcher::box_grid_pos(3, 6, 3, &c, &r);
        assert(c == 1 && r == 0);  // ... lalu kolom baru di kanan
        Rect b0 = launcher.box_rect(0, 6, 3);
        assert(b0.x == ICON_X0 && b0.y == ICON_Y0);
        assert(b0.width == BOX_W && b0.height == BOX_H);
        Rect b1 = launcher.box_rect(1, 6, 3);
        assert(b1.x == ICON_X0 && b1.y == ICON_Y0 + CELL_H);
        Rect b3 = launcher.box_rect(3, 6, 3);
        assert(b3.x == ICON_X0 + CELL_W && b3.y == ICON_Y0);
        // Klik area label ikut kena (dulu cuma kotak 48px).
        assert(launcher.find_icon_at(pt(ICON_X0 + 40, ICON_Y0 + 70), 640,
                                     480) == 0);
        // Celah antar box bukan milik siapa pun (tak ada tabrakan).
        assert(launcher.find_icon_at(pt(ICON_X0 + BOX_W + 8, ICON_Y0 + 10),
                                     640, 480) == -1);
        // Tak ada dua box saling menindih.
        {
            int cols = Launcher::grid_cols(640);
            int rows = Launcher::grid_rows(480);
            int cap = launcher.grid_cap(640, 480);
            for (int i = 0; i < cap; i++)
                for (int j = i + 1; j < cap; j++)
                    assert(!launcher.box_rect(i, cols, rows)
                                .intersects(launcher.box_rect(j, cols, rows)));
        }
        // Wrap 2 baris: kata utuh sebisa mungkin, "..." cuma di baris2.
        {
            char l1[16], l2[16];
            assert(Launcher::wrap_label("Demo", l1, l2, sizeof(l1)) == 1);
            assert(t_streq(l1, "Demo") && l2[0] == '\0');
            assert(Launcher::wrap_label("", l1, l2, sizeof(l1)) == 1);
            assert(l1[0] == '\0' && l2[0] == '\0');
            assert(Launcher::wrap_label("1234567890", l1, l2, sizeof(l1)) == 1);
            assert(t_streq(l1, "1234567890"));
            // Spasi -> baris baru tanpa elipsis.
            assert(Launcher::wrap_label("Very Long Name Here", l1, l2,
                                        sizeof(l1)) == 2);
            assert(t_streq(l1, "Very Long"));
            assert(t_streq(l2, "Name Here"));
            assert(Launcher::wrap_label("ab cd ef gh ij kl", l1, l2,
                                        sizeof(l1)) == 2);
            assert(t_streq(l1, "ab cd ef"));
            assert(t_streq(l2, "gh ij kl"));
            // Luber baris2 -> "..." menempel ("World" 5 + 3 = 8 muat).
            assert(Launcher::wrap_label("Hello World Extra", l1, l2,
                                        sizeof(l1)) == 2);
            assert(t_streq(l1, "Hello"));
            assert(t_streq(l2, "World..."));
            // Tanpa spasi: potong keras, baris2 elipsis 7 + "...".
            assert(Launcher::wrap_label("VeryLongApplicationName", l1, l2,
                                        sizeof(l1)) == 2);
            assert(t_streq(l1, "VeryLongAp"));
            assert(t_streq(l2, "plicati..."));
            // Kasus spek: 1 kata <= 10 char utuh 1 baris; spasi jadi 2 baris.
            assert(Launcher::wrap_label("Terminal", l1, l2, sizeof(l1)) == 1);
            assert(t_streq(l1, "Terminal"));
            assert(Launcher::wrap_label("Kalkulator", l1, l2, sizeof(l1)) == 1);
            assert(t_streq(l1, "Kalkulator"));
            assert(Launcher::wrap_label("Image Viewer", l1, l2, sizeof(l1)) ==
                   2);
            assert(t_streq(l1, "Image"));
            assert(t_streq(l2, "Viewer"));
            assert(Launcher::wrap_label("Control Center", l1, l2, sizeof(l1)) ==
                   2);
            assert(t_streq(l1, "Control"));
            assert(t_streq(l2, "Center"));
            // 1 kata 13 char: character-wrap tanpa elipsis (sisa muat).
            assert(Launcher::wrap_label("ControlCenter", l1, l2, sizeof(l1)) ==
                   2);
            assert(t_streq(l1, "ControlCen"));
            assert(t_streq(l2, "ter"));
        }
        // State hover/selected terbaca via describe().
        {
            launcher.set_hover(1);
            launcher.set_selected(2);
            DesktopIcon di;
            launcher.describe(0, 6, 3, &di);
            assert(di.state == ICON_ST_NORMAL);
            launcher.describe(1, 6, 3, &di);
            assert(di.state == ICON_ST_HOVER);
            launcher.describe(2, 6, 3, &di);
            assert(di.state == ICON_ST_SELECTED);
            launcher.set_hover(-1);
            launcher.set_selected(-1);
        }
        // Sort A-Z + posisi kustom (free drag) + kembali auto.
        {
            launcher.sort_by_name();
            assert(t_streq(launcher.entry(0).label, "Demo"));
            assert(t_streq(launcher.entry(1).label, "Explorer"));
            assert(t_streq(launcher.entry(2).label, "Kalkulator"));
            assert(t_streq(launcher.entry(3).label, "badptr"));
            assert(launcher.auto_arrange() == true);
            launcher.set_auto_arrange(false);
            launcher.set_custom_pos(0, 200, 200);
            Rect bc = launcher.box_rect(0, 6, 3);
            assert(bc.x == 200 && bc.y == 200);
            assert(bc.width == BOX_W && bc.height == BOX_H);
            assert(launcher.find_icon_at(pt(210, 210), 640, 480) == 0);
            launcher.set_auto_arrange(true);  // buang posisi kustom
            Rect bg = launcher.box_rect(0, 6, 3);
            assert(bg.x == ICON_X0 && bg.y == ICON_Y0);
        }
    }

    // --- Shell: seleksi, menu konteks kanan, free drag (canvas null) ---
    {
        DesktopShell sh;
        Canvas cv;  // host: null (w=h=0) — strip dianggap tak ada
        sh.on_start(cv);
        assert(sh.launcher().count() == 4);
        Event e = no_event();
        e.type = EventType::MouseMove;
        e.pos = pt(30, 30);
        sh.on_event(e, cv);
        assert(sh.launcher().hovered() == 0);  // hover box ikon 0
        // Klik kanan di ikon -> menu app (Open/Properties), top-most.
        e.type = EventType::MouseButton;
        e.button = 1;
        e.pressed = true;
        sh.on_event(e, cv);
        assert(sh.menu_open() && sh.menu_icon() == 0);
        assert(sh.menu_count() == 2);
        assert(sh.launcher().selected() == 0);
        Rect mr = sh.menu_rect(0, 0);
        assert(mr.width == MENU_W && mr.height == 2 * MENU_ROW_H + 8);
        assert(sh.menu_row_at(pt(10, 15), 0, 0) == 0);
        assert(sh.menu_row_at(pt(10, 40), 0, 0) == 1);
        assert(sh.menu_row_at(pt(500, 500), 0, 0) == -1);
        // Klik kiri item Open -> spawn + menu tutup.
        int base = g_spawns;
        e.button = 0;
        e.pressed = true;
        // kursor masih (30,30): pindahkan dulu ke baris menu.
        Event mv = no_event();
        mv.type = EventType::MouseMove;
        mv.pos = pt(10, 15);
        sh.on_event(mv, cv);
        assert(sh.menu_hover() == 0);
        sh.on_event(e, cv);
        assert(!sh.menu_open());
        assert(g_spawns == base + 1);
        assert(t_streq(g_last_spawn, "/apps/fileman.elf"));
        // Klik kanan area kosong -> menu desktop (3 item).
        mv.pos = pt(500, 500);
        sh.on_event(mv, cv);
        sh.on_event(e, cv);  // kiri di kosong: batalkan seleksi
        assert(sh.launcher().selected() == -1);
        e.button = 1;
        sh.on_event(e, cv);
        assert(sh.menu_open() && sh.menu_icon() == -1);
        assert(sh.menu_count() == 3);
        // Klik kiri baris 0 -> toggle auto-arrange mati.
        mv.pos = pt(10, 15);
        sh.on_event(mv, cv);
        e.button = 0;
        sh.on_event(e, cv);
        assert(!sh.menu_open());
        assert(sh.launcher().auto_arrange() == false);
        // Klik kiri di luar menu menutup tanpa aksi.
        e.button = 1;
        mv.pos = pt(500, 500);
        sh.on_event(mv, cv);
        sh.on_event(e, cv);
        assert(sh.menu_open());
        e.button = 0;
        mv.pos = pt(500, 500);
        sh.on_event(mv, cv);
        sh.on_event(e, cv);
        assert(!sh.menu_open());
        assert(sh.launcher().auto_arrange() == false);  // tak berubah
        // Free drag: seleksi lalu seret ikon 0 ke (100,100).
        mv.pos = pt(30, 30);
        sh.on_event(mv, cv);
        sh.on_event(e, cv);  // klik pertama = seleksi + mulai drag
        assert(sh.launcher().selected() == 0);
        assert(g_spawns == base + 1);  // belum spawn
        mv.pos = pt(100, 100);
        sh.on_event(mv, cv);
        Rect db = sh.launcher().box_rect(0, 1, 1);
        assert(db.x == 100 - BOX_W / 2 && db.y == 100 - BOX_ICON_Y - ICON_SZ / 2);
        e.pressed = false;  // lepas kiri = akhir drag
        sh.on_event(e, cv);
        e.pressed = true;
        // Klik kedua di posisi baru = buka.
        mv.pos = pt(100, 100);
        sh.on_event(mv, cv);
        sh.on_event(e, cv);
        assert(g_spawns == base + 2);
        assert(t_streq(g_last_spawn, "/apps/fileman.elf"));
    }

    // --- Shell: Hot Reload generik (syscall 85) ---
    // Canvas host null (w=h=0): reload wallpaper ditolak aman (tanpa kerja,
    // tanpa crash, wallpaper lama utuh). Target tak dikenal = None.
    // FONT lewat ui_font_poll (fontUI stub: file font absen = fallback).
    {
        DesktopShell sh;
        Canvas cv;  // host: null (w=h=0)
        sh.on_start(cv);
        Event hr = no_event();
        hr.type = EventType::HotReload;
        hr.hot_target = (uint32_t)KZ_HOT_RELOAD_WALLPAPER;
        assert(sh.on_event(hr, cv) == Damage::None);  // guard ukuran
        hr.hot_target = 99;  // tak dikenal -> abaikan aman
        assert(sh.on_event(hr, cv) == Damage::None);
        hr.hot_target = (uint32_t)KZ_HOT_RELOAD_FONT;
        assert(sh.on_event(hr, cv) == Damage::None);  // font absen: fallback
        // LEGACY syscall 84: jalur kanonis sama, guard sama.
        Event legacy = no_event();
        legacy.type = EventType::WallpaperReload;
        assert(sh.on_event(legacy, cv) == Damage::None);
    }

    // --- Shell: on_poll menyapu zombie (WNOHANG, tak menggantung) ---
    {
        DesktopShell sh;
        Canvas cv;  // host: null
        sh.on_start(cv);
        g_zombies = 3;
        int calls0 = g_waitpid_calls;
        sh.on_poll(cv);  // harus kembali: 3 reap + 1 terminator
        assert(g_zombies == 0);
        assert(g_waitpid_calls == calls0 + 4);
        g_zombies = 0;
        int calls1 = g_waitpid_calls;
        sh.on_poll(cv);  // tanpa child: sapu no-op, tak menggantung
        assert(g_waitpid_calls == calls1 + 1);
    }

    printf("desktop phase8: OK\n");
    return 0;
}
