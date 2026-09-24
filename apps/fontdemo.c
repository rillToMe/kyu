// ============================================================
// fontdemo.c — Font Demo (FreeType 2.14.3, libs/text + FtText).
//
// Menampilkan 5 font embedded (ld -b binary, memory blob ->
// FT_New_Memory_Face) pada 5 ukuran + garis UTF-8 + palet glyph,
// dengan ganti font runtime (tombol [<]/[>] atau panah Left/Right).
//
// Rendering: kz_text_draw (coverage 8-bit -> color_blend_alpha
// existing) di dalam callback ui_fttext_draw_cb; damage via
// ui_fttext_refresh (mark_dirty -> Window::render -> gui_flush).
// Fallback: bila font gagal dimuat, label bitmap existing menjelaskan.
// Jejak COM1 "[fontdemo]" untuk verifikasi headless QEMU.
//
// Build: fontdemo.o + userlib.o + libgui.o + libui.o + libtext +
//        libfreetype_kyuzen.a + 5 blob .o (lihat apps/Makefile).
// ============================================================
#include "userlib.h"
#include "libui.h"
#include "libgui.h"
#include "kzfont.h"
#include "kzfonts.h"   // display names + urutan (registry tunggal)

// --- Blob embedded (simbol dari ld -b binary; nama pasti dicek
// --- via llvm-nm saat build pertama, dipetakan di sini) ---
extern const uint8_t _binary____assets_fonts_DejaVuSans_ttf_start[];
extern const uint8_t _binary____assets_fonts_DejaVuSans_ttf_end[];
extern const uint8_t _binary____assets_fonts_Inter_Regular_ttf_start[];
extern const uint8_t _binary____assets_fonts_Inter_Regular_ttf_end[];
extern const uint8_t _binary____assets_fonts_NotoSansAdlam_Regular_ttf_start[];
extern const uint8_t _binary____assets_fonts_NotoSansAdlam_Regular_ttf_end[];
extern const uint8_t _binary____assets_fonts_NotoSansMono_Regular_ttf_start[];
extern const uint8_t _binary____assets_fonts_NotoSansMono_Regular_ttf_end[];
extern const uint8_t _binary____assets_fonts_NotoSansMono_Bold_ttf_start[];
extern const uint8_t _binary____assets_fonts_NotoSansMono_Bold_ttf_end[];

typedef struct {
    const char *name;
    const uint8_t *data;
    const uint8_t *end;
} font_entry_t;

static const font_entry_t g_fonts[] = {
    // Nama + urutan dari registry tunggal (kzfonts.h); blob tetap
    // embedded khusus demo (memory -> FT_New_Memory_Face, terverifikasi).
    // Urutan == KZ_FONT_* (Inter default UI di index 0).
    { KZ_FONT_NAMES[0], _binary____assets_fonts_Inter_Regular_ttf_start,
      _binary____assets_fonts_Inter_Regular_ttf_end },
    { KZ_FONT_NAMES[1], _binary____assets_fonts_DejaVuSans_ttf_start,
      _binary____assets_fonts_DejaVuSans_ttf_end },
    { KZ_FONT_NAMES[2],
      _binary____assets_fonts_NotoSansMono_Regular_ttf_start,
      _binary____assets_fonts_NotoSansMono_Regular_ttf_end },
    { KZ_FONT_NAMES[3], _binary____assets_fonts_NotoSansMono_Bold_ttf_start,
      _binary____assets_fonts_NotoSansMono_Bold_ttf_end },
    { KZ_FONT_NAMES[4],
      _binary____assets_fonts_NotoSansAdlam_Regular_ttf_start,
      _binary____assets_fonts_NotoSansAdlam_Regular_ttf_end },
};
#define N_FONTS 5
// KZ_FONT_COUNT adalah enum (bukan makro) — cek sinkron via assert
// compile-time, bukan #if.
_Static_assert(N_FONTS == KZ_FONT_COUNT,
               "g_fonts harus sinkron dengan kzfonts.h");

static const uint32_t g_sizes[] = { 12, 16, 20, 24, 32 };
#define N_SIZES 5

// --- State ---
static ui_window_t *g_win;
static ui_widget_t *g_font_label;
static ui_widget_t *g_status_label;
static ui_widget_t *g_lines[N_SIZES];  // satu FtText per ukuran
static ui_widget_t *g_utf8_line;
static ui_widget_t *g_palette_line;
static kz_font_t *g_fonth[N_SIZES];  // satu handle per ukuran (cache key px)
static int g_idx = 0;
static int g_ok = 0;
static int g_traced = 0;  // stats sudah dilaporkan untuk font aktif

// Satu baris demo: teks + ukuran + warna (userdata callback).
typedef struct {
    const char *text;
    int slot;  // index g_sizes/g_fonth
} line_t;

static line_t g_line_info[N_SIZES];
static line_t g_utf8_info = { "\xC3\xA9\xC3\xA0\xC3\xB6\xC3\xB1 \xE4\xB8\xAD?", 1 };
static line_t g_palette_info = { "ABgMW08@&?", 2 };

