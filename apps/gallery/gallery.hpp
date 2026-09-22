// apps/gallery/gallery.hpp — aplikasi Gallery (media browser).
//
// Tanggung jawab Gallery: menemukan berkas gambar, menampilkan kisi thumbnail,
// dan meluncurkan ImageView untuk berkas yang dipilih. Gallery TIDAK menggambar
// gambar penuh-resolusi dan tidak tahu apa-apa soal zoom/pan — itu milik
// ImageView (dua proses terpisah; satu executable tidak menggabungkan keduanya).
//
// Modul:
//   platform.hpp — header platform C (linkage C)
//   thumbs.*     — cache thumbnail berbatas + pembuatan thumbnail
//   media.h/libs/media/media.c — deteksi tipe, util path, pemindaian direktori,
//                          format ukuran, probe metadata (dipakai bersama
//                          ImageView; tidak ada salinan per-app)
//   main.cpp     — entry + argumen
#ifndef GALLERY_GALLERY_HPP
#define GALLERY_GALLERY_HPP

#include "platform.hpp"
#include "thumbs.hpp"

namespace gal {

class Gallery {
public:
    Gallery();
    int run(const char* initial_dir);

    // --- Perintah (dipanggil menu/toolbar/shortcut lewat thunk C) ---
    void cmd_open();
    void cmd_back();
    void cmd_rescan();
    void cmd_close();

    // Tick periodik toolkit (~60/s): memuat SATU thumbnail per frame untuk sel
    // yang terlihat (pemuatan bertahap, tanpa thread). Return 1 = ada perubahan.
    int  tick();

    void on_selection_changed();
    void on_activate();
    void on_thumb_evicted(const char* name);
    void on_dialog_done(int index);

private:
    enum { DIR_STACK_MAX = 8 };
    // 0 = belum ada thumbnail, 1 = selesai (atau folder), 2 = gagal decode.
    enum { CELL_PENDING = 0, CELL_DONE = 1, CELL_ERROR = 2 };

    ui_window_t* win_;
    ui_widget_t* grid_;
    ui_widget_t* status_;

    media_entry_t entries_[MEDIA_MAX_ENTRIES];
    int  count_;
    char dir_[MEDIA_PATH_MAX];
    char stack_[DIR_STACK_MAX][MEDIA_PATH_MAX];        // direktori induk
    char stack_name_[DIR_STACK_MAX][MEDIA_NAME_MAX];   // folder yang dimasuki
    int  depth_;
    uint8_t state_[MEDIA_MAX_ENTRIES];
    int  cursor_;          // rotasi sel berikutnya yang perlu thumbnail
    ThumbCache thumbs_;

    void build_ui();
    void scan(const char* dir);
    void refresh_status();
    void load_visible();
    void clear_cell(int i);
    int  find_cell(const char* name) const;
    int  selected_entry() const;
    bool open_entry(int i);
    void launch_viewer(const char* path);
    void show_error(const char* title, const char* text);
};

}  // namespace gal

#endif // GALLERY_GALLERY_HPP
