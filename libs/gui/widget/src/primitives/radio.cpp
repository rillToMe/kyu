// libs/widget/src/primitives/radio.cpp — definisi butuh-Window radio.hpp.
#include "primitives/radio.hpp"
#include "window/window.hpp"

namespace ui {

void Radio::on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) {
    (void)mods;
    if (!enabled) return;
    uint32_t sc = scancode & 0xFF;
    if (sc == 0x48 || sc == 0x4B || sc == 0x50 || sc == 0x4D) {
        // Panah = pindah + pilih + fokus (gaya native radio group).
        int dir = (sc == 0x48 || sc == 0x4B) ? -1 : 1;
        Radio* t = sibling(dir);
        if (t) {
            mark_dirty();
            if (group) group->select(t, true);
            if (owner) owner->set_focus(t);
        }
        return;
    }
    if (ascii == '\n' || ascii == '\r' || ascii == ' ' || sc == 0x1C)
        on_click(0, 0);
}

} // namespace ui
