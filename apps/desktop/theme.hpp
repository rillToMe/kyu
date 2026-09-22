// Kyuzen Desktop — metrik layout + palet (KEBIJAKAN implementasi, bukan
// framework; milik apps/desktop, tidak di-stage ke SDK).
//
// Satu-satunya sumber konstanta visual desktop Phase 9: warna, jarak,
// dimensi, tinggi taskbar, ukuran ikon, ukuran teks, dimensi preview,
// konstanta wallpaper. Kode gambar TIDAK boleh menaruh angka magis sendiri.
//
// Grid 8px, font 8x16 (ASCII). Semua warna = kyuzen::desktop::Color.
//
// Pelapisan gambar (kontrak dengan compositor, TANPA ubahan framework):
//   Window desktop = full-screen + opaque. libgui (gui_create_desktop) sudah
//   mengisi canvas-nya dengan latar opaque dan mendeklarasikannya opaque, jadi
//   compositor (a) melewatkan base-blit untuk seluruh layar dan (b) menimpa
//   base_canvas dengan canvas window. AKIBATNYA gambar ke base_canvas
//   (sys_draw_image, syscall 23) TIDAK pernah terlihat.
//   Karena itu SEMUA pixel — wallpaper, ikon, strip taskbar, kartu preview,
//   notifikasi — digambar ke canvas window (Canvas::fill_rect/draw_text untuk
//   bentuk solid; Canvas::draw_px untuk pixel PNG ber-alpha).
//   Pixel wallpaper foto di-blit dengan RLE per baris (satu fill_rect
//   per rentang warna identik) karena libgui tidak punya draw-image; ikon
//   di-blend per-pixel agar transparansi PNG utuh.
#ifndef KYUZEN_DESKTOP_IMPL_THEME_HPP
#define KYUZEN_DESKTOP_IMPL_THEME_HPP

#include <kyuzen/desktop/geometry.hpp>

namespace desktop_impl {

using kyuzen::desktop::Color;
using kyuzen::desktop::rgb;

// ---- Taskbar ----
const int TB_H = 44;          // tinggi strip bawah (2 baris teks 16px + napas)
const int TB_PAD = 8;         // padding horizontal tepi strip
const int TB_SLOT_W = 40;     // lebar slot ikon app di taskbar
const int TB_SLOT_GAP = 4;    // jarak antar slot
const int TB_ICON_PX = 28;    // ikon taskbar digambar 28x28
const int TB_SYS_W = 176;     // lebar area sistem kanan (jam + tanggal)
const int TB_FOCUS_H = 3;     // tinggi garis status fokus/hover di slot

// ---- Launcher (grid ikon desktop) ----
const int CELL_W = 96;    // lebar sel ikon launcher
const int CELL_H = 104;   // tinggi sel (ikon 48 + label 16 + gap)
const int ICON_SZ = 48;   // ikon launcher digambar 48x48
const int ICON_X0 = 24;
const int ICON_Y0 = 24;
const int LBL_MAX = CELL_W / 8;  // char label per sel, sisanya dipotong

// ---- Ikon aplikasi (cache + fallback) ----
const int ICON_CACHE_PX = 48;  // satu ukuran cache; taskbar/preview
                               // mengecilkan saat gambar (tanpa alokasi)
const int ICON_CACHE_N = 12;   // entri cache (umur: timpa paling lama)
// Kekuatan penajaman (unsharp 3x3, media_sharpen_rgba) setelah downscale
// ikon: 0 = mati. Box murni ~15% lebih lembut dari acuan Lanczos pada 256->48;
// 50 mengangkat kontras tepi ke sekitar tingkat Lanczos tanpa halo (blur
// selalu di dalam rentang lokal, jadi tidak pernah overshoot).
const int ICON_SHARPEN_PCT = 50;
const int ICON_NAME_MAX = 24;  // nama file ikon di manifest ("icon=")
const char ICON_DEFAULT_PATH[] = "/default.png";  // fallback terpusat

// ---- Wallpaper ----
const int WALL_BAND_H = 8;  // tinggi strip gradasi prosedural (fallback)
const char WALL_DEFAULT[] = "island.png";  // wallpaper bawaan (akar FS)
// Daftar wallpaper bawaan (berkas di akar FS via modul limine).
const char* const WALL_BUILTINS[] = {
    "island.png",      "black-hole.png", "city-lanscaps.png",
    "city-town.png",   "kimi-no-nawa.png", "meadow.png",
};
const int WALL_BUILTIN_N = 6;

// ---- Preview hover taskbar (kartu statis) ----
const int PV_W = 220;   // lebar kartu preview
const int PV_H = 120;   // tinggi kartu preview
const int PV_GAP = 8;   // jarak kartu di atas taskbar
const int PV_PAD = 12;  // padding dalam kartu

// ---- Palet ----
const Color WALL_BG = rgb(0x14, 0x1A, 0x2E);
const Color WALL_BG2 = rgb(0x1E, 0x28, 0x48);  // ujung gradasi prosedural
const Color WALL_TXT = rgb(0x3A, 0x41, 0x60);
const Color TASK_BG = rgb(0x0B, 0x0E, 0x1C);
const Color TASK_EDGE = rgb(0x2A, 0x33, 0x55);
const Color TASK_BTN = rgb(0x1A, 0x21, 0x38);
const Color TASK_ACTIVE = rgb(0x2E, 0x4A, 0x8E);
const Color TASK_HOVER = rgb(0x22, 0x2C, 0x4C);
const Color TASK_TXT = rgb(0xE0, 0xE0, 0xE0);
const Color SYS_TXT = rgb(0xE8, 0xEC, 0xF8);  // jam (baris utama)
const Color SYS_DIM = rgb(0x90, 0x9C, 0xBC);  // tanggal (baris sekunder)
const Color ICON_TXT = rgb(0xC0, 0xC8, 0xE0);
const Color APP_DEFAULT = rgb(0x37, 0x47, 0x4F);  // tanpa manifest/ikon
const Color PV_BG = rgb(0x12, 0x16, 0x2A);
const Color PV_EDGE = rgb(0x3A, 0x4C, 0x80);
const Color PV_TITLE = rgb(0xF0, 0xF2, 0xFA);
const Color PV_DIM = rgb(0x90, 0x9C, 0xBC);

const int NOTIF_W = 520;
const int NOTIF_H = 100;
const int NOTIF_MARGIN = 16;
const int NOTIF_MS = 8000;  // 3–15 dtk (host test mengunci rentang)
const Color NOTIF_BG = rgb(0x3A, 0x12, 0x20);
const Color NOTIF_EDGE = rgb(0xE0, 0x60, 0x60);
const Color NOTIF_TITLE = rgb(0xFF, 0x80, 0x80);
const Color NOTIF_TXT = rgb(0xE8, 0xDC, 0xE0);
const Color NOTIF_HINT = rgb(0xA0, 0xB0, 0xD0);

}  // namespace desktop_impl

#endif
