// Host test desktop Phase 8: Launcher + CrashNotice + Taskbar
// (apps/desktop) serta Event/WM/System/geometri (libdesktop).
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
#include "launcher.hpp"
#include "taskbar.hpp"
#include "theme.hpp"

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
    {"desktop.app", "name=Desktop\nhidden=1\n"},
    {"badptr.elf", 0},
    {"kyuzen.png", 0},
    {"notes.txt", 0},
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

struct RawEv {
    int type, p1, p2, p3, win;
};
static RawEv g_evs[8];
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
    assert(launcher.count() == 3);
    assert(t_streq(launcher.entry(0).label, "Explorer"));
    assert(t_streq(launcher.entry(0).elf, "/apps/fileman.elf"));
    assert(color_eq(launcher.entry(0).color, rgb(0x15, 0x65, 0xC0)));
    assert(t_streq(launcher.entry(1).label, "Kalkulator"));
    assert(color_eq(launcher.entry(1).color, rgb(0x2E, 0x7D, 0x32)));
    assert(t_streq(launcher.entry(2).label, "badptr"));
    assert(t_streq(launcher.entry(2).elf, "/apps/badptr.elf"));
    assert(color_eq(launcher.entry(2).color, APP_DEFAULT));
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
        g_nev = 5;
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
        assert(!poller.poll(e));  // antrean habis
    }

    // --- window manager: salin + filter milik taskbar, aktivasi slot ---
    {
        WindowManager wm;
        assert(!wm.activate(0));  // id kosong ditolak aman
        assert(wm.activate(2));   // terjemahan slot+1 → slot mentah
        assert(g_last_activate == 1);

        Taskbar bar;
        assert(bar.poll(wm) == true);  // kosong → 2 window
        assert(bar.count() == 2);
        assert(bar.entry(1).focused);
        assert(bar.entry(0).is_desktop);
        assert(bar.find_button(pt(10, 0)) == 1);    // tombol "term" (indeks 1;
                                                     // 0 = window desktop)
        assert(bar.find_button(pt(200, 0)) == -1);  // area kosong
        assert(bar.poll(wm) == false);              // tanpa perubahan
        g_kw_focus2 = 1;
        assert(bar.poll(wm) == true);  // fokus berubah
        assert(!bar.entry(1).focused);
        g_kw_focus2 = 0;
        g_kw_fail = 1;
        assert(bar.poll(wm) == false);  // gagal → false, state utuh
        assert(bar.count() == 2);
        g_kw_fail = 0;
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

    printf("desktop phase8: OK\n");
    return 0;
}
