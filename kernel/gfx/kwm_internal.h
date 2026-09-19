#ifndef KWM_INTERNAL_H
#define KWM_INTERNAL_H

// State internal KWM yang dibagi antara kwm.c dan compositor.c.
// JANGAN di-include dari luar kernel/gfx/ — akses publik lewat include/kwm.h.

#include <stdint.h>
#include "spinlock.h"
#include "display.h"
#include "color_types.h"   // color_t — konstanta dekorasi WM

#define MAX_WINDOWS 16

// --- Dekorasi milik WM (Phase 5C) ---
// Canvas window = KONTEN MURNI. Frame = konten + titlebar di atasnya, semua
// dihitung WM. Nama/ukuran didefinisikan sekali di sini; compositor + hit-test
// KWM membacanya, jadi tidak ada angka duplikat di jalur lain.
// Chrome ala desktop modern DARK: titlebar abu gelap, teks off-white, tombol
// close transparan (ikon abu terang) yang hanya memerah saat hover. Warna
// senada palet toolkit (menubar/status #2D2D2D), jadi window gelap terlihat
// satu kesatuan, bukan kotak hitam dengan bingkai putih.
#define KWM_TITLEBAR_H      32
#define KWM_CLOSE_BTN_W     46
static const color_t KWM_TITLEBAR_COLOR = { 0x2D, 0x2D, 0x2D, 255 };  // fokus (#2D2D2D)
static const color_t KWM_TITLEBAR_INACT = { 0x25, 0x25, 0x26, 255 };  // tidak fokus (sedikit lebih gelap)
static const color_t KWM_TITLE_FG       = { 0xD4, 0xD4, 0xD4, 255 };  // teks judul (off-white)
static const color_t KWM_CTL_FG         = { 0xA0, 0xA0, 0xA0, 255 };  // ikon kontrol (X) normal
static const color_t KWM_CLOSE_HOVER_BG = { 0xE8, 0x11, 0x23, 255 };  // latar close saat hover
static const color_t KWM_CLOSE_HOVER_FG = { 0xFF, 0xFF, 0xFF, 255 };  // ikon close saat hover

// --- Dekorasi modern (Phase 11 rendering) ---
// Titlebar digambar bergradient dengan sudut ATAS membulat, dan setiap frame
// window mendapat drop shadow yang meluber KWM_SHADOW_MARGIN px di luar frame.
// Semua dirty-rect frame HARUS diperlebar margin ini (kwm.c: frame_dirty_area)
// agar sisa shadow tidak tertinggal saat window pindah/hilang.
#define KWM_CORNER_R        8
#define KWM_SHADOW_MARGIN   6

// Garis tepi 1px tepat di luar frame (kiri/kanan/bawah) — pembatas tajam agar
// konten window tidak menyatu dengan desktop. Ring shadow bawah baru mulai
// ~3px di luar frame (offset blur ke bawah), jadi tepi bawah butuh garis ini.
// Abu terang + alpha: memisahkan window gelap dari desktop gelap.
static const color_t KWM_EDGE_COLOR = { 0x9A, 0x9A, 0x9A, 255 };
#define KWM_EDGE_ALPHA      70

typedef struct {
    uint8_t active;
    // Posisi FRAME (termasuk titlebar): (x, y) = sudut kiri-atas titlebar.
    int32_t x, y;
    // Ukuran KONTEN (== ukuran canvas). Frame = width x (height + KWM_TITLEBAR_H).
    uint32_t width, height;
    // Phase 3A/5 adopsi: surface window = DisplayBuffer (owned, dibuat
    // display_buffer_create — stride == width).
    DisplayBuffer* canvas;
    uint32_t z_index;
    int32_t owner_task;   // FIX_004: task pemilik window (-1 = tidak ada)
    uint32_t flags;       // Phase 10: KWM_WIN_DESKTOP dll.
    // Phase 14: canvas terverifikasi 100% opaque (alpha != 0 di SETIAP piksel),
    // di-set hanya oleh kwm_set_window_opaque setelah validasi kernel. Murni
    // hint performa untuk fast-path memcpy compositor — BUKAN semantik render.
    // 0 (default) = tidak dijamin → selalu jalur scalar alpha (baseline benar).
    uint8_t  fully_opaque;
    char     title[32];   // Phase 10: judul titlebar + taskbar ("" = kosong)
} kwm_window_t;

// Phase 10: window desktop — full-screen, frameless (tanpa titlebar/close),
// z=0 (selalu di belakang), klik tidak refokus, tidak ikut Alt-Tab.
#define KWM_WIN_DESKTOP 0x1

extern kwm_window_t kwm_windows[MAX_WINDOWS];
extern uint32_t next_z_index;
extern spinlock_t kwm_lock;
// Phase 5B/5C: window pemegang fokus keyboard + tint titlebar. Dibaca
// compositor.c (di bawah kwm_lock) untuk warna titlebar.
extern int focused_win_id;
// Window (+1; 0 = tidak ada) yang kursor mouse-nya di atas tombol close →
// compositor menggambar latar merah. Di-set di kwm_process_mouse (tiap event),
// dibaca compositor.c; 0 = default.
extern int hovered_close_win;

#endif