// --- Helper teks (tanpa libc) ---
static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void trace(const char *tag, const char *arg, int num) {
    print((char *)"[fontdemo] ");
    print((char *)tag);
    if (arg) {
        print((char *)" ");
        print((char *)arg);
    }
    if (num >= 0) {
        print((char *)" = ");
        print_num((uint32_t)num);
    }
    print((char *)"\n");
}

// --- kz heap di atas sys_alloc (allocator Kyuzen existing) ---
static void *fd_alloc(uint32_t n) { return sys_alloc(n ? n : 1); }
static void fd_free(void *p) { sys_free(p); }
static void *fd_realloc(void *p, uint32_t o, uint32_t n) {
    return sys_realloc(p, o, n);
}

// --- Callback gambar FtText: raster FreeType -> canvas ---
static void draw_line(void *ud, uint32_t *canvas, int cw, int ch, int x,
                      int y) {
    line_t *ln = (line_t *)ud;
    if (!g_ok || !ln || ln->slot < 0 || ln->slot >= N_SIZES) return;
    kz_font_t *f = g_fonth[ln->slot];
    if (!f) return;
    color_t fg = COLOR_RGB(0xE8, 0xE8, 0xEC);
    int baseline = y + (int)g_sizes[ln->slot] + 4;
    kz_text_draw(canvas, (uint32_t)cw, (uint32_t)ch, f, x + 4, baseline, fg,
                 ln->text, 0);
    // Laporkan cache 16px sekali, setelah render pertama font aktif
    // (trace di switch_font terlalu dini — font baru 0/0/0).
    if (!g_traced && ln->slot == 1 && g_fonth[1]) {
        uint32_t l = 0, h = 0, m = 0;
        kz_font_stats(g_fonth[1], &l, &h, &m);
        if (l > 0) {
            g_traced = 1;
            trace("cache16 lookups", 0, (int)l);
            trace("cache16 hits", 0, (int)h);
            trace("cache16 misses", 0, (int)m);
        }
    }
}

static void destroy_fonts(void) {
    for (int i = 0; i < N_SIZES; i++) {
        if (g_fonth[i]) {
            kz_font_destroy(g_fonth[i]);
            g_fonth[i] = 0;
        }
    }
    g_ok = 0;
}

// Muat font g_idx untuk semua ukuran. Return 1 bila minimal satu ok.
static int load_fonts(void) {
    destroy_fonts();
    kz_heap_t heap = { fd_alloc, fd_free, fd_realloc };
    kz_font_blob_t blob;
    blob.data = g_fonts[g_idx].data;
    blob.size = (uint32_t)(g_fonts[g_idx].end - g_fonts[g_idx].data);
    if (!blob.data || blob.size < 1000) return 0;
    int ok = 0;
    for (int i = 0; i < N_SIZES; i++) {
        kz_font_t *f = kz_font_load(&blob, &heap, &kz_ft_backend);
        if (f && kz_font_set_size(f, g_sizes[i]) == 0) {
            g_fonth[i] = f;
            ok = 1;
        } else if (f) {
            kz_font_destroy(f);
        }
    }
    g_ok = ok;
    return ok;
}

static void refresh_all(void) {
    for (int i = 0; i < N_SIZES; i++) ui_fttext_refresh(g_lines[i]);
    ui_fttext_refresh(g_utf8_line);
    ui_fttext_refresh(g_palette_line);
}

// Status bitmap: nama font + cache 16px + ukuran blob.
static void refresh_status(void) {
    char buf[160];
    int k = 0;
    const char *nm = g_fonts[g_idx].name;
    buf[k++] = (char)('1' + g_idx);
    buf[k++] = '/';
    buf[k++] = (char)('0' + N_FONTS);
    buf[k++] = ' ';
    for (int i = 0; nm[i] && k < 60; i++) buf[k++] = nm[i];
    uint32_t sz = (uint32_t)(g_fonts[g_idx].end - g_fonts[g_idx].data);
    buf[k++] = ' ';
    buf[k++] = '(';
    // ukuran KB desimal sederhana
    uint32_t kb = sz / 1024;
    char nb[8];
    int nl = 0;
    if (kb == 0) nb[nl++] = '0';
    else {
        char tr[8];
        int tl = 0;
        while (kb > 0 && tl < 7) {
            tr[tl++] = (char)('0' + kb % 10);
            kb /= 10;
        }
        while (tl > 0) nb[nl++] = tr[--tl];
    }
    for (int i = 0; i < nl && k < 100; i++) buf[k++] = nb[i];
    buf[k++] = 'K';
    buf[k++] = 'B';
    buf[k++] = ')';
    if (g_ok && g_fonth[1]) {
        uint32_t l = 0, h = 0, m = 0;
        kz_font_stats(g_fonth[1], &l, &h, &m);
        const char *pre = " cache16 l=";
        for (int i = 0; pre[i] && k < 120; i++) buf[k++] = pre[i];
        // tiga angka kecil (cukup untuk demo)
        uint32_t vs[3] = { l, h, m };
        for (int v = 0; v < 3; v++) {
            char tb[8];
            int tbl = 0;
            if (vs[v] == 0) tb[tbl++] = '0';
            else {
                char tr[8];
                int tl = 0;
                uint32_t t = vs[v] > 9999 ? 9999 : vs[v];
                while (t > 0 && tl < 7) {
                    tr[tl++] = (char)('0' + t % 10);
                    t /= 10;
                }
                while (tl > 0) tb[tbl++] = tr[--tl];
            }
            for (int i = 0; i < tbl && k < 150; i++) buf[k++] = tb[i];
            if (v == 0) {
                if (k < 150) buf[k++] = ' ';
                if (k < 150) buf[k++] = 'h';
                if (k < 150) buf[k++] = '=';
            } else if (v == 1) {
                if (k < 150) buf[k++] = ' ';
                if (k < 150) buf[k++] = 'm';
                if (k < 150) buf[k++] = '=';
            }
        }
    } else {
        const char *fail = " GAGAL DIMUAT (fallback bitmap)";
        for (int i = 0; fail[i] && k < 150; i++) buf[k++] = fail[i];
    }
    buf[k] = '\0';
    ui_label_set_text(g_status_label, buf);
}

