// user_apps/imageview/image.hpp — dokumen gambar + konteks direktori.
//
// Modul ini HANYA tahu metadata dan status sebuah berkas gambar; ia tidak
// menyimpan satu piksel pun. Piksel dipegang toolkit (ui_image_*: satu buffer
// penuh-resolusi untuk SATU gambar, di-free toolkit saat gambar diganti),
// jadi tidak ada salinan buffer di sisi aplikasi.
//
// Semua API di sini berasal dari library bersama:
//   media.h   — deteksi tipe, util path, pemindaian direktori, probe header
//   apps/png.c — dekode piksel (dipanggil toolkit, bukan modul ini)
#ifndef IMAGEVIEW_IMAGE_HPP
#define IMAGEVIEW_IMAGE_HPP

#include <stdint.h>
#include "media.h"

namespace iv {

// Kenapa gambar tidak bisa ditampilkan — dipakai UI untuk memilih pesan error.
enum ImageState {
    IMG_OK = 0,
    IMG_MISSING,       // berkas tidak ada / tak bisa dibuka
    IMG_UNSUPPORTED,   // ada dekoder-nya tapi belum terpasang (.jpg hari ini)
    IMG_UNDECODABLE,   // header oke, isi rusak / OOM saat decode
};

// Satu gambar yang sedang dibuka.
class ImageDocument {
public:
    char     path[MEDIA_PATH_MAX];
    char     dir[MEDIA_PATH_MAX];
    char     name[MEDIA_NAME_MAX];
    char     format[8];        // "PNG" / "BMP" / "" (dari probe header)
    int      width, height;    // dari probe; 0 bila tak diketahui
    uint32_t size;             // byte
    ImageState state;

    ImageDocument();

    // Buka path: cek keberadaan + tipe + probe header (TANPA decode penuh).
    // Return true bila dokumen siap ditampilkan (decode menyusul di toolkit).
    bool open(const char* path);

    // Toolkit gagal men-decode isi berkas (header lolos probe, isi rusak/OOM).
    void mark_decode_failed();

    bool ready() const { return state == IMG_OK; }
    const char* error_title() const;
    const char* error_text() const;
};

// Daftar gambar yang bisa ditampilkan di dalam satu direktori + posisi gambar
// tertentu di dalamnya. Inilah "konteks" yang membuat Next/Previous mungkin
// tanpa IPC apa pun: ImageView memindai direktori gambar yang sedang dibuka.
class ImageFolder {
public:
    media_entry_t entries[MEDIA_MAX_ENTRIES];
    int count;
    int index;      // posisi gambar aktif; -1 = tidak ketemu di daftar

    ImageFolder();

    // Pindai direktori sebuah path dan cari `path` di dalamnya. Hanya gambar
    // yang BENAR-BENAR bisa didekode yang masuk daftar (Next/Previous tidak
    // boleh mendarat di format yang belum didukung).
    void scan_for(const char* path);

    bool has_nav() const { return count > 1 && index >= 0; }
    int next_index() const;      // -1 bila tidak ada
    int prev_index() const;      // -1 bila tidak ada
    const char* path_at(int i) const;
};

}  // namespace iv

#endif // IMAGEVIEW_IMAGE_HPP
