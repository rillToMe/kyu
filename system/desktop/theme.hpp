// Kyuzen Desktop — metrik layout + palet (KEBIJAKAN implementasi, bukan
// framework; milik system/desktop, tidak di-stage ke SDK).
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

// ---- Desktop icon box (model seleksi gaya Windows) ----
// Tiap ikon hidup dalam bounding box tetap di dalam sel grid 96x104:
// ikon 48px center-x di atas (BOX_ICON_Y), label maks 2 baris di bawah
// (BOX_LBL_DY); "..." HANYA bila baris2 masih luber.
const int BOX_W = 84;    // lebar bounding box ikon (seleksi biru ikut ini)
const int BOX_H = 100;   // tinggi bounding box ikon
const int BOX_ICON_Y = 8;                       // offset ikon dari atas box
const int BOX_LBL_DY = BOX_ICON_Y + ICON_SZ + 6;  // offset label dari atas box
const int BOX_LBL_MAX = 10;  // char label muat per baris box (bitmap 8px;
// FT 11px muat ~13 char, tapi wrap disamakan 10 agar deterministik)
// Ukuran font label FreeType (px). Dulu 16: "Terminal" (8 huruf) ~70px+
// > BOX_W-8 hingga kena shrink "...". 11px ~= 50px: muat utuh 1 baris.
const int LABEL_FONT_PX = 13;
const int BOX_LBL_LINE_H = 18;  // jarak antar baris label (16px glyph + 2)
// Dua baris muat dalam box: baris1 y+62..78, baris2 y+80..96 < BOX_H=100;
// sel 104px menyisakan gap 8px ke box bawah (tak menutupi ikon berikut).
// Hover/seleksi ikon launcher memakai tangga netral libui, bukan biru navy:
// seleksi dibedakan oleh permukaan + outline, bukan oleh warna yang berbeda
// keluarga dari seluruh UI.
const Color ICON_HOVER = rgb(0x2A, 0x2A, 0x2A);  // == libui surface_hover
const Color ICON_SEL = rgb(0x33, 0x33, 0x33);    // == surface_hover + sedikit
const Color ICON_SEL_EDGE = rgb(0xD0, 0xD0, 0xD0);  // outline seleksi
// (backend tak menjamin alpha-blend, jadi highlight = warna solid)

// ---- Context menu desktop/ikon (top-most, milik shell) ----
const int MENU_W = 180;      // lebar menu
const int MENU_ROW_H = 24;   // tinggi per baris item (== libui chrome::MENU_ROW_H)
const int MENU_PAD = 4;      // padding dalam menu
const Color MENU_BG = rgb(0x23, 0x23, 0x23);    // == libui panel/elevated
const Color MENU_EDGE = rgb(0x30, 0x30, 0x30);  // == libui border
const Color MENU_HOVER = rgb(0x2A, 0x2A, 0x2A); // == libui surface_hover
const Color MENU_TXT = rgb(0xF2, 0xF2, 0xF2);   // == libui text
const Color MENU_ACC = rgb(0x8B, 0x8B, 0x8B);   // centang "aktif" (netral)

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
// MIGRASI KE BAHASA VISUAL KYUZENOS
// Palet desktop dulu biru-navy (TASK_BG 0x0B0E1C, TASK_EDGE 0x2A3355, ...)
// sementara libui sudah memakai netral charcoal. Dua palet yang berbeda
// membuat taskbar/launcher terasa seperti aplikasi lain yang menempel di
// layar. Sekarang desktop memakai tangga netral YANG SAMA dengan tema libui
// (surface 0x181818, elevated 0x232323, border 0x303030, teks 0xF2F2F2),
// dengan aksen netral 0x8B8B8B — jadi shell dan aplikasi satu keluarga.
//
// Warna wallpaper tetap lebih dingin dari UI: wallpaper adalah LATAR, dan
// sedikit perbedaan suhu membuat jendela aplikasi terbaca sebagai "di atas"
// wallpaper tanpa perlu border tebal.
const Color WALL_BG = rgb(0x12, 0x14, 0x18);
const Color WALL_BG2 = rgb(0x1C, 0x20, 0x28);  // ujung gradasi prosedural
const Color WALL_TXT = rgb(0x3A, 0x3E, 0x46);
const Color TASK_BG = rgb(0x11, 0x11, 0x11);       // == libui bg (dark)
const Color TASK_EDGE = rgb(0x30, 0x30, 0x30);     // == libui border
const Color TASK_BTN = rgb(0x18, 0x18, 0x18);      // == libui surface
const Color TASK_ACTIVE = rgb(0x2A, 0x2A, 0x2A);   // == libui surface_hover
const Color TASK_HOVER = rgb(0x23, 0x23, 0x23);    // == libui surface_elevated
const Color TASK_TXT = rgb(0xF2, 0xF2, 0xF2);      // == libui text
const Color SYS_TXT = rgb(0xF2, 0xF2, 0xF2);       // jam (baris utama)
const Color SYS_DIM = rgb(0xA8, 0xA8, 0xA8);       // tanggal (sekunder)
const Color ICON_TXT = rgb(0xE8, 0xE8, 0xE8);
const Color APP_DEFAULT = rgb(0x37, 0x47, 0x4F);   // tanpa manifest/ikon
const Color PV_BG = rgb(0x1C, 0x1C, 0x1C);
const Color PV_EDGE = rgb(0x3A, 0x3A, 0x3A);
const Color PV_TITLE = rgb(0xF2, 0xF2, 0xF2);
const Color PV_DIM = rgb(0xA8, 0xA8, 0xA8);

const int NOTIF_W = 520;
const int NOTIF_H = 100;
const int NOTIF_MARGIN = 16;
const int NOTIF_MS = 8000;  // 3–15 dtk (host test mengunci rentang)
// Notifikasi crash tetap merah: ini satu-satunya tempat warna kuat dibenarkan
// (peringatan keselamatan), dan justru kontras itulah yang membuatnya terbaca.
const Color NOTIF_BG = rgb(0x3A, 0x12, 0x20);
const Color NOTIF_EDGE = rgb(0xE0, 0x60, 0x60);
const Color NOTIF_TITLE = rgb(0xFF, 0x80, 0x80);
const Color NOTIF_TXT = rgb(0xE8, 0xDC, 0xE0);
const Color NOTIF_HINT = rgb(0xA0, 0xB0, 0xD0);

}  // namespace desktop_impl

#endif
