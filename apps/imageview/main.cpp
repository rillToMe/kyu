// apps/imageview/main.cpp — entry ImageView (Image Viewer).
//
// Penggunaan:
//   imageview /path/to/image.png
//
// Gallery meluncurkannya lewat sys_spawn_argv dengan argv[1] = path berkas.
// Kernel memanggil entry ELF dengan RDI = argc, RSI = argv (create_user_task,
// jalur argumen P0 Phase 2), jadi main(argc, argv) langsung menerima argumen
// proses — tidak perlu file hand-off seperti view.tmp generasi lama.
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: ELF loader tidak
// menjalankan .init_array (lihat apps/app.ld + catatan di
// apps/Makefile), jadi konstruktor objek namespace-scope tidak akan
// pernah dipanggil.
#include "viewer.hpp"

extern "C" int main(int argc, char** argv) {
    iv::Viewer viewer;
    viewer.run((argc > 1 && argv) ? argv[1] : 0);
    sys_exit();     // app selesai: kembali ke pemanggil (desktop/shell)
    return 0;       // tidak tercapai
}
