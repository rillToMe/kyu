// Kyuzen Desktop — metrik layout + palet (KEBIJAKAN implementasi, bukan
// framework; milik apps/desktop, tidak di-stage ke SDK).
//
// Grid 8px, font 8px. Semua warna = kyuzen::desktop::Color (backend Canvas
// yang mengonversi ke color_t libgui).
#ifndef KYUZEN_DESKTOP_IMPL_THEME_HPP
#define KYUZEN_DESKTOP_IMPL_THEME_HPP

#include <kyuzen/desktop/geometry.hpp>

namespace desktop_impl {

using kyuzen::desktop::Color;
using kyuzen::desktop::rgb;

const int TB_H = 36;      // tinggi taskbar (baris bawah)
const int CELL_W = 92;    // lebar sel ikon launcher
const int CELL_H = 100;   // tinggi sel (ikon 56 + label 16 + gap)
const int ICON_SZ = 56;
const int ICON_X0 = 24;
const int ICON_Y0 = 24;
const int LBL_MAX = CELL_W / 8;  // char label per sel, sisanya dipotong

const Color WALL_BG = rgb(0x14, 0x1A, 0x2E);
const Color WALL_TXT = rgb(0x3A, 0x41, 0x60);
const Color TASK_BG = rgb(0x0B, 0x0E, 0x1C);
const Color TASK_EDGE = rgb(0x2A, 0x33, 0x55);
const Color TASK_BTN = rgb(0x1A, 0x21, 0x38);
const Color TASK_ACTIVE = rgb(0x2E, 0x4A, 0x8E);
const Color TASK_TXT = rgb(0xE0, 0xE0, 0xE0);
const Color ICON_TXT = rgb(0xC0, 0xC8, 0xE0);
const Color APP_DEFAULT = rgb(0x37, 0x47, 0x4F);  // tanpa manifest

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
