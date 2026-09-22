// Kyuzen Desktop — resolusi + cache ikon aplikasi (TERPUSAT).
//
// Urutan resolusi (satu tempat, dipakai launcher/taskbar/preview):
//   1. ikon kustom app (field "icon=" di manifest "<base>.app")
//   2. assets/icons/default.png (ICON_DEFAULT_PATH)
//   3. null → pemanggil menggambar fallback kotak warna (tanpa gambar)
//
// Piksel digambar ke CANVAS WINDOW desktop via Canvas::fill_rect (RLE per
// baris: satu rect per rentang warna identik). libgui tidak punya draw-image,
// dan layer base_canvas (sys_draw_image) TIDAK pernah terlihat: window desktop
// full-screen + opaque — lihat catatan pelapisan di theme.hpp.
//
// Cache menyimpan SATU ukuran (ICON_CACHE_PX) yang SUDAH dipertajam
// (scale_icon); pemakai yang butuh ukuran lain mengecilkan saat gambar via
// scale_icon juga (tanpa alokasi, buffer stack pemanggil).
//
// Alpha dipertahankan dari png_decode (ARGB8888): piksel transparan (a=0)
// dilewati agar wallpaper terlihat, semi-transparan di-blend ke canvas.
// Canvas hasil blend selalu opaque (0xFF) agar deklarasi opaque valid.
#ifndef KYUZEN_DESKTOP_IMPL_APP_ICONS_HPP
#define KYUZEN_DESKTOP_IMPL_APP_ICONS_HPP

#include <stdint.h>
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include "theme.hpp"

// Dekoder PNG bersama (apps/png.c, stb_image STBI_ONLY_PNG — pola yang
// dipakai widget Image + viewer): dideklarasikan di sini agar satu titik.
// Wallpaper PNG ikut lewat sini (aset dikonversi JPG->PNG offline; JPEG
// butuh float yang dilarang guard x87/SSE desktop).
extern "C" {
uint32_t* png_decode(const char* filename, int* out_w, int* out_h);
void png_free(uint32_t* buf);
}

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Color;
using kyuzen::desktop::Rect;

// Piksel XRGB8888 milik cache (jangan dibebaskan pemanggil).
struct IconPx {
    const uint32_t* px;
    int size;  // px adalah size x size (ICON_CACHE_PX)
};

// Field "icon=" manifest (boleh kosong) -> path FS absolut.
// Kosong -> ICON_DEFAULT_PATH. Non-kosong -> "/<nama>" (akar FS, tempat
// modul non-app mendarat). Selalu NUL-terminated, dipotong bila panjang.
void resolve_icon_path(const char* icon_field, char* out, int out_cap);

// Skala src sw×sh -> dst dw×dh (tanpa alokasi, tanpa float): upscale =
// nearest, downscale = rata-rata box berbobot alpha (garis tipis tidak
// hilang seperti pada nearest 256->48). Dipakai cache (decode ->
// ICON_CACHE_PX) dan pemakai yang butuh ukuran lain (taskbar 28, preview 48
// langsung). Diekspos untuk host test.
void scale_nearest(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw,
                   int dh);

// Jalur IKON: media_scale_rgba + unsharp ringan (media_sharpen_rgba,
// ICON_SHARPEN_PCT). Dipakai cache (256->48) dan taskbar (48->28) supaya ikon
// kecil tidak tampak lembek habis downscale. Scratch 3 baris di stack; tujuan
// lebih besar dari ICON_CACHE_PX atau murni upscale (nearest) hanya diskala,
// tanpa dipertajam.
void scale_icon(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw,
                int dh);

// ARGB8888 -> Color. Alpha dipertahankan (a=0 = transparan, dilewati saat
// blend; canvas hasil blend selalu opaque).
inline Color px_color(uint32_t v) {
    Color c;
    c.r = static_cast<uint8_t>((v >> 16) & 0xFFu);
    c.g = static_cast<uint8_t>((v >> 8) & 0xFFu);
    c.b = static_cast<uint8_t>(v & 0xFFu);
    c.a = static_cast<uint8_t>((v >> 24) & 0xFFu);
    return c;
}

// Blit pixel px (w×h, stride = w) ke `sink` di (x,y), RLE per baris: satu
// fill_rect per rentang warna identik (nearest-neighbor sudah dilakukan
// pemanggil). `sink` cukup punya fill_rect(Rect, Color) — Canvas di app,
// sink palsu di host test (mekanisme ini yang diuji tanpa compositor).
template <typename Sink>
void blit_px(Sink& sink, int x, int y, const uint32_t* px, int w, int h) {
    if (!px || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        const uint32_t* src = px + row * w;
        int i = 0;
        while (i < w) {
            uint32_t v = src[i];
            int n = 1;
            while (i + n < w && src[i + n] == v) n++;
            Rect r;
            r.x = x + i;
            r.y = y + row;
            r.width = n;
            r.height = 1;
            sink.fill_rect(r, px_color(v));
            i += n;
        }
    }
}

// Blit ke canvas window desktop di (x,y).
void draw_px(Canvas& canvas, int x, int y, const uint32_t* px, int w, int h);

class IconCache {
public:
    IconCache();
    // Ikon untuk path FS (hasil resolve_icon_path). null = tak ada gambar
    // sama sekali (default pun gagal) -> pemanggil pakai fallback warna.
    // Const: miss mengisi slot (mutable) — aman dipanggil dari draw().
    const IconPx* icon_for(const char* path) const;

private:
    struct Slot {
        char path[32];
        uint32_t* px;  // ICON_CACHE_PX x ICON_CACHE_PX, atau 0
        bool used;
        unsigned age;
    };
    mutable Slot slots_[ICON_CACHE_N];
    mutable unsigned tick_;
    const IconPx* load_into(int slot, const char* path) const;
};

}  // namespace desktop_impl

#endif
