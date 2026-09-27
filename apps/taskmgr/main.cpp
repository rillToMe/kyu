// apps/taskmgr/main.cpp — entry Task Manager.
//
// Konvensi entry sama dengan Settings dan File Manager (libs/cpp/include/
// kyuzen/app.hpp): crt `_start` memanggil `main` BERLINKAGE C dengan
// RDI=argc, RSI=argv.
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: konsisten dengan
// Settings/Gallery/desktop dan tidak bergantung pada jalannya .init_array.
#include "task_manager.hpp"

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    taskmgr::TaskManagerApp app;
    const int rc = app.run();
    sys_exit();  // app selesai: kembali ke pemanggil (desktop/shell)
    return rc;   // tidak tercapai
}
