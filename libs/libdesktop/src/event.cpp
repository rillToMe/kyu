// libdesktop backend — penerjemah event kernel → Event framework.
//
// Satu-satunya TU yang menyentuh pompa event mentah. Mapping (perilaku
// desktop generasi C, didokumentasikan di sini):
//   EVENT_MOUSE_MOVE  → MouseMove (pos = param1/param2)
//   EVENT_MOUSE_CLICK → MouseButton (button = param1, pressed = param2==1)
//                       posisi klik TIDAK dibawa event (konvensi KWM);
//                       shell melacak posisi dari MouseMove terakhir.
//   EVENT_KEY_PRESS / EVENT_KEY_RELEASE → Key (key = param1, modifiers = param2)
//   EVENT_WIN_CLOSE   → Quit (window_id = win_id)
//   lainnya (SCROLL, NONE, tak dikenal) → None (diabaikan framework)
#include <kyuzen/desktop/event.hpp>

extern "C" {
#include "userlib.h"
}

namespace kyuzen {
namespace desktop {

namespace {

Event translate_raw(const kyuzen_event_t& raw) {
    Event e = no_event();
    if (raw.type == EVENT_MOUSE_MOVE) {
        e.type = EventType::MouseMove;
        e.pos.x = raw.param1;
        e.pos.y = raw.param2;
    } else if (raw.type == EVENT_MOUSE_CLICK) {
        e.type = EventType::MouseButton;
        e.button = raw.param1;
        e.pressed = (raw.param2 == 1);
    } else if (raw.type == EVENT_KEY_PRESS || raw.type == EVENT_KEY_RELEASE) {
        e.type = EventType::Key;
        e.key = raw.param1;
        e.modifiers = raw.param2;
    } else if (raw.type == EVENT_WIN_CLOSE) {
        e.type = EventType::Quit;
        e.window_id = static_cast<uint32_t>(raw.win_id);
    }
    return e;
}

}  // namespace

bool EventPoller::poll(Event& out) {
    kyuzen_event_t raw;
    raw.type = EVENT_NONE;
    raw.param1 = 0;
    raw.param2 = 0;
    raw.param3 = 0;
    raw.win_id = 0;
    if (!sys_get_event(&raw)) {
        out = no_event();
        return false;
    }
    out = translate_raw(raw);
    return true;
}

}  // namespace desktop
}  // namespace kyuzen
