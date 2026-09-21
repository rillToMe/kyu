// Kyuzen Desktop — implementasi notifikasi crash (port desktop.c).
//
// Baris teks kartu: judul + "task \"<nama>\" (exception|kernel_panic)" +
// "laporan: <path>" + hint. Jejak serial via print() (konsol di-mirror ke
// COM1 — berguna headless; preseden desktop lama).
#include "crash_notice.hpp"
#include "sys_abi.hpp"
#include "theme.hpp"

namespace desktop_impl {

using kyuzen::desktop::Damage;
using kyuzen::desktop::Rect;
using kyuzen::desktop::System;

CrashNotice::CrashNotice() : on_(false), until_ms_(0) {
    report_.pending = false;
    report_.crash_count = 0;
    report_.uptime_ms = 0;
    report_.vector = 0;
    report_.task_id = 0;
    report_.task_name[0] = '\0';
    report_.path[0] = '\0';
}

bool CrashNotice::probe() {
    if (!System::poll_crash(report_)) return false;
    on_ = true;
    until_ms_ = System::uptime_ms() + NOTIF_MS;
    print(const_cast<char*>("[desktop] notifikasi crash: "));
    print(report_.path);
    print(const_cast<char*>(" (dump ke-"));
    print_num(report_.crash_count);
    print(const_cast<char*>(")\n"));
    return true;
}

bool CrashNotice::update(uint64_t now_ms) {
    if (on_ && now_ms >= until_ms_) {
        close();
        print(const_cast<char*>(
            "[desktop] notifikasi crash ditutup (waktu habis)\n"));
        return true;
    }
    return false;
}

void CrashNotice::close() {
    on_ = false;
}

bool CrashNotice::card_rect(int screen_w, int* x, int* y) const {
    int cx = screen_w - NOTIF_W - NOTIF_MARGIN;
    if (cx < 0) cx = 0;
    if (x) *x = cx;
    if (y) *y = NOTIF_MARGIN;
    return true;
}

NoticeClick CrashNotice::on_click(Point p, int screen_w) {
    if (!on_) return NoticeClick::None;
    int x = 0, y = 0;
    card_rect(screen_w, &x, &y);
    Rect card;
    card.x = x;
    card.y = y;
    card.width = NOTIF_W;
    card.height = NOTIF_H;
    if (!card.contains(p)) {
        close();
        print(const_cast<char*>(
            "[desktop] notifikasi crash ditutup (klik)\n"));
        return NoticeClick::Close;
    }
    close();
    print(const_cast<char*>(
        "[desktop] notifikasi crash ditutup (buka File Manager)\n"));
    char fm[32];
    build_app_path(fm, sizeof(fm), "fileman.elf");
    System::spawn(fm);
    return NoticeClick::CloseAndOpenFm;
}

void CrashNotice::draw(Canvas& canvas) const {
    if (!on_) return;
    int x = 0, y = 0;
    card_rect(canvas.width(), &x, &y);

    Rect body;
    body.x = x;
    body.y = y;
    body.width = NOTIF_W;
    body.height = NOTIF_H;
    canvas.fill_rect(body, NOTIF_BG);
    Rect top;
    top.x = x;
    top.y = y;
    top.width = NOTIF_W;
    top.height = 2;
    canvas.fill_rect(top, NOTIF_EDGE);
    Rect left;
    left.x = x;
    left.y = y;
    left.width = 2;
    left.height = NOTIF_H;
    canvas.fill_rect(left, NOTIF_EDGE);

    Point p;
    p.x = x + 14;
    p.y = y + 12;
    canvas.draw_text("SISTEM PANIC pada boot sebelumnya", p, NOTIF_TITLE);

    char l2[64];
    int n = 0;
    const char* s = "task \"";
    while (*s && n < 60) l2[n++] = *s++;
    for (int i = 0; report_.task_name[i] && n < 60; i++)
        l2[n++] = report_.task_name[i];
    s = report_.vector == 0xFFFFu ? "\"  (kernel_panic)" : "\"  exception";
    while (*s && n < 60) l2[n++] = *s++;
    l2[n] = '\0';
    p.y = y + 32;
    canvas.draw_text(l2, p, NOTIF_TXT);

    char l3[64];
    n = 0;
    s = "laporan: ";
    while (*s && n < 60) l3[n++] = *s++;
    for (int i = 0; report_.path[i] && n < 60; i++) l3[n++] = report_.path[i];
    l3[n] = '\0';
    p.y = y + 50;
    canvas.draw_text(l3, p, NOTIF_TXT);

    p.y = y + 74;
    canvas.draw_text("klik: tutup  -  klik kartu ini: buka File Manager", p,
                     NOTIF_HINT);
}

}  // namespace desktop_impl
