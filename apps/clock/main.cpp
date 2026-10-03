// apps/clock/main.cpp — entry aplikasi Clock.
//
// Objek aplikasi dibuat DI DALAM main, bukan sebagai global: ELF loader tidak
// menjalankan .init_array (apps/app.ld), jadi konstruktor namespace-scope
// tidak akan pernah dipanggil.
#include "app.hpp"

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    clock_app::ClockApp app;
    app.run();
    sys_exit();   // app selesai: kembali ke pemanggil (desktop/shell)
    return 0;     // tidak tercapai
}
