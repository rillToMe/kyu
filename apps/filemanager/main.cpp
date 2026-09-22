// apps/filemanager/main.cpp — entry File Manager.
//
// Konvensi entry sama dengan Gallery dan SDK C++ (libs/cpp/include/kyuzen/app.hpp):
// crt `_start` memanggil `main` BERLINKAGE C dengan RDI=argc, RSI=argv.
//
// Penggunaan:
//   fileman                 → buka "/"
//   fileman /path/folder    → buka direktori itu (argv[1] opsional)
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: konsisten dengan
// Gallery/desktop dan tidak bergantung pada jalannya .init_array.
#include "app.hpp"

extern "C" int main(int argc, char** argv) {
    fm::FileManagerApp app;
    const int rc = app.run((argc > 1 && argv) ? argv[1] : nullptr);
    sys_exit();      // app selesai: kembali ke pemanggil (desktop/shell)
    return rc;       // tidak tercapai
}
