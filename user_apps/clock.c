// ============================================================
// clock.c — Jam: jam digital + tanggal (libui + Kyuzen C SDK).
//
// Desain: variasikan HANYA kode lokal app ini (tanpa menyentuh toolkit,
// KWM, atau kernel). Hierarki + ritme dicapai dengan primitif publik:
// VBox/HBox + spacer + ui_widget_set_size + satu ukuran font bitmap.
//
// Geometri (semua dari konstanta di bawah, tanpa angka magis tersebar):
//   font bitmap 8×16; baris terlebar = tanggal "Min, 20 Sep 2026" (16 char).
//   Tiap baris HBox selebar area isi (ROW_W) dengan spacer terhitung
//   (ROW_W - len*8)/2 — center simetris, bukan spasi hardcoded per elemen.
//   Ritme vertikal: PAD_TOP + cap + GAP + time + GAP + date + PAD_BOT.
//   Fokus visual = jam (ruang napas simetris + label seksi uppercase).
//
// Build: clock.o (header SDK) + userlib.o + libgui.o +
//        libui.o + png.o + SDK crt.o + libc.a  [ENTRY _start]
// ============================================================
#include "userlib.h"
#include "libui.h"
#include <stdio.h>
#include <string.h>

// ---- Metrik font bitmap (libs/widget label: lebar = len*8, tinggi 16) ----
#define CLK_GLYPH_W   8
#define CLK_GLYPH_H   16

// ---- Ritme: padding luar + jeda antar-baris (px) ----
// Margin horizontal muncul sendiri dari ROW_W (date 128 → 16px/sisi).
#define CLK_PAD_TOP   14
#define CLK_GAP_MID   12
#define CLK_PAD_BOT   14

// ---- Lebar konten = baris tanggal (16 char, selalu terpanjang) ----
// Baris HBox SELALU selebar area isi window agar centering simetris:
// root VBox toolkit menaruh box app di x=8 (Window::add), jadi
// ROW_W = WIN_W - 2*ROOT_M. Tanpa ini, blok 128px akan rata-kiri
// dengan 40px mati di kanan (terlihat di screendump pertama).
#define CLK_DATE_LEN  16
#define CLK_ROOT_M    8    // margin root VBox toolkit (Window::add)
#define CLK_PAD_X     24
#define CLK_WIN_W     176
#define CLK_ROW_W     (CLK_WIN_W - 2 * CLK_ROOT_M)   // 160, area isi penuh
#define CLK_WIN_H     (CLK_PAD_TOP + CLK_GLYPH_H + CLK_GAP_MID + \
                       CLK_GLYPH_H + CLK_GAP_MID + CLK_GLYPH_H + CLK_PAD_BOT) // 100

static ui_widget_t *g_time;
static ui_widget_t *g_date;
static uint32_t last_sec = 0xFF;

static const char *BULAN[13] = {
    "", "Jan", "Feb", "Mar", "Apr", "Mei", "Jun",
    "Jul", "Agu", "Sep", "Okt", "Nov", "Des"
};
static const char *HARI[7] = {
    "Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"
};

// Zeller congruence → 0=Min..6=Sab.
static int day_of_week(int y, int m, int d) {
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;
}

// Satu baris konten ter-center: HBox { spacer((ROW_W - len*8)/2), label }.
// Spacer = label kosong berukuran paksa (tak menggambar apa-apa).
// Invarian: teks pengganti (tick) SELALU sepanjang teks awal.
static ui_widget_t *clock_row(ui_window_t *win, ui_widget_t *box,
                              const char *text) {
    size_t pad = (CLK_ROW_W - strlen(text) * CLK_GLYPH_W) / 2;
    ui_widget_t *row = ui_hbox_create(win, 0);
    ui_widget_t *sp = ui_label_create(win, "");
    ui_widget_set_size(sp, (int)pad, CLK_GLYPH_H);
    ui_widget_t *lb = ui_label_create(win, text);
    ui_layout_add(row, sp);
    ui_layout_add(row, lb);
    ui_layout_add(box, row);
    return lb;
}

// Spacer vertikal setinggi h (napas antar-baris).
static void clock_gap(ui_window_t *win, ui_widget_t *box, int h) {
    ui_widget_t *sp = ui_label_create(win, "");
    ui_widget_set_size(sp, 1, h);
    ui_layout_add(box, sp);
}

// Tick 1×/detik: "HH:MM:SS" + "Min, 20 Sep 2026" (panjang tetap).
static int tick(void *userdata) {
    (void)userdata;
    uint32_t t[6];   // [year, month, day, hour, min, sec]
    sys_get_time(t);
    if (t[5] == last_sec && last_sec != 0xFF) return 0;   // detik sama → tetap
    last_sec = t[5];

    char tb[12];
    snprintf(tb, sizeof(tb), "%02u:%02u:%02u", t[3], t[4], t[5]);
    ui_label_set_text(g_time, tb);

    char db[20];
    snprintf(db, sizeof(db), "%s, %02u %s %u",
             HARI[day_of_week((int)t[0], (int)t[1], (int)t[2])],
             t[2], BULAN[t[1]], t[0]);
    ui_label_set_text(g_date, db);
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    ui_window_t *win = ui_window_create(CLK_WIN_W, CLK_WIN_H);
    if (!win) { sys_exit(); }

    ui_window_set_title(win, "Jam");

    ui_widget_t *box = ui_vbox_create(win, 0);
    clock_gap(win, box, CLK_PAD_TOP);
    clock_row(win, box, "WAKTU SISTEM");   // label seksi (statis, 12 char)
    clock_gap(win, box, CLK_GAP_MID);
    g_time = clock_row(win, box, "--:--:--");   // fokus (8 char)
    clock_gap(win, box, CLK_GAP_MID);
    g_date = clock_row(win, box, "--, -- --- ----");   // sekunder (16 char)
    clock_gap(win, box, CLK_PAD_BOT);
    ui_window_add(win, box);

    ui_window_set_tick(win, tick, 0);
    tick(0);   // render nilai awal (sebelum detik pertama berubah)
    ui_window_run(win);   // blocking; keluar via X / ESC
    ui_window_destroy(win);
    sys_exit();
    return 0;   // tak tercapai (sys_exit tidak kembali)
}
