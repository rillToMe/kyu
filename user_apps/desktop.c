// user_apps/desktop.c — Desktop Environment (Phase 10): shell DE.
//
// Wallpaper + launcher ikon + taskbar. Memakai libgui (renderer murni,
// bukan pohon widget) karena butuh kontrol penuh event loop: render HANYA
// saat berubah (full-screen canvas mahal — jangan per-frame). Taskbar
// poll daftar window (sys_kwm_get_windows) tiap ~10 iterasi; klik taskbar
// → sys_kwm_activate_window (bring-to-front + fokus).
//
// Launcher TIDAK hardcode daftar app: discover_apps() men-scan KyuzenFS
// (sys_get_file_list) untuk tiap *.elf, lalu membaca manifest "<base>.app"
// (key=value) untuk name/color/hidden. Tanpa manifest → label = nama file
// tanpa ekstensi, warna netral.
//
// KyuzenFS direktori (Fase 3): app binary + manifest pindah ke /apps/. Scan
// /apps, baca manifest "/apps/<base>.app". e->elf menyimpan path lengkap
// "/apps/<nama>" (build_app_path) — spawn langsung, tanpa resolve ulang.
//
// Build: desktop.o + userlib.o + libgui.o

#include "userlib.h"
#include "libgui.h"

#define KWM_WIN_DESKTOP 0x1   // mirror kernel kwm_internal.h (filter taskbar)

// --- geometri (grid 8px) ---
#define TB_H     36       // tinggi taskbar (baris bawah)
#define CELL_W   92       // lebar sel ikon launcher
#define CELL_H   100      // tinggi sel (ikon 56 + label 16 + gap)
#define ICON_SZ  56
#define ICON_X0  24
#define ICON_Y0  24
#define LBL_MAX  (CELL_W / 8)   // char label per sel (font 8px), sisanya dipotong

// --- warna ---
#define WALL_BG     0x141A2E
#define WALL_TXT    0x3A4160
#define TASK_BG     0x0B0E1C
#define TASK_EDGE   0x2A3355
#define TASK_BTN    0x1A2138
#define TASK_ACTIVE 0x2E4A8E
#define ICON_TXT    0xC0C8E0
#define APP_DEFAULT 0x37474F    // app tanpa manifest (abu netral)

// --- daftar app (hasil scan runtime, bukan compile-time) ---
#define MAX_APPS     32
#define MAX_FILES    64
#define MANIFEST_MAX 512
typedef struct {
    char     label[32];
    char     elf[32];     // path lengkap "/apps/<nama>" (maks 6+22+1)
    uint32_t color;
} AppEntry;
static AppEntry g_apps[MAX_APPS];
static int      g_napps = 0;
static uint32_t g_apps_sum = 0;   // checksum hasil scan → deteksi perubahan

// --- window list (taskbar) ---
#define MAX_WINS 16
static kwm_window_info_t g_wins[MAX_WINS];
static int g_nwins = 0;

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// Bandingkan n byte (kunci manifest / ekstensi file) — tanpa libc.
static int neq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// "0x1565C0" (atau desimal) → RGB. Berhenti di karakter non-digit, jadi aman
// dipanggil pada nilai yang belum di-NUL (langsung menunjuk ke tengah buffer).
static uint32_t parse_color(const char* s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (int i = 2; ; i++) {
            char c = s[i]; int d;
            if (c >= '0' && c <= '9')      d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + (uint32_t)d;
        }
    } else {
        for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint32_t)(s[i] - '0');
    }
    return v & 0xFFFFFF;
}

// Manifest "<base>.app": satu "key=value" per baris. Dikenali: name, color,
// hidden. Baris tanpa '=' diabaikan (komentar/kosong). `exec` TIDAK dipakai —
// binary sudah didapat dari scan, manifest dipasangkan lewat nama file.
static void parse_manifest(const char* b, AppEntry* e, int* hidden) {
    int i = 0;
    while (b[i]) {
        int ls = i;
        while (b[i] && b[i] != '\n') i++;
        int le = i;                      // [ls, le) = satu baris
        if (b[i]) i++;
        int eq = ls;
        while (eq < le && b[eq] != '=') eq++;
        if (eq >= le) continue;
        const char* k = &b[ls];
        int klen = eq - ls;
        const char* v = &b[eq + 1];
        int vlen = le - (eq + 1);
        while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' ')) vlen--;
        if (klen == 4 && neq(k, "name", 4)) {
            int n = vlen > 31 ? 31 : vlen;
            for (int j = 0; j < n; j++) e->label[j] = v[j];
            e->label[n] = '\0';
        } else if (klen == 5 && neq(k, "color", 5)) {
            e->color = parse_color(v);
        } else if (klen == 6 && neq(k, "hidden", 6)) {
            *hidden = (vlen > 0 && v[0] != '0');
        }
    }
}

