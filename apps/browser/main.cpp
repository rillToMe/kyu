// apps/browser/main.cpp — entry Browser (pola taskmgr/settings/filemanager:
// objek DI DALAM main, main BERLINKAGE C, keluar via sys_exit).
#include "browser_app.hpp"
#include "platform.hpp"

extern "C" int main(int argc, char** argv) {
    browser::BrowserApp app;
    const char* start = (argc > 1 && argv && argv[1]) ? argv[1] : nullptr;
    const int rc = app.run(start);
    sys_exit();  // kontrak: tak boleh return (ret = #GP)
    return rc;   // tidak tercapai
}
