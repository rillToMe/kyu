// Kyuzen Desktop — implementasi kartu preview statis.
#include "app_preview.hpp"
#include "theme.hpp"

namespace desktop_impl {

namespace {

int tlen(const char* t) {
    int n = 0;
    while (t[n] && n < 32) n++;
    return n;
}

}  // namespace

AppPreview::AppPreview()
    : on_(false), win_id_(0), focused_(false), app_(0) {
    title_[0] = '\0';
    card_.x = card_.y = card_.width = card_.height = 0;
    icon_.x = icon_.y = icon_.width = icon_.height = 0;
    drawn_ = card_;
}

Rect AppPreview::card_rect(Rect anchor, int wscr, int hscr) {
    Rect c;
    c.width = PV_W;
    c.height = PV_H;
    c.x = anchor.x + (anchor.width - PV_W) / 2;
    if (c.x < TB_PAD) c.x = TB_PAD;
    if (c.x + PV_W > wscr - TB_PAD) c.x = wscr - TB_PAD - PV_W;
    if (c.x < 0) c.x = 0;
    c.y = anchor.y - PV_GAP - PV_H;
    if (c.y < TB_PAD) c.y = TB_PAD;  // layar pendek: timpa ke dalam
    (void)hscr;
    return c;
}

void AppPreview::show(const WindowInfo& w, const AppEntry* app, Rect anchor,
                      int wscr, int hscr) {
    on_ = true;
    win_id_ = w.id;
    focused_ = w.focused;
    app_ = app;
    for (int i = 0; i < 31; i++) {
        title_[i] = w.title[i];
        if (!w.title[i]) break;
    }
    title_[31] = '\0';
    card_ = card_rect(anchor, wscr, hscr);
    icon_.x = card_.x + PV_PAD;
    icon_.y = card_.y + PV_PAD;
    icon_.width = ICON_CACHE_PX;
    icon_.height = ICON_CACHE_PX;
}

void AppPreview::hide() {
    on_ = false;
    // drawn_ sengaja DIBIARKAN: pemanggil memakainya untuk memulihkan
    // wallpaper di bekas kartu pada render Partial berikutnya.
}

bool AppPreview::hit(Point p) const {
    return on_ && card_.contains(p);
}

void AppPreview::draw(Canvas& canvas, const IconCache& icons) {
    if (!on_) {
        drawn_ = card_;
        drawn_.width = 0;
        drawn_.height = 0;
        return;
    }
    // Kartu opaque digambar penuh (latar + ikon di atasnya). Tidak ada lagi
    // strategi "lubang ikon": base_canvas tak pernah terlihat di window
    // desktop full-screen opaque.
    canvas.fill_rect(card_, PV_BG);
    Rect edge;
    edge.x = card_.x;
    edge.y = card_.y;
    edge.width = card_.width;
    edge.height = 2;
    canvas.fill_rect(edge, PV_EDGE);
    Rect ledge;
    ledge.x = card_.x;
    ledge.y = card_.y;
    ledge.width = 2;
    ledge.height = card_.height;
    canvas.fill_rect(ledge, PV_EDGE);

    char path[32];
    resolve_icon_path(app_ ? app_->icon : 0, path, sizeof(path));
    const IconPx* ic = icons.icon_for(path);
    if (ic) {
        draw_px(canvas, icon_.x, icon_.y, ic->px, ic->size, ic->size);
    } else {
        Color fc = app_ ? app_->color : APP_DEFAULT;
        canvas.fill_rect(icon_, fc);
        if (title_[0]) {
            char init[2];
            init[0] = title_[0];
            init[1] = '\0';
            Point tp;
            tp.x = icon_.x + (ICON_CACHE_PX - 8) / 2;
            tp.y = icon_.y + (ICON_CACHE_PX - 16) / 2;
            canvas.draw_text(init, tp, TASK_TXT);
        }
    }
    // Judul (dipotong lebar kartu) + status + hint.
    int maxc = (PV_W - PV_PAD * 2 - ICON_CACHE_PX - 8) / 8;
    if (maxc < 1) maxc = 1;
    if (maxc > 31) maxc = 31;
    char line[32];
    int n = tlen(title_);
    if (n > maxc) n = maxc;
    for (int i = 0; i < n; i++) line[i] = title_[i];
    line[n] = '\0';
    Point p;
    p.x = icon_.x + ICON_CACHE_PX + 8;
    p.y = card_.y + PV_PAD + 4;
    canvas.draw_text(line, p, PV_TITLE);
    p.y = card_.y + PV_PAD + 26;
    canvas.draw_text(focused_ ? "Aktif" : "Berjalan", p, PV_DIM);
    p.y = card_.y + PV_H - PV_PAD - 16;
    p.x = card_.x + PV_PAD;
    canvas.draw_text("klik: tampilkan", p, PV_DIM);

    drawn_ = card_;
}

}  // namespace desktop_impl
