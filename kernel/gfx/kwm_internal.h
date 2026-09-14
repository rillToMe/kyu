#ifndef KWM_INTERNAL_H
#define KWM_INTERNAL_H

// State internal KWM yang dibagi antara kwm.c dan compositor.c.
// JANGAN di-include dari luar kernel/gfx/ — akses publik lewat include/kwm.h.

#include <stdint.h>
#include "spinlock.h"
#include "display.h"

#define MAX_WINDOWS 16

// --- Dekorasi milik WM (Phase 5C) ---
// Canvas window = KONTEN MURNI. Frame = konten + titlebar di atasnya, semua
// dihitung WM. Nama/ukuran didefinisikan sekali di sini; compositor + hit-test
// KWM membacanya, jadi tidak ada angka duplikat di jalur lain.
// Chrome netral ala desktop modern: titlebar terang, teks gelap, tombol close
// transparan (ikon abu) yang hanya memerah saat hover.
#define KWM_TITLEBAR_H      32
#define KWM_CLOSE_BTN_W     46
#define KWM_TITLEBAR_COLOR  0xF5F5F5   // fokus (terang)
#define KWM_TITLEBAR_INACT  0xEDEDED   // tidak fokus
#define KWM_TITLE_FG        0x202124   // teks judul
#define KWM_CTL_FG          0x3C4043   // ikon kontrol (X) normal
#define KWM_CLOSE_HOVER_BG  0xE81123   // latar close saat hover
#define KWM_CLOSE_HOVER_FG  0xFFFFFF   // ikon close saat hover

// --- Dekorasi modern (Phase 11 rendering) ---
// Titlebar digambar bergradient dengan sudut ATAS membulat, dan setiap frame
// window mendapat drop shadow yang meluber KWM_SHADOW_MARGIN px di luar frame.
// Semua dirty-rect frame HARUS diperlebar margin ini (kwm.c: frame_dirty_area)
// agar sisa shadow tidak tertinggal saat window pindah/hilang.
#define KWM_CORNER_R        8
#define KWM_SHADOW_MARGIN   6

// Garis tepi 1px tepat di luar frame (kiri/kanan/bawah) — pembatas tajam agar
// konten window terang tidak menyatu dengan desktop. Ring shadow bawah baru
// mulai ~3px di luar frame (offset blur ke bawah), jadi tepi bawah butuh garis
// ini. Warna hitam dengan alpha: tegas di latar terang, nyaris hilang di gelap.
#define KWM_EDGE_COLOR      0x000000
#define KWM_EDGE_ALPHA      96

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
