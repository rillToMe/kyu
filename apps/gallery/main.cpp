// apps/gallery/main.cpp — entry Gallery.
//
// Penggunaan:
//   gallery                 → buka GALLERY_HOME ("/", lihat gallery.cpp)
//   gallery /path/folder    → buka direktori itu (argumen opsional; kernel
//                             memanggil entry dengan RDI=argc, RSI=argv)
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: ELF loader tidak
// menjalankan .init_array (apps/app.ld), jadi konstruktor objek
// namespace-scope tidak akan pernah dipanggil.
#include "gallery.hpp"

extern "C" int main(int argc, char** argv) {
    gal::Gallery app;
    app.run((argc > 1 && argv) ? argv[1] : 0);
    sys_exit();     // app selesai: kembali ke pemanggil (desktop/shell)
    return 0;       // tidak tercapai
}
