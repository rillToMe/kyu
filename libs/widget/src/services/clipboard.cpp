// libs/widget/src/services/clipboard.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "services/clipboard.hpp"
#include "runtime/memory.hpp"

static char* g_clipboard = 0;
void clipboard_set(const char* s) {
    char* n = _ui_strdup(s ? s : "");
    if (!n) return;
    _ui_free(g_clipboard);
    g_clipboard = n;
}
const char* clipboard_get() { return g_clipboard ? g_clipboard : ""; }
void clipboard_clear() { _ui_free(g_clipboard); g_clipboard = 0; }
