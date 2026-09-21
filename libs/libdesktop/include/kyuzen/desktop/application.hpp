// libdesktop — lifecycle aplikasi desktop (framework memiliki loop).
//
// run(): buat surface desktop → shell.on_start() → render Full pertama →
// loop { poll event → dispatch → poll berkala → flush bila ber-damage →
// yield } → shutdown. Mengembalikan 0 bila shell berhenti sendiri;
// nonzero bila surface gagal dibuat (desktop kedua yang konflik, OOM).
#ifndef KYUZEN_DESKTOP_APPLICATION_HPP
#define KYUZEN_DESKTOP_APPLICATION_HPP

namespace kyuzen {
namespace desktop {

class Shell;

class Application {
public:
    int run(Shell& shell);
};

}  // namespace desktop
}  // namespace kyuzen

#endif
