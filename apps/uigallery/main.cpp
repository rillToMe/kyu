// apps/uigallery/main.cpp — entry Galeri UI libui.
//
// Galeri ini adalah REFERENSI VISUAL KyuzenOS: ia memakai widget produksi
// (bukan komponen tiruan untuk presentasi) dan menunjukkan seluruh bahasa
// visual — tipografi, warna, state interaktif, navigasi, dialog — di kedua
// mode (terang/gelap).
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: ELF loader tidak
// menjalankan .init_array (apps/app.ld), jadi konstruktor namespace-scope
// tidak akan pernah dipanggil.
#include "gallery.hpp"

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    gal::UiGalleryApp app;
    app.run();
    sys_exit();     // app selesai: kembali ke pemanggil (desktop/shell)
    return 0;       // tidak tercapai
}
