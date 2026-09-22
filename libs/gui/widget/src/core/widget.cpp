// libs/widget/src/core/widget.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "core/widget.hpp"
#include "window/window.hpp"

namespace ui {

void Widget::set_visible(bool on) {
    if (visible == on) return;
    visible = on;
    // Tata letak berubah → seluruh window digambar ulang (widget lain bergeser).
    if (owner) owner->damage_full();
}

} // namespace ui
