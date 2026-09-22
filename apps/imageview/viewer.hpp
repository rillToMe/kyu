// apps/imageview/viewer.hpp — UI ImageView (satu gambar penuh-resolusi).
//
// Pembagian tanggung jawab:
//   image.hpp  — metadata + status + konteks direktori (tanpa piksel)
//   viewer.*   — window/toolbar/statusbar + transform zoom/pan
//   ui::Image (toolkit, lewat ABI ui_image_*) — pemilik buffer piksel + gambar
//   ScrollView mode pan (toolkit) — viewport, fit-center, dan pan (drag + panah)
//
// Tidak ada buffer piksel milik aplikasi: mengganti gambar = satu
// ui_image_set_file() (toolkit membebaskan buffer lama), mengganti zoom =
// ui_image_set_scale() (toolkit menghitung ulang ukuran target, TANPA
// mengalokasikan bitmap per level zoom — Painter mem-blit nearest-neighbor).
#ifndef IMAGEVIEW_VIEWER_HPP
#define IMAGEVIEW_VIEWER_HPP

// Header platform C: harus berlinkage C (tanpa ini, simbol syscall menjadi
// termangled C++ dan link gagal). libui.h punya guard extern "C" sendiri.
extern "C" {
#include "userlib.h"
#include "libgui.h"
}
#include "libui.h"

#include "image.hpp"

namespace iv {

class Viewer {
public:
    Viewer();

    // Bangun UI + buka `initial_path`. Return 0 selalu (keluar lewat sys_exit
    // di main). Path kosong = tidak ada argumen → keadaan error yang rapi.
    int run(const char* initial_path);

    // --- Perintah (dipanggil toolbar/menu/shortcut lewat thunk C) ---
    void cmd_zoom_in();
    void cmd_zoom_out();
    void cmd_fit();
    void cmd_actual();
    void cmd_next();
    void cmd_prev();
    void cmd_close();

private:
    ui_window_t* win_;
    ui_widget_t* image_;    // ui::Image — satu gambar penuh-resolusi
    ui_widget_t* scroll_;   // ScrollView mode pan (viewport + drag/panah pan)
    ui_widget_t* status_;
    ImageDocument doc_;
    ImageFolder folder_;
    int  zoom_;             // persen yang sedang ditampilkan
    bool fit_;              // true = skala dihitung dari ukuran view (auto)
    bool failed_;           // true = tidak ada gambar yang bisa ditampilkan

    void build_ui();
    void load_path(const char* path);
    void load_index(int i);
    void apply_fit();
    void set_zoom(int percent);
    void refresh_status();
    void show_error_dialog();
};

}  // namespace iv

#endif // IMAGEVIEW_VIEWER_HPP
