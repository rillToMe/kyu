// Kyuzen Desktop — notifikasi crash: kartu sudut kanan atas, sekali tampil.
//
// Kebijakan implementasi: probe via System::poll_crash saat startup, timeout
// NOTIF_MS, tutup via klik (di kartu → buka File Manager; di luar → hanya
// tutup) atau waktu habis. Penutup selalu lewat close() (idempoten) agar flag
// + permintaan repaint penuh tetap satu tempat.
#ifndef KYUZEN_DESKTOP_IMPL_CRASH_NOTICE_HPP
#define KYUZEN_DESKTOP_IMPL_CRASH_NOTICE_HPP

#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/system.hpp>

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::CrashReport;
using kyuzen::desktop::Point;

enum class NoticeClick {
    None,            // di luar kartu (atau tak aktif) — klik diteruskan
    Close,           // di luar kartu saat aktif — tutup, klik diteruskan
    CloseAndOpenFm,  // di dalam kartu — tutup + buka File Manager, dikonsumsi
};

class CrashNotice {
public:
    CrashNotice();

    // Cek sekali saat startup; true = kartu tampil (boot setelah panic).
    bool probe();
    bool visible() const { return on_; }

    // Timeout: true = baru saja kedaluwarsa (pemanggil minta render Full).
    bool update(uint64_t now_ms);

    NoticeClick on_click(Point p, int screen_w);
    void draw(Canvas& canvas) const;

private:
    void close();
    bool card_rect(int screen_w, int* x, int* y) const;

    CrashReport report_;
    bool on_;
    uint64_t until_ms_;
};

}  // namespace desktop_impl

#endif
