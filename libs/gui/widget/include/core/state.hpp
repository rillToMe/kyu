// libs/widget/include/core/state.hpp — model state widget (satu definisi).
//
// MASALAH YANG DIPECAHKAN
// Setiap widget interaktif butuh kombinasi state yang sama (normal/hover/
// pressed/focused/disabled/selected/checked), dan sebelum ini tiap widget
// menuliskan rantai if-else-nya sendiri. Akibatnya: tombol hover memakai
// campuran X, baris daftar memakai campuran Y, dan disabled kadang menang
// kadang kalah atas hover — persis jenis inkonsistensi yang membuat UI terasa
// "diperbaiki satu-satu".
//
// Solusi: SATU tempat yang (a) mendefinisikan state mana yang relevan, dan
// (b) memetakan state → warna permukaan/teks/border. Widget cukup mengisi
// `StateInputs` lalu menggambar; tidak ada lagi rantai if warna di draw().
//
// Aturan prioritas (dipakai semua widget, tanpa pengecualian):
//   disabled  >  pressed  >  hover
//   focus     hanya mempengaruhi BORDER, tidak pernah warna isi
//   selected  mempengaruhi isi (baris daftar), bukan border
#ifndef KWIDGET_CORE_STATE_HPP
#define KWIDGET_CORE_STATE_HPP

#include "runtime/platform.hpp"
#include "core/theme.hpp"

namespace ui {

class Painter;   // fwd: draw_focus_ring/underline — definisi di core/painter.hpp

// State interaktif. Bitmask supaya satu widget bisa berada di beberapa state
// sekaligus (mis. hover + focused) dan pemetaannya tetap satu jalur.
enum WidgetState {
    ST_NORMAL   = 0,
    ST_HOVER    = 1 << 0,
    ST_PRESSED  = 1 << 1,
    ST_FOCUSED  = 1 << 2,
    ST_DISABLED = 1 << 3,
    ST_SELECTED = 1 << 4,
    ST_CHECKED  = 1 << 5,
    ST_INVALID  = 1 << 6
};

// ------------------------------------------------------------
// Input satu widget untuk pemetaan warna. Diisi widget di draw().
// ------------------------------------------------------------
struct StateInputs {
    bool hover;
    bool pressed;
    bool focused;
    bool enabled;
    bool selected;
    bool invalid;
    StateInputs()
        : hover(false), pressed(false), focused(false), enabled(true),
          selected(false), invalid(false) {}
};

// ------------------------------------------------------------
// Hasil pemetaan: warna + apakah border perlu digambar.
// `border_w` = 0 berarti tidak ada border (widget memutuskan sendiri apakah
// border itu memang bagian bahasanya, mis. tombol primer memakai border
// senada isi supaya tepi tetap tegas).
// ------------------------------------------------------------
struct SurfaceStyle {
    color_t fill;
    color_t border;
    color_t text;
    int     border_w;
};

// Permukaan untuk state — SATU rumus.
//   normal  = surface_elevated (kontrol terangkat dari latar halaman)
//   hover   = surface_hover
//   pressed = surface_pressed
//   disabled= surface (datar; redup dicapai lewat warna TEKS, bukan isi,
//             supaya bentuk kontrol tetap terbaca)
static inline color_t state_surface(const Theme& t, const StateInputs& s) {
    if (!s.enabled) return t.surface;
    if (s.pressed)  return t.surface_pressed;
    if (s.hover)    return t.surface_hover;
    return t.surface_elevated;
}

// Border untuk state. Focus SELALU menang atas hover (fokus harus terlihat
// bahkan saat pointer kebetulan berada di atas widget), tapi KALAH dari
// disabled (widget nonaktif tidak bisa difokuskan).
static inline color_t state_border(const Theme& t, const StateInputs& s) {
    if (!s.enabled) return t.border_subtle;
    if (s.invalid)  return t.danger;
    if (s.focused)  return t.focus;
    if (s.hover)    return t.border;
    return t.border;
}

// Border untuk kontrol yang fokusnya digambar sebagai RING DI LUAR border
// (input teks, dropdown): border biasa tetap, ring ditambahkan terpisah.
static inline color_t state_border_quiet(const Theme& t, const StateInputs& s) {
    if (!s.enabled) return t.border_subtle;
    if (s.invalid)  return t.danger;
    if (s.hover)    return t.border;
    return t.border_subtle;
}

// Teks untuk state (disabled selalu menang).
static inline color_t state_text(const Theme& t, const StateInputs& s) {
    return s.enabled ? t.text : t.text_disabled;
}

// Style lengkap untuk kontrol bergaya "tombol/permukaan" (Button, ComboBox).
static inline SurfaceStyle state_style(const Theme& t, const StateInputs& s) {
    SurfaceStyle o;
    o.fill = state_surface(t, s);
    o.border = state_border(t, s);
    o.text = state_text(t, s);
    o.border_w = 1;
    return o;
}

// ------------------------------------------------------------
// Ring fokus & underline fokus digambar oleh Painter (butuh tipe lengkap):
// lihat Painter::focus_ring() / Painter::focus_underline() di core/painter.hpp.
// Keduanya digambar DI DALAM bounds widget supaya damage tracking tetap tepat.
// ------------------------------------------------------------

} // namespace ui

#endif // KWIDGET_CORE_STATE_HPP
