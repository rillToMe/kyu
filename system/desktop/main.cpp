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
    desktop_impl::DesktopShell shell;
    return app.run(shell);
}
