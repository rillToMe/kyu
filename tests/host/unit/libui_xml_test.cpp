// ============================================================
// libui_xml_test.cpp — uji host XML deklaratif -> libui (Phase E).
//
//   make test-libui-xml
//
// Lapisan terpisah: Parser (§44) / Skema (§45) / Inflater (§46) /
// Ownership+failure (§47) / Ekuivalensi (§48) / Matriks tema (§49).
// Stub syscall+libgui disalin dari libui_theme_test.cpp (pola repo).
// Alokasi dihitung (g_allocs/g_frees) untuk membuktikan tak ada leak
// pada jalur gagal. Angka PASS dilaporkan apa adanya di laporan Phase E.
// ============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "userlib.h"
#include "libgui.h"
}

static int g_allocs = 0, g_frees = 0;

extern "C" {
void* sys_alloc(uint32_t n) { g_allocs++; void* p = malloc(n ? n : 1); return p; }
void  sys_free(void* p)     { if (p) { g_frees++; free(p); } }
static int g_advt = 0;   // 1 = uptime maju (uji tooltip) di blok W
static uint64_t g_now = 1000;
uint64_t sys_uptime(void)   { if (g_advt) g_now += 100; return g_now; }
void  sys_yield(void)       {}
// Antrean event scripted untuk blok W (dispatch Window-level penuh).
static kyuzen_event_t g_evq[24];
static int g_nevq = 0, g_posq = 0;
int   sys_get_event(kyuzen_event_t* e) {
    if (g_posq < g_nevq) { *e = g_evq[g_posq++]; return 1; }
    if (e) { e->type = 0; e->param1 = 0; e->param2 = 0; e->param3 = 0; e->win_id = 0; }
    return 0;
}
static void qev(uint32_t t, int32_t p1, int32_t p2, int32_t p3) {
    if (g_nevq < 24) {
        g_evq[g_nevq].type = t; g_evq[g_nevq].param1 = p1;
        g_evq[g_nevq].param2 = p2; g_evq[g_nevq].param3 = p3;
        g_evq[g_nevq].win_id = 0; g_nevq++;
    }
}
int   sys_kwm_set_cursor(int k) { (void)k; return 0; }
// FS palsu (settings_load; tak dipakai test ini tapi di-link).
static unsigned char g_file[64];
static int g_file_len = 0, g_file_pos = 0, g_fd_open = 0;
int   sys_open(const char* p, uint32_t f) {
    (void)p; (void)f;
    g_file_pos = 0; g_fd_open = 1;
    return 1;
}
int   sys_read_fd(int fd, void* b, uint32_t n) {
    (void)fd;
    if (!g_fd_open) return -1;
    int avail = g_file_len - g_file_pos;
    int take = (int)n < avail ? (int)n : avail;
    for (int i = 0; i < take; i++) ((unsigned char*)b)[i] = g_file[g_file_pos + i];
    g_file_pos += take;
    return take;
}
int   sys_write_fd(int fd, const void* b, uint32_t n) {
    (void)fd;
    if (!g_fd_open || g_file_pos + (int)n > (int)sizeof(g_file)) return -1;
    for (uint32_t i = 0; i < n; i++) g_file[g_file_pos + i] = ((const unsigned char*)b)[i];
    g_file_pos += (int)n;
    g_file_len = g_file_pos;
    return (int)n;
}
int   sys_close(int fd) { (void)fd; g_fd_open = 0; return 0; }

#define GW_W 240
#define GW_H 120
static uint32_t g_canvas[GW_W * GW_H];
static gui_window_t g_win;

gui_window_t* gui_create_window(uint32_t w, uint32_t h) {
    memset(&g_win, 0, sizeof(g_win));
    memset(g_canvas, 0, sizeof(g_canvas));
    g_win.win_id = 1;
    g_win.width = w > GW_W ? GW_W : w;
    g_win.height = h > GW_H ? GW_H : h;
    g_win.inner_w = g_win.width;
    g_win.inner_h = g_win.height;
    g_win.canvas = g_canvas;
    g_win.is_running = 1;
    return &g_win;
}
void gui_destroy(gui_window_t* w) { (void)w; }
void gui_flush(gui_window_t* w) { (void)w; }
void gui_damage_rect(gui_window_t* w, int x, int y, int cw, int ch) {
    (void)w; (void)x; (void)y; (void)cw; (void)ch;
}
void gui_draw_rect(gui_window_t* w, int x, int y, int cw, int ch, color_t c) {
    uint32_t solid = color_to_u32(color_opaque(c), FORMAT_ARGB);
    for (int iy = y; iy < y + ch; iy++) {
        if (iy < 0 || iy >= (int)w->height) continue;
        for (int ix = x; ix < x + cw; ix++) {
            if (ix < 0 || ix >= (int)w->width) continue;
            w->canvas[iy * (int)w->width + ix] = solid;
        }
    }
}
void gui_draw_text(gui_window_t* w, const char* t, int x, int y, color_t c) {
    (void)w; (void)t; (void)x; (void)y; (void)c;
}
void gui_draw_char(gui_window_t* w, char ch, int x, int y, color_t c) {
    (void)w; (void)ch; (void)x; (void)y; (void)c;
}
int gui_set_window_title(gui_window_t* w, const char* t) { (void)w; (void)t; return 0; }
uint32_t* png_decode(const char* f, int* w, int* h) {
    (void)f;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
void png_free(uint32_t* b) { (void)b; }
}   // extern "C"

#include "libui.h"
#include "libui_xml.h"
// Internal untuk verifikasi struktur (test-only, bukan ABI).
#include "core/theme.hpp"
#include "core/widget.hpp"
#include "layout/layout.hpp"
#include "layout/vbox.hpp"
#include "primitives/button.hpp"
#include "primitives/label.hpp"
#include "primitives/textbox.hpp"
#include "primitives/checkbox.hpp"
#include "primitives/radio.hpp"
#include "primitives/combobox.hpp"
#include "primitives/slider.hpp"
#include "primitives/separator.hpp"
#include "primitives/progressbar.hpp"
#include "containers/listview.hpp"
#include "containers/grid.hpp"
#include "containers/tab.hpp"
#include "containers/scrollview.hpp"
#include "window/window.hpp"

static int PASS = 0, FAIL = 0;
static void check(int cond, const char* what) {
    if (cond) { PASS++; printf("PASS %s\n", what); }
    else      { FAIL++; printf("FAIL %s\n", what); }
}

static unsigned xlen(const char* s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

// Parse helper: return doc (0 = gagal), isi err.
static ui_xml_doc_t* pd(const char* s, ui_xml_error_t* e) {
    return ui_xml_parse(s, xlen(s), e);
}

static int balanced(void) { return g_allocs == g_frees; }

void xmltest_on_click(void* u) { (*(int*)u)++; }
void xmltest_on_change(void* u) { (*(int*)u)++; }

int main(void) {
    ui_xml_error_t e;

    // ================= P: PARSER =================
    {
        ui_xml_doc_t* d = pd("<window><label text=\"Hi\"/></window>", &e);
        check(d != 0 && e.code == UI_XML_OK, "P01: dasar lolos");
        ui_xml_doc_destroy(d);
        check(balanced(), "P02: parse+destroy seimbang");
    }
    {
        ui_xml_doc_t* d = pd("<window><vbox spacing=\"8\"><label text=\"a\"/><button text=\"b\"/></vbox></window>", &e);
        check(d != 0, "P03: nested lolos");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("<window><label text='sq'/></window>", &e);
        check(d != 0, "P04: quote tunggal lolos");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("  <window>\n\t<label text=\"a\" />\n</window>  ", &e);
        check(d != 0, "P05: whitespace lolos");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("<!-- c --><window><!-- d --><label text=\"a\"/><!-- e --></window><!-- f -->", &e);
        check(d != 0, "P06: komentar lolos");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("<?xml version=\"1.0\"?><window/>", &e);
        check(d != 0, "P07: deklarasi lolos");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a\"></window>", &e);
        check(d == 0 && e.code == UI_XML_MISMATCH, "P08: mismatch ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a\"></label2>", &e);
        check(d == 0 && e.code == UI_XML_MISMATCH, "P09: tutup salah nama ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a\"/></window", &e);
        check(d == 0 && (e.code == UI_XML_UNCLOSED || e.code == UI_XML_SYNTAX), "P10: input terpotong ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a></window>", &e);
        check(d == 0, "P11: quote hilang ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a\" text=\"b\"/></window>", &e);
        check(d == 0 && e.code == UI_XML_SYNTAX, "P12: atribut duplikat ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a &amp; b &lt;c&gt; &quot;q&quot; &apos;z&apos;\"/></window>", &e);
        check(d != 0, "P13: 5 entity lolos");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a &foo;\"/></window>", &e);
        check(d == 0 && e.code == UI_XML_BAD_ENTITY, "P14: entity asing ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a & b\"/></window>", &e);
        check(d == 0 && e.code == UI_XML_BAD_ENTITY, "P15: & mentah ditolak");
    }
    {
        // depth 17 > 16
        char deep[1024];
        int k = 0;
        deep[k++] = '<'; deep[k++] = 'w'; deep[k++] = 'i'; deep[k++] = 'n';
        deep[k++] = 'd'; deep[k++] = 'o'; deep[k++] = 'w'; deep[k++] = '>';
        for (int i = 0; i < 17; i++) {
            deep[k++] = '<'; deep[k++] = 'v'; deep[k++] = 'b'; deep[k++] = 'o';
            deep[k++] = 'x'; deep[k++] = '>';
        }
        deep[k] = '\0';
        ui_xml_doc_t* d = pd(deep, &e);
        check(d == 0 && e.code == UI_XML_TOO_DEEP, "P16: depth berlebih ditolak");
    }
    {
        ui_xml_doc_t* d = pd("", &e);
        check(d == 0 && e.code == UI_XML_EMPTY, "P17: dokumen kosong ditolak");
    }
    {
        ui_xml_doc_t* d = pd("   <!-- saja -->  ", &e);
        check(d == 0 && e.code == UI_XML_EMPTY, "P18: tanpa akar ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window/><window/>", &e);
        check(d == 0 && e.code == UI_XML_MULTI_ROOT, "P19: dua akar ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window>teks</window>", &e);
        check(d == 0 && e.code == UI_XML_BAD_TEXT, "P20: teks non-ws ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><![CDATA[x]]></window>", &e);
        check(d == 0 && e.code == UI_XML_UNSUPPORTED, "P21: CDATA ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<!DOCTYPE x><window/>", &e);
        check(d == 0 && e.code == UI_XML_UNSUPPORTED, "P22: DOCTYPE ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><?php x?></window>", &e);
        check(d == 0 && e.code == UI_XML_UNSUPPORTED, "P23: PI non-deklarasi ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><!-- tak tutup</window>", &e);
        check(d == 0, "P24: komentar tak tutup ditolak");
    }
    {
        // atribut >16
        char many[1024];
        int k = 0;
        const char* h = "<window><label ";
        while (*h) many[k++] = *h++;
        for (int i = 0; i < 17; i++) {
            many[k++] = 'a' + (char)(i % 26);
            many[k++] = '=';
            many[k++] = '"';
            many[k++] = 'x';
            many[k++] = '"';
            many[k++] = ' ';
        }
        many[k++] = '/'; many[k++] = '>'; many[k++] = '<'; many[k++] = '/';
        many[k++] = 'w'; many[k++] = 'i'; many[k++] = 'n'; many[k++] = 'd';
        many[k++] = 'o'; many[k++] = 'w'; many[k++] = '>'; many[k] = '\0';
        ui_xml_doc_t* d = pd(many, &e);
        check(d == 0 && e.code == UI_XML_TOO_MANY_ATTRS, "P25: atribut berlebih ditolak");
    }
    {
        // nilai atribut >256
        static char big[320];
        int k = 0;
        const char* h = "<window><label text=\"";
        while (*h) big[k++] = *h++;
        for (int i = 0; i < 260; i++) big[k++] = 'y';
        big[k++] = '"'; big[k++] = '/'; big[k++] = '>';
        big[k++] = '<'; big[k++] = '/'; big[k++] = 'w'; big[k++] = 'i';
        big[k++] = 'n'; big[k++] = 'd'; big[k++] = 'o'; big[k++] = 'w';
        big[k++] = '>'; big[k] = '\0';
        ui_xml_doc_t* d = pd(big, &e);
        check(d == 0 && e.code == UI_XML_ATTR_TOO_LONG, "P26: nilai overlong ditolak");
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a\" / ></window>", &e);
        check(d != 0, "P27: spasi sebelum /> lolos");
        ui_xml_doc_destroy(d);
    }
    {
        // parser buta skema: elemen asing lolos parse, ditolak inflater
        ui_xml_doc_t* d = pd("<window>\n  <buton/>\n</window>", &e);
        check(d != 0, "P28a: parse lolos (elemen bebas di parser)");
        ui_xml_doc_destroy(d);
    }
    {
        ui_xml_doc_t* d = pd("</window>", &e);
        check(d == 0, "P29: tutup liar ditolak");
    }
    check(balanced(), "P30: semua jalur parser seimbang");

    // ================= S: SKEMA =================
    {
        ui_xml_doc_t* d = pd("<vbox/>", &e);
        check(d != 0, "S00: parser terima akar non-window");
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_UNKNOWN_ELEMENT, "S01: akar non-window ditolak");
        check(((ui::Window*)win)->root == 0, "S02: window bersih setelah tolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "S03: tolak akar seimbang");
    }
    {
        ui_xml_doc_t* d = pd("<window><buton text=\"OK\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_UNKNOWN_ELEMENT, "S04: elemen asing ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><button texxt=\"OK\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_UNKNOWN_ATTRIBUTE, "S05: atribut typo ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><button text=\"OK\" enabled=\"maybe\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_INVALID_VALUE, "S06: bool invalid ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><textbox width=\"abc\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_INVALID_VALUE, "S07: int invalid ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><button text=\"OK\" variant=\"mega\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_INVALID_VALUE, "S08: enum invalid ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><textbox/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_MISSING_ATTRIBUTE, "S09: width wajib ditolak bila hilang");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><grid rows=\"2\" cols=\"2\"><label text=\"a\"/></grid></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_BAD_PLACEMENT, "S10: grid tanpa row/col ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><grid rows=\"1\" cols=\"1\"><label row=\"0\" col=\"0\" text=\"a\"/><label row=\"0\" col=\"0\" text=\"b\"/></grid></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_BAD_PLACEMENT, "S11: grid overlap ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><label text=\"a\"><button text=\"b\"/></label></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_BAD_CHILD, "S12: anak daun ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><combobox width=\"120\"><label text=\"x\"/></combobox></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_BAD_CHILD, "S13: combo non-item ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><combobox width=\"120\"><item/></combobox></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_MISSING_ATTRIBUTE, "S14: item tanpa text ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><button id=\"a\" text=\"1\"/><button id=\"a\" text=\"2\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_DUP_ID, "S15: id ganda ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window theme-accent=\"purple\"/>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_MISSING_ATTRIBUTE, "S16: accent tanpa mode ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window theme-mode=\"dark\" theme-accent=\"custom\"/>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_MISSING_ATTRIBUTE, "S17: custom tanpa warna ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window theme-mode=\"dark\" theme-custom=\"red\"/>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_INVALID_VALUE, "S18: warna non-hex ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><grid rows=\"2\" cols=\"2\" padding=\"8\" padding-left=\"4\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_INVALID_VALUE, "S19: padding campur ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    {
        ui_xml_doc_t* d = pd("<window><tab width=\"100\" height=\"60\"><label text=\"x\"/></tab></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && e.code == UI_XML_BAD_CHILD, "S20: tab non-page ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }
    check(balanced(), "S21: semua jalur skema seimbang");

    // ================= I: INFLATER =================
    static const char* DOC_SETTINGS =
        "<window theme-mode=\"dark\" theme-accent=\"purple\">"
        "<vbox spacing=\"md\">"
        "<label id=\"title\" text=\"Settings\"/>"
        "<grid id=\"form\" rows=\"2\" cols=\"2\" gap=\"8\" padding=\"12\" align=\"center\">"
        "<label row=\"0\" col=\"0\" text=\"Theme\"/>"
        "<combobox id=\"theme\" row=\"0\" col=\"1\" width=\"140\" selected=\"1\">"
        "<item text=\"Dark\"/><item text=\"Light\"/>"
        "</combobox>"
        "<label row=\"1\" col=\"0\" text=\"Mode\"/>"
        "<hbox row=\"1\" col=\"1\" spacing=\"sm\">"
        "<radio id=\"r1\" text=\"Basic\" group=\"mode\" selected=\"true\"/>"
        "<radio id=\"r2\" text=\"Advanced\" group=\"mode\"/>"
        "</hbox>"
        "</grid>"
        "<separator/>"
        "<checkbox id=\"notif\" text=\"Enabled\" checked=\"true\"/>"
        "<textbox id=\"name\" width=\"160\" text=\"kyuzen\"/>"
        "<slider id=\"vol\" min=\"0\" max=\"10\" value=\"7\"/>"
        "<progressbar id=\"bar\" width=\"160\" value=\"40\"/>"
        "<listview id=\"lv\" width=\"160\" height=\"60\" selected=\"0\">"
        "<item text=\"One\"/><item text=\"Two\"/>"
        "</listview>"
        "<button id=\"apply\" text=\"Apply\" variant=\"primary\"/>"
        "<button id=\"cancel\" text=\"Cancel\" enabled=\"false\" tooltip=\"Batal\"/>"
        "</vbox>"
        "</window>";
    {
        ui_xml_doc_t* d = pd(DOC_SETTINGS, &e);
        check(d != 0, "I01: parse settings lolos");
        ui_window_t* win = ui_window_create(400, 400);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "I02: inflate settings sukses");
        ui::Window* w = (ui::Window*)win;
        check(w->root != 0 && w->root->count == 1, "I03: satu root di window");
        ui::Layout* vb = (ui::Layout*)w->root->children[0];
        check(vb->count == 10, "I04: vbox 10 anak");
        // title
        ui::Label* t = (ui::Label*)ui_xml_find(cx, "title");
        check(t != 0, "I05: find title");
        // grid
        ui::Grid* g = (ui::Grid*)ui_xml_find(cx, "form");
        check(g != 0 && g->ncell == 4, "I06: grid 4 sel");
        check(g->pad_l == 12 && g->pad_t == 12, "I07: padding uniform 12");
        check(g->align == UI_ALIGN_CENTER, "I08: align center");
        // auto-size grid (tanpa width/height eksplisit): menampung isi,
        // tak overlap saudara (regresi crash QEMU: grid h=0 menimpa slider).
        check(g->w > 100 && g->h > 40, "I08b: grid auto-size");
        {
            vb->settle();
            ui::Slider* sl0 = (ui::Slider*)ui_xml_find(cx, "vol");
            check(sl0->y >= g->y + g->h, "I08c: slider di bawah grid (tanpa overlap)");
            ui::Widget* hit = vb->pick(sl0->x + 4, sl0->y + 4);
            check(hit == (ui::Widget*)sl0, "I08d: pick slider tepat");
        }
        // combo
        ui::ComboBox* cb = (ui::ComboBox*)ui_xml_find(cx, "theme");
        check(cb != 0 && cb->n == 2 && cb->selected == 1, "I09: combo 2 item + selected 1");
        // radio grup
        ui::Radio* r1 = (ui::Radio*)ui_xml_find(cx, "r1");
        ui::Radio* r2 = (ui::Radio*)ui_xml_find(cx, "r2");
        check(r1 != 0 && r2 != 0 && r1->is_selected() && !r2->is_selected(), "I10: radio grup eksklusif");
        check(r1->group == r2->group && r1->group != 0, "I11: segrup");
        // checkbox/textbox/slider/progress/list
        ui::CheckBox* ch = (ui::CheckBox*)ui_xml_find(cx, "notif");
        check(ch != 0 && ch->checked, "I12: checkbox checked");
        ui::TextBox* tb = (ui::TextBox*)ui_xml_find(cx, "name");
        int txok = 0;
        if (tb) { txok = tb->text[0] == 'k' && tb->text[1] == 'y'; }
        check(txok, "I13: textbox text");
        ui::Slider* sl = (ui::Slider*)ui_xml_find(cx, "vol");
        check(sl != 0 && sl->min == 0 && sl->max == 10 && sl->val == 7, "I14: slider 0..10=7");
        ui::ProgressBar* pb = (ui::ProgressBar*)ui_xml_find(cx, "bar");
        check(pb != 0 && pb->val == 40, "I15: progress 40");
        ui::ListView* lv = (ui::ListView*)ui_xml_find(cx, "lv");
        check(lv != 0 && lv->n == 2 && lv->selected == 0, "I16: list 2 item + selected 0");
        // button + disabled + tooltip
        ui::Button* ap = (ui::Button*)ui_xml_find(cx, "apply");
        check(ap != 0 && ap->variant == UI_BUTTON_PRIMARY, "I17: variant primary");
        ui::Button* cn = (ui::Button*)ui_xml_find(cx, "cancel");
        int cndis = cn != 0 && !cn->enabled;
        int cntt = 0;
        if (cn && cn->tooltip) cntt = cn->tooltip[0] == 'B';
        check(cndis && cntt, "I18: disabled + tooltip");
        // tema config diterapkan
        check(w->cfg_valid, "I19: theme config diterapkan");
        // spacing token md=12
        check(vb->count > 0, "I20: vbox hidup");
        // dokumen boleh mati setelah inflasi
        ui_xml_doc_destroy(d);
        ui::Label* t2 = (ui::Label*)ui_xml_find(cx, "title");
        check(t2 == t, "I21: widget independen dari dokumen");
        ui_xml_ctx_destroy(cx);
        ui_window_destroy(win);
        check(balanced(), "I22: sukses penuh seimbang");
    }

    // ================= O: OWNERSHIP =================
    {
        // gagal di anak ke-3: window tetap perawan
        ui_xml_doc_t* d = pd("<window><label text=\"a\"/><button text=\"b\"/><buton/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0, "O01: gagal di tengah");
        check(((ui::Window*)win)->root == 0, "O02: window perawan (tanpa root)");
        check(ui_xml_find(cx, "x") == 0, "O03: tak ada id tersisa");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "O04: gagal tengah seimbang (tanpa leak)");
    }
    {
        // gagal dalam grup radio: grup dibuang, tak ada gantung
        ui_xml_doc_t* d = pd("<window><radio text=\"a\" group=\"g\"/><buton/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 0 && ((ui::Window*)win)->root == 0, "O05: gagal grup radio bersih");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "O06: gagal grup seimbang");
    }
    {
        // inflasi ganda = LIMIT
        ui_xml_doc_t* d = pd("<window><label text=\"a\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r1 = ui_xml_inflate(cx, d, &e);
        int r2 = ui_xml_inflate(cx, d, &e);
        check(r1 == 1 && r2 == 0 && e.code == UI_XML_LIMIT, "O07: inflasi ganda ditolak");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "O08: ganda seimbang");
    }
    {
        // ctx mati SEBELUM window: grup dilepas aman (radio jadi mandiri)
        ui_xml_doc_t* d = pd("<window><radio text=\"a\" group=\"g\" selected=\"true\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "O09: inflate ok");
        ui_xml_ctx_destroy(cx);   // grup mati duluan
        ui_window_destroy(win);   // radio yatim harus aman
        ui_xml_doc_destroy(d);
        check(balanced(), "O10: urutan ctx-dulu aman");
    }
    {
        // bind: click + change + tolak pasangan ilegal
        static int clicked = 0, changed = 0;
        ui_xml_doc_t* d = pd("<window><button id=\"b\" text=\"Go\"/><checkbox id=\"c\" text=\"C\"/><label id=\"l\" text=\"L\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "O11: inflate bind-doc ok");
        check(ui_xml_bind(cx, "b", UI_XML_ON_CLICK, xmltest_on_click, &clicked) == 1, "O12: bind click button");
        check(ui_xml_bind(cx, "c", UI_XML_ON_CHANGE, xmltest_on_change, &changed) == 1, "O13: bind change checkbox");
        check(ui_xml_bind(cx, "l", UI_XML_ON_CLICK, xmltest_on_click, &clicked) == 0, "O14: bind label ditolak");
        check(ui_xml_bind(cx, "b", UI_XML_ON_CHANGE, xmltest_on_change, &changed) == 0, "O15: bind change button ditolak");
        check(ui_xml_bind(cx, "nope", UI_XML_ON_CLICK, xmltest_on_click, &clicked) == 0, "O16: bind id asing ditolak");
        ui::Button* b = (ui::Button*)ui_xml_find(cx, "b");
        ui::CheckBox* c = (ui::CheckBox*)ui_xml_find(cx, "c");
        if (b) b->on_click(0, 0);
        if (c) c->on_click(0, 0);
        check(clicked == 1 && changed == 1, "O17: callback native terpanggil");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "O18: bind seimbang");
    }

    // ================= Q: EKUIVALENSI =================
    {
        // Native
        ui_window_t* wn = ui_window_create(300, 200);
        ui_widget_t* vb = ui_vbox_create(wn, 12);
        ui_widget_t* lb = ui_label_create(wn, "Hello");
        ui_layout_add(vb, lb);
        ui_widget_t* bt = ui_button_create(wn, "Click me");
        ui_layout_add(vb, bt);
        ui_widget_t* tx = ui_textbox_create(wn, 160);
        ui_layout_add(vb, tx);
        ui_window_add(wn, vb);
        // XML
        ui_xml_doc_t* d = pd("<window><vbox spacing=\"12\"><label text=\"Hello\"/><button text=\"Click me\"/><textbox width=\"160\"/></vbox></window>", &e);
        ui_window_t* wx = ui_window_create(300, 200);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(wx);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "Q01: inflate ekuivalen ok");
        ui::Window* an = (ui::Window*)wn;
        ui::Window* ax = (ui::Window*)wx;
        ui::Layout* ln = (ui::Layout*)an->root->children[0];
        ui::Layout* lx = (ui::Layout*)ax->root->children[0];
        int same = ln->count == 3 && lx->count == 3;
        if (same) {
            for (int i = 0; i < 3 && same; i++) {
                ui::Widget* a = ln->children[i];
                ui::Widget* b = lx->children[i];
                // tipe sama = ukuran + perilaku native sama (tanpa RTTI:
                // bandingkan w/h + focusable + teks atribut utama).
                if (a->w != b->w || a->h != b->h) same = 0;
                if (a->focusable() != b->focusable()) same = 0;
            }
            ui::Label* la = (ui::Label*)ln->children[0];
            ui::Label* lb2 = (ui::Label*)lx->children[0];
            int eq = 1;
            for (int i = 0; la->text[i] || lb2->text[i]; i++)
                if (la->text[i] != lb2->text[i]) { eq = 0; break; }
            if (!eq) same = 0;
        }
        check(same, "Q02: hierarki+geometri+teks identik");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(wx);
        ui_window_destroy(wn);
        check(balanced(), "Q03: ekuivalen seimbang");
    }

    // ================= T: MATRIKS TEMA =================
    {
        static const char* modes[] = { "dark", "light", 0 };
        static const char* accs[] = { "neutral", "purple", "green", 0 };
        static const char* TDOC = "<window theme-mode=\"%s\" theme-accent=\"%s\">"
                                  "<button id=\"b\" text=\"X\" variant=\"primary\"/>"
                                  "<radio id=\"r\" text=\"R\" group=\"g\" selected=\"true\"/>"
                                  "<combobox id=\"c\" width=\"120\">"
                                  "<item text=\"A\"/><item text=\"B\"/></combobox>"
                                  "<separator/>"
                                  "<textbox id=\"t\" width=\"120\" error=\"true\"/>"
                                  "</window>";
        int n = 0;
        for (int m = 0; modes[m]; m++) {
            for (int a = 0; accs[a]; a++) {
                char doc[512];
                int k = 0;
                const char* p = TDOC;
                // format manual (tanpa snprintf): %s x2
                int slot = 0;
                const char* vals[2];
                vals[0] = modes[m]; vals[1] = accs[a];
                while (*p && k < 500) {
                    if (p[0] == '%' && p[1] == 's') {
                        const char* v = vals[slot++];
                        while (*v && k < 500) doc[k++] = *v++;
                        p += 2;
                    } else doc[k++] = *p++;
                }
                doc[k] = '\0';
                ui_xml_doc_t* d = pd(doc, &e);
                ui_window_t* win = ui_window_create(200, 100);
                ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
                int r = (d != 0) ? ui_xml_inflate(cx, d, &e) : 0;
                if (r == 1 && ((ui::Window*)win)->cfg_valid) n++;
                if (d) ui_xml_doc_destroy(d);
                ui_xml_ctx_destroy(cx);
                ui_window_destroy(win);
            }
        }
        check(n == 6, "T01: matriks 2x3 tema lolos + cfg valid");
        check(balanced(), "T02: matriks seimbang");
    }
    {
        // theme-custom 0xRRGGBB
        ui_xml_doc_t* d = pd("<window theme-mode=\"dark\" theme-accent=\"custom\" theme-custom=\"0x1E1E1E\"><label text=\"x\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1 && ((ui::Window*)win)->cfg_valid, "T03: custom hex lolos");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
    }

    // ================= R: RENDER + INTERACT =================
    // Jalur yang TIDAK diuji blok I: arrange/draw/pick/klik/popup (
    // crash QEMU terjadi di sini — inflate+commit lolos, input mati).
    {
        ui_xml_doc_t* d = pd(DOC_SETTINGS, &e);
        ui_window_t* win = ui_window_create(460, 380);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "R01: inflate render-doc ok");
        ui::Window* w = (ui::Window*)win;
        w->root->settle();
        w->render();
        check(1, "R02: settle+render hidup");
        // klik Apply (tengah tombol)
        ui::Button* ap = (ui::Button*)ui_xml_find(cx, "apply");
        ui::Widget* hit = ((ui::Layout*)w->root->children[0])->pick(ap->x + 4, ap->y + 4);
        check(hit != 0, "R03: pick tombol");
        if (hit) hit->on_click(ap->x + 4, ap->y + 4);
        // klik radio Advanced
        ui::Radio* r2 = (ui::Radio*)ui_xml_find(cx, "r2");
        hit = ((ui::Layout*)w->root->children[0])->pick(r2->x + 3, r2->y + 3);
        if (hit) hit->on_click(r2->x + 3, r2->y + 3);
        ui::Radio* r1 = (ui::Radio*)ui_xml_find(cx, "r1");
        check(r2->is_selected() && !r1->is_selected(), "R04: klik radio native");
        // combo popup buka + pilih + tutup
        ui::ComboBox* cb2 = (ui::ComboBox*)ui_xml_find(cx, "theme");
        cb2->on_click(cb2->x + 4, cb2->y + 4);
        check(cb2->open, "R05: popup terbuka");
        w->render();
        if (cb2->menu) cb2->menu->on_click(cb2->menu->x + 4, cb2->menu->row_y(0) + 2);
        check(!cb2->open && cb2->selected == 0, "R06: pilih item menutup");
        w->render();
        // slider klik + drag
        ui::Slider* sl2 = (ui::Slider*)ui_xml_find(cx, "vol");
        sl2->on_click(sl2->x + sl2->w - 4, sl2->y + 4);
        sl2->on_drag(sl2->x + sl2->w - 4, sl2->y + 4);
        sl2->on_release();
        check(sl2->val == sl2->max, "R07: slider drag max");
        // checkbox toggle
        ui::CheckBox* ch2 = (ui::CheckBox*)ui_xml_find(cx, "notif");
        ch2->on_click(ch2->x + 2, ch2->y + 2);
        check(!ch2->checked, "R08: checkbox toggle");
        w->render();
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "R09: render+interact seimbang");
    }

    // ================= W: WINDOW RUN PENUH =================
    // Replay dispatch native (move/klik/fokus/tooltip-timer/ESC) lewat
    // antrean sys_get_event — jalur yang men-crash-kan xml_demo di QEMU.
    // Memakai dokumen xml_demo VERBATIM (bukan DOC_SETTINGS).
    {
        static const char* DEMO_DOC =
            "<window theme-mode=\"dark\" theme-accent=\"purple\">"
            "<vbox spacing=\"md\">"
            "<label text=\"XML Settings\"/>"
            "<grid rows=\"3\" cols=\"2\" gap=\"8\">"
            "<label row=\"0\" col=\"0\" text=\"Theme\"/>"
            "<combobox id=\"theme\" row=\"0\" col=\"1\" width=\"140\">"
            "<item text=\"Dark\"/><item text=\"Light\"/>"
            "</combobox>"
            "<label row=\"1\" col=\"0\" text=\"Notify\"/>"
            "<checkbox id=\"notif\" row=\"1\" col=\"1\" text=\"Enabled\"/>"
            "<label row=\"2\" col=\"0\" text=\"Mode\"/>"
            "<hbox row=\"2\" col=\"1\" spacing=\"sm\">"
            "<radio id=\"m1\" text=\"Basic\" group=\"mode\" selected=\"true\"/>"
            "<radio id=\"m2\" text=\"Advanced\" group=\"mode\"/>"
            "</hbox>"
            "</grid>"
            "<separator/>"
            "<textbox id=\"name\" width=\"200\" text=\"kyuzen\" tooltip=\"User name\"/>"
            "<slider id=\"vol\" min=\"0\" max=\"10\" value=\"7\"/>"
            "<hbox spacing=\"sm\">"
            "<button id=\"apply\" text=\"Apply\" variant=\"primary\"/>"
            "<button id=\"cancel\" text=\"Cancel\"/>"
            "</hbox>"
            "<label id=\"status\" text=\"Ready\"/>"
            "</vbox>"
            "</window>";
        ui_xml_doc_t* d = pd(DEMO_DOC, &e);
        ui_window_t* win = ui_window_create(460, 380);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "W01: inflate demo-doc ok");
        ui::Window* w = (ui::Window*)win;
        w->root->settle();
        w->render();   // arrange cascade penuh (draw->arrange rekursif) —
                       // koordinat final baru valid setelah ini
        ui::Radio* m2 = (ui::Radio*)ui_xml_find(cx, "m2");
        ui::TextBox* nm = (ui::TextBox*)ui_xml_find(cx, "name");
        // move ke radio m2 -> klik -> move ke textbox (tooltip) -> ESC keluar
        g_nevq = 0; g_posq = 0;
        g_advt = 1;
        qev(EVENT_MOUSE_MOVE, m2->x + 3, m2->y + 3, 0);
        qev(EVENT_MOUSE_CLICK, 0, 1, 0);
        qev(EVENT_MOUSE_CLICK, 0, 0, 0);
        qev(EVENT_MOUSE_MOVE, nm->x + 10, nm->y + 10, 0);
        for (int i = 0; i < 8; i++)
            qev(EVENT_MOUSE_MOVE, nm->x + 10, nm->y + 10, 0);   // diam 800ms -> tip
        qev(EVENT_KEY_PRESS, 27, 0, 0);
        ui_window_run(win);   // crash di sini = bug dispatch; lolos = target-only
        g_advt = 0;
        ui::Radio* m1 = (ui::Radio*)ui_xml_find(cx, "m1");
        check(m2->is_selected() && !m1->is_selected(), "W02: klik run-loop pilih m2");
        {
            // Geometri deterministik (== target QEMU): kunci untuk probe.
            const char* ids[] = { "theme", "notif", "m1", "m2", "name",
                                  "vol", "apply", "cancel", "status", 0 };
            ui::Grid* fg = 0;
            ui::Layout* wvb = (ui::Layout*)w->root->children[0];
            for (int i = 0; i < wvb->count; i++) {
                ui::Widget* c = wvb->children[i];
                printf("GEO vb[%d]=%p(%d,%d,%dx%d)\n", i, (void*)c,
                       c->x, c->y, c->w, c->h);
                if (i == 1) fg = (ui::Grid*)c;
            }
            if (fg) printf("GEO grid ncell=%d\n", fg->ncell);
            for (int i = 0; ids[i]; i++) {
                ui::Widget* f = (ui::Widget*)ui_xml_find(cx, ids[i]);
                if (f) printf("GEO %s=(%d,%d,%dx%d)\n", ids[i], f->x, f->y, f->w, f->h);
            }
        }
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "W03: run penuh seimbang");
    }

    // ================= F: KONSUMEN NYATA (AppearancePage) =================
    // Bentuk == apps/settings/appearance.cpp DOC (form statis + 3 binding).
    // Byte-identik dikunci probe QEMU; di sini bentuk + perilaku binding.
    {
        static const char* APPEARANCE_DOC =
            "<window><vbox spacing=\"6\" id=\"appearance_root\">"
            "<label text=\"Appearance\"/>"
            "<label text=\"Choose how KyuzenOS looks.\"/>"
            "<button id=\"theme_dark\" text=\"Dark\"/>"
            "<button id=\"theme_light\" text=\"Light\"/>"
            "<button id=\"theme_green\" text=\"Green\"/>"
            "<label id=\"appearance_status\" text=\"Theme: (from settings.ui)\"/>"
            "</vbox></window>";
        ui_xml_doc_t* d = pd(APPEARANCE_DOC, &e);
        ui_window_t* win = ui_window_create(400, 300);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate(cx, d, &e);
        check(r == 1, "F01: inflate appearance ok");
        ui::Window* w = (ui::Window*)win;
        ui::Layout* vb = (ui::Layout*)w->root->children[0];
        check(vb->count == 6, "F02: 6 anak (2 label + 3 tombol + status)");
        static int fapplied = 0;
        check(ui_xml_bind(cx, "theme_dark", UI_XML_ON_CLICK,
                          xmltest_on_click, &fapplied) == 1, "F03: bind tema");
        ui::Button* bd = (ui::Button*)ui_xml_find(cx, "theme_dark");
        ui::Label* st = (ui::Label*)ui_xml_find(cx, "appearance_status");
        check(bd != 0 && st != 0, "F04: find tombol + status");
        if (bd) bd->on_click(0, 0);
        check(fapplied == 1, "F05: klik native -> binding");
        // root + status untuk showPage/applyTheme
        ui::Widget* fr = (ui::Widget*)ui_xml_find(cx, "appearance_root");
        check(fr == (ui::Widget*)vb, "F06: root = vbox");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "F07: appearance seimbang");
    }
    {
        // Detached (konsumen me-parenting sendiri, mis. settings pages):
        // tanpa commit, owner null, manual add, release, destroy seimbang.
        ui_xml_doc_t* d = pd("<window><vbox id=\"r\"><label text=\"x\"/></vbox></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate_detached(cx, d, &e);
        check(r == 1, "F08: inflate detached ok");
        check(ui_xml_root_count(cx) == 1, "F09: satu root");
        ui::Widget* rt = (ui::Widget*)ui_xml_root_at(cx, 0);
        check(rt != 0 && rt->owner == 0, "F10: root detached (tanpa owner)");
        check(ui_xml_root_at(cx, 1) == 0, "F11: root_at luar batas = 0");
        // parenting manual oleh caller (pengganti commit)
        ui::Layout* host = new ui::VBox(4);
        host->add(rt);
        check(rt->owner == 0, "F12: add manual mentah (owner ikut Window::add)");
        ui_xml_release(cx);
        check(ui_xml_root_count(cx) == 0, "F13: release lupakan roots");
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(d);
        delete host;   // root ikut terhapus (milik layout)
        ui_window_destroy(win);
        check(balanced(), "F14: detached seimbang");
    }
    {
        // Detached TANPA release + destroy: roots ikut terbuang (anti-leak).
        ui_xml_doc_t* d = pd("<window><label text=\"y\"/></window>", &e);
        ui_window_t* win = ui_window_create(200, 100);
        ui_xml_ctx_t* cx = ui_xml_ctx_create(win);
        int r = ui_xml_inflate_detached(cx, d, &e);
        check(r == 1, "F15: detached ok");
        ui_xml_ctx_destroy(cx);   // tanpa release: rollback hapus roots
        ui_xml_doc_destroy(d);
        ui_window_destroy(win);
        check(balanced(), "F16: tanpa release seimbang");
    }

    printf("XML: %d PASS, %d FAIL | allocs=%d frees=%d\n", PASS, FAIL, g_allocs, g_frees);
    return FAIL ? 1 : 0;
}
