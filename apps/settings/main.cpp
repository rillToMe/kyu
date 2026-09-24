// apps/settings/main.cpp — entry Settings.
//
// Konvensi entry sama dengan File Manager dan SDK C++
// (libs/cpp/include/kyuzen/app.hpp): crt `_start` memanggil `main`
// BERLINKAGE C dengan RDI=argc, RSI=argv.
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: konsisten dengan
// Gallery/desktop dan tidak bergantung pada jalannya .init_array.
#include "settings.hpp"

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    settings::SettingsApp app;
    const int rc = app.run();
    sys_exit();  // app selesai: kembali ke pemanggil (desktop/shell)
    return rc;   // tidak tercapai
}