static void switch_font(int dir) {
    g_idx += dir;
    if (g_idx < 0) g_idx = N_FONTS - 1;
    if (g_idx >= N_FONTS) g_idx = 0;
    g_traced = 0;
    load_fonts();
    ui_label_set_text(g_font_label, g_fonts[g_idx].name);
    refresh_status();
    refresh_all();
    trace("font", g_fonts[g_idx].name, g_idx);
    if (g_ok && g_fonth[1]) {
        // Satu lookup dulu (MISS); draw berikutnya: HIT 'A' -> transisi
        // MISS->HIT cache terbukti di serial via trace draw pertama.
        uint32_t w = 0, h = 0;
        kz_text_measure(g_fonth[1], "A", &w, &h);
    }
}

static void on_prev(void *u) {
    (void)u;
    switch_font(-1);
}

static void on_next(void *u) {
    (void)u;
    switch_font(+1);
}

// Panah Left/Right = ganti font (scancode E0: Left 0x14B, Right 0x14D).
static void on_key(void *u, uint32_t ascii, uint32_t scancode,
                   uint32_t mods) {
    (void)u;
    (void)ascii;
    (void)mods;
    if (scancode == 0x14B) switch_font(-1);
    else if (scancode == 0x14D) switch_font(+1);
}

void main(void) {
    ui_window_t *win = ui_window_create(660, 500);
    if (!win) sys_exit();
    g_win = win;
    ui_window_set_title(win, "Font Demo - FreeType");
    ui_window_set_key(win, on_key, 0);

    ui_widget_t *box = ui_vbox_create(win, 6);
    ui_widget_t *t = ui_label_create(win, "Font Demo - FreeType 2.14.3");
    ui_layout_add(box, t);

    ui_widget_t *frow = ui_hbox_create(win, 6);
    ui_widget_t *bp = ui_button_create(win, "<");
    ui_button_set_click(bp, on_prev, 0);
    ui_layout_add(frow, bp);
    g_font_label = ui_label_create(win, g_fonts[0].name);
    ui_layout_add(frow, g_font_label);
    ui_widget_t *bn = ui_button_create(win, ">");
    ui_button_set_click(bn, on_next, 0);
    ui_layout_add(frow, bn);
    ui_window_add(win, frow);

    static const char *sample = "AaBbCcDdEeFfGg 0123456789";
    for (int i = 0; i < N_SIZES; i++) {
        int hh = (int)g_sizes[i] + 16;
        ui_widget_t *w = ui_fttext_create(win, 624, hh);
        g_line_info[i].text = sample;
        g_line_info[i].slot = i;
        ui_fttext_set_draw(w, draw_line, &g_line_info[i]);
        ui_layout_add(box, w);
        g_lines[i] = w;
    }
    g_utf8_line = ui_fttext_create(win, 624, 40);
    ui_fttext_set_draw(g_utf8_line, draw_line, &g_utf8_info);
    ui_layout_add(box, g_utf8_line);

    g_palette_line = ui_fttext_create(win, 624, 48);
    ui_fttext_set_draw(g_palette_line, draw_line, &g_palette_info);
    ui_layout_add(box, g_palette_line);

    g_status_label = ui_label_create(win, "...");
    ui_layout_add(box, g_status_label);

    ui_window_add(win, box);

    int n = slen(g_fonts[g_idx].name);
    trace("launch fonts", 0, N_FONTS);
    trace("name-len", 0, n);
    load_fonts();
    refresh_status();
    if (g_ok && g_fonth[1]) {
        uint32_t w = 0, h = 0;
        kz_text_measure(g_fonth[1], "A", &w, &h);
        trace("measure A16 w", 0, (int)w);
        trace("measure A16 h", 0, (int)h);
    }
    ui_window_run(win);
    destroy_fonts();
    ui_window_destroy(win);
    sys_exit();
}
