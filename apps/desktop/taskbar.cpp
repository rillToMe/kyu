// Kyuzen Desktop — implementasi taskbar (port desktop generasi C).
#include "taskbar.hpp"
#include "theme.hpp"

namespace desktop_impl {

namespace {

int title_len(const char* t) {
    int n = 0;
    while (t[n]) n++;
    return n;
}

bool title_eq(const char* a, const char* b) {
    for (int j = 0; j < 32; j++) {
        if (a[j] != b[j]) return false;
        if (!a[j]) break;
    }
    return true;
}

// Lebar tombol = judul*8 + padding (mirror desktop lama).
int button_w(const WindowInfo& w) {
    return title_len(w.title) * 8 + 20;
}

}  // namespace

Taskbar::Taskbar() : nwins_(0) {
    for (int i = 0; i < MAX_WINS; i++) {
        wins_[i].id = 0;
        wins_[i].title[0] = '\0';
        wins_[i].focused = false;
        wins_[i].is_desktop = false;
    }
}

bool Taskbar::poll(WindowManager& wm) {
    WindowInfo tmp[MAX_WINS];
    int n = wm.get_windows(tmp, MAX_WINS);
    if (n < 0) return false;
    bool changed = (n != nwins_);
    if (!changed) {
        for (int i = 0; i < n; i++) {
            if (wins_[i].id != tmp[i].id ||
                wins_[i].focused != tmp[i].focused ||
                wins_[i].is_desktop != tmp[i].is_desktop ||
                !title_eq(wins_[i].title, tmp[i].title)) {
                changed = true;
                break;
            }
        }
    }
    nwins_ = n;
    for (int i = 0; i < n; i++) wins_[i] = tmp[i];
    return changed;
}

int Taskbar::find_button(Point p) const {
    int bx = 8;
    for (int i = 0; i < nwins_; i++) {
        if (wins_[i].is_desktop) continue;
        if (!wins_[i].title[0]) continue;
        int bw = button_w(wins_[i]);
        if (p.x >= bx && p.x < bx + bw) return i;
        bx += bw + 6;
    }
    return -1;
}

void Taskbar::draw(Canvas& canvas) const {
    int W = canvas.width();
    int H = canvas.height();
    Rect bg;
    bg.x = 0;
    bg.y = H - TB_H;
    bg.width = W;
    bg.height = TB_H;
    canvas.fill_rect(bg, TASK_BG);
    Rect edge;
    edge.x = 0;
    edge.y = H - TB_H;
    edge.width = W;
    edge.height = 1;
    canvas.fill_rect(edge, TASK_EDGE);
    int bx = 8;
    for (int i = 0; i < nwins_; i++) {
        if (wins_[i].is_desktop) continue;
        if (!wins_[i].title[0]) continue;
        int bw = button_w(wins_[i]);
        Rect btn;
        btn.x = bx;
        btn.y = H - TB_H + 4;
        btn.width = bw;
        btn.height = TB_H - 8;
        canvas.fill_rect(btn, wins_[i].focused ? TASK_ACTIVE : TASK_BTN);
        Point p;
        p.x = bx + 10;
        p.y = H - TB_H + 9;
        canvas.draw_text(wins_[i].title, p, TASK_TXT);
        bx += bw + 6;
    }
}

}  // namespace desktop_impl