// Scan FS: tiap *.elf jadi satu entri launcher, di-enrich manifest bila ada.
// Return 1 bila hasil scan beda dari sebelumnya (perlu re-render).
static int discover_apps(void) {
    static file_info_t files[MAX_FILES];
    static char mbuf[MANIFEST_MAX];
    int n = sys_get_file_list("/apps", files, MAX_FILES);
    if (n < 0) n = 0;

    g_napps = 0;
    for (int i = 0; i < n && g_napps < MAX_APPS; i++) {
        const char* fn = files[i].filename;
        int L = slen(fn);
        if (L < 5 || L > 22 || !neq(fn + L - 4, ".elf", 4)) continue;

        AppEntry* e = &g_apps[g_napps];
        int base = L - 4;                       // nama tanpa ".elf"
        for (int j = 0; j < base; j++) e->label[j] = fn[j];
        e->label[base] = '\0';
        build_app_path(e->elf, sizeof(e->elf), fn);
        e->color = APP_DEFAULT;

        char man[32];                           // "/apps/<base>.app" (Fase 3)
        int m = 0;
        const char* ap = "/apps/";
        while (ap[m]) { man[m] = ap[m]; m++; }
        for (int j = 0; j < base && m < 30; j++) man[m++] = fn[j];
        man[m++] = '.'; man[m++] = 'a'; man[m++] = 'p'; man[m++] = 'p';
        man[m] = '\0';

        int hidden = 0;
        if (sys_file_exists(man)) {
            memset(mbuf, 0, sizeof(mbuf));      // syscall 13 tidak menutup NUL
            if (sys_read_file_to_buffer(man, mbuf, sizeof(mbuf) - 1))
                parse_manifest(mbuf, e, &hidden);
        }
        if (!hidden) g_napps++;                 // hidden → slot dipakai ulang
    }

    uint32_t s = (uint32_t)g_napps;
    for (int i = 0; i < g_napps; i++) {
        s = s * 31 + g_apps[i].color;
        for (int j = 0; g_apps[i].label[j]; j++) s = s * 31 + (uint8_t)g_apps[i].label[j];
    }
    int changed = (s != g_apps_sum);
    g_apps_sum = s;
    return changed;
}

// Kolom & kapasitas grid yang muat di layar (di atas taskbar).
// ponytail: app di luar kapasitas tidak digambar; scrolling launcher = TODO.
static int grid_cols(const gui_window_t* d) {
    int c = ((int)d->width - ICON_X0) / CELL_W;
    return c < 1 ? 1 : c;
}
static int grid_cap(const gui_window_t* d) {
    int rows = ((int)d->height - TB_H - ICON_Y0) / CELL_H;
    if (rows < 1) rows = 1;
    int cap = rows * grid_cols(d);
    return cap > g_napps ? g_napps : cap;
}

// Poll daftar window; return 1 bila ada perubahan (tampilan taskbar berubah).
static int winlist_changed(void) {
    kwm_window_info_t tmp[MAX_WINS];
    int n = sys_kwm_get_windows(tmp, MAX_WINS);
    if (n < 0) return 0;
    int changed = (n != g_nwins);
    if (!changed) {
        for (int i = 0; i < n; i++) {
            if (g_wins[i].win_id != tmp[i].win_id ||
                g_wins[i].focused != tmp[i].focused ||
                g_wins[i].flags  != tmp[i].flags) { changed = 1; break; }
            for (int j = 0; j < 32; j++)
                if (g_wins[i].title[j] != tmp[i].title[j]) { changed = 1; break; }
            if (changed) break;
        }
    }
    g_nwins = n;
    for (int i = 0; i < n; i++) g_wins[i] = tmp[i];
    return changed;
}

static void draw_icon(gui_window_t* d, int i, int cols) {
    int cx = ICON_X0 + (i % cols) * CELL_W;
    int cy = ICON_Y0 + (i / cols) * CELL_H;
    gui_draw_rect(d, cx, cy, ICON_SZ, ICON_SZ, g_apps[i].color);
    char lbl[LBL_MAX + 1];              // potong label panjang: jangan tabrakan
    int n = slen(g_apps[i].label);
    if (n > LBL_MAX) n = LBL_MAX;
    for (int j = 0; j < n; j++) lbl[j] = g_apps[i].label[j];
    lbl[n] = '\0';
    gui_draw_text(d, lbl, cx + (ICON_SZ - n * 8) / 2, cy + ICON_SZ + 4, ICON_TXT);
}

