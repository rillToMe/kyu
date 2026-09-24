// Kyuzen Desktop — entry: HANYA komposisi + startup.
//
// Framework (Application) memiliki loop; DesktopShell memiliki kebijakan.
// Tidak ada logika desktop di file ini.
#include <kyuzen/desktop/application.hpp>
#include "desktop_shell.hpp"

extern "C" int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    kyuzen::desktop::Application app;
    // Shell ~6KB: static, BUKAN stack. Stack user 8KB dengan puncak
    // 0xC000000 — ctor IconCache pernah zeroing menembus puncak itu
    // (write fault 0x0C000000 tepat setelah login). Function-local static:
    // ctor berjalan saat entry lewat guard (__cxa_guard_acquire ada di
    // kyuzen_cxx_runtime), tanpa .init_array yang tak dijalankan loader.
    static desktop_impl::DesktopShell shell;
    return app.run(shell);
}
