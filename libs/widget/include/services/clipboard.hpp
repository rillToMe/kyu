// libs/widget/include/services/clipboard.hpp — clipboard teks toolkit.
#ifndef KWIDGET_SERVICES_CLIPBOARD_HPP
#define KWIDGET_SERVICES_CLIPBOARD_HPP

// Clipboard — buffer teks global toolkit (Phase 9). Cross-app butuh IPC
// kernel (shared memory) — sengaja di luar scope phase ini.
// ponytail: satu buffer global, bukan per-app/per-window; upgrade bila
// multi-app clipboard dibutuhkan (phase kernel + protokol).
// g_clipboard tetap zero-initialized (trivial) di clipboard.cpp.

void clipboard_set(const char* s);
const char* clipboard_get();
void clipboard_clear();

#endif // KWIDGET_SERVICES_CLIPBOARD_HPP