// Phase 4: wallpaper + ikon (statis; hanya berubah saat discover/screen).
static void render_wallpaper(gui_window_t* d) {
    int W = (int)d->width, H = (int)d->height;
    gui_draw_rect(d, 0, 0, W, H - TB_H, WALL_BG);
    gui_draw_text(d, "KyuzenOS", W - 8 * 8 - 16, 12, WALL_TXT);
    int cols = grid_cols(d), cap = grid_cap(d);
    for (int i = 0; i < cap; i++) draw_icon(d, i, cols);
}

// Phase 4: taskbar saja — damage bbox = strip bawah, bukan layar penuh.
static void render_taskbar(gui_window_t* d) {
    int W = (int)d->width, H = (int)d->height;
    gui_draw_rect(d, 0, H - TB_H, W, TB_H, TASK_BG);
    gui_draw_rect(d, 0, H - TB_H, W, 1, TASK_EDGE);
    int bx = 8;
    for (int i = 0; i < g_nwins; i++) {
        if (g_wins[i].flags & KWM_WIN_DESKTOP) continue;
        if (!g_wins[i].title[0]) continue;
        int bw = slen(g_wins[i].title) * 8 + 20;
        gui_draw_rect(d, bx, H - TB_H + 4, bw, TB_H - 8,
                      g_wins[i].focused ? TASK_ACTIVE : TASK_BTN);
        gui_draw_text(d, g_wins[i].title, bx + 10, H - TB_H + 9, 0xE0E0E0);
        bx += bw + 6;
    }
}

static void render(gui_window_t* d) {
    render_wallpaper(d);
    render_taskbar(d);
}

static void handle_click(gui_window_t* d, int mx, int my) {
    int H = (int)d->height;
    // taskbar → aktivasi window
    if (my >= H - TB_H) {
        int bx = 8;
        for (int i = 0; i < g_nwins; i++) {
            if (g_wins[i].flags & KWM_WIN_DESKTOP) continue;
            if (!g_wins[i].title[0]) continue;
            int bw = slen(g_wins[i].title) * 8 + 20;
            if (mx >= bx && mx < bx + bw) {
                sys_kwm_activate_window((int)g_wins[i].win_id - 1);
                return;
            }
            bx += bw + 6;
        }
        return;
    }
    // launcher ikon → spawn app
    int cols = grid_cols(d), cap = grid_cap(d);
    for (int i = 0; i < cap; i++) {
        int cx = ICON_X0 + (i % cols) * CELL_W;
        int cy = ICON_Y0 + (i / cols) * CELL_H;
        if (mx >= cx && mx < cx + ICON_SZ && my >= cy && my < cy + ICON_SZ) {
            sys_spawn(g_apps[i].elf);
            return;
        }
    }
}

void main(void) {
    gui_window_t* d = gui_create_desktop();
    if (!d) { sys_exit(); }

    discover_apps();
    uint64_t last_scan = sys_uptime();

    kyuzen_event_t ev;
    int frame = 0;
    int need_full = 1;       // render awal (wallpaper + taskbar)
    int need_taskbar = 0;    // hanya strip taskbar yang berubah

    while (d->is_running) {
        if (sys_get_event(&ev)) {
            if (ev.type == EVENT_MOUSE_MOVE) {
                // Phase 4: gerak pointer saja tidak mengubah tampilan apa pun
                // (tak ada hover state) → TIDAK memicu repaint.
                d->mouse_x = ev.param1;
                d->mouse_y = ev.param2;
            } else if (ev.type == EVENT_MOUSE_CLICK && ev.param1 == 0 && ev.param2 == 1) {
                // Klik bisa mengubah fokus taskbar / spawn app → taskbar repaint.
                handle_click(d, d->mouse_x, d->mouse_y);
                need_taskbar = 1;
            }
        }
        // poll window list ~10Hz (bukan callback — tanpa infra notifikasi kernel)
        if (++frame % 10 == 0) {
            if (winlist_changed()) need_taskbar = 1;   // fokus/title/daftar berubah
            // re-scan app tiap ~5s: app baru di FS langsung muncul di launcher.
            uint64_t now = sys_uptime();
            if (now - last_scan >= 5000) {
                last_scan = now;
                if (discover_apps()) need_full = 1;    // grid ikon berubah
            }
        }

        if (need_full) {
            render(d);                 // wallpaper + taskbar, damage = layar penuh
            gui_flush(d);
            need_full = 0;
            need_taskbar = 0;
        } else if (need_taskbar) {
            render_taskbar(d);         // damage bbox = strip taskbar saja
            gui_flush(d);
            need_taskbar = 0;
        }
        sys_yield();
    }
    sys_exit();
}
