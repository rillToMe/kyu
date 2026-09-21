// Kyuzen Desktop — implementasi taskbar Phase 9.
#include "taskbar.hpp"
#include "sys_abi.hpp"
#include "theme.hpp"

namespace desktop_impl {

namespace {

void put2(char* d, unsigned v) {
    if (v > 99) v = 99;
    d[0] = static_cast<char>('0' + v / 10);
    d[1] = static_cast<char>('0' + v % 10);
}

void put4(char* d, unsigned v) {
    if (v > 9999) v = 9999;
    d[0] = static_cast<char>('0' + (v / 1000) % 10);
    d[1] = static_cast<char>('0' + (v / 100) % 10);
    d[2] = static_cast<char>('0' + (v / 10) % 10);
    d[3] = static_cast<char>('0' + v % 10);
}

int tlen(const char* t) {
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

bool shown(const WindowInfo& w) {
    return !w.is_desktop && w.title[0];
}

}  // namespace

Taskbar::Taskbar() : nwins_(0), nslots_(0), hovered_(-1) {
    for (int i = 0; i < MAX_WINS; i++) {
        wins_[i].id = 0;
        wins_[i].title[0] = '\0';
        wins_[i].focused = false;
        wins_[i].is_desktop = false;
        slot_win_[i] = -1;
        slot_app_[i] = 0;
    }
    time_[0] = '\0';
    date_[0] = '\0';
}

Rect Taskbar::slot_rect(int s, int w, int h) {
    Rect r;
    r.x = TB_PAD + s * (TB_SLOT_W + TB_SLOT_GAP);
    r.y = h - TB_H;
    r.width = TB_SLOT_W;
    r.height = TB_H;
    (void)w;
    return r;
}

Rect Taskbar::slot_icon_rect(int s, int w, int h) {
    Rect slot = slot_rect(s, w, h);
    Rect r;
    r.x = slot.x + (TB_SLOT_W - TB_ICON_PX) / 2;
    r.y = slot.y + (TB_H - TB_ICON_PX) / 2;
    r.width = TB_ICON_PX;
    r.height = TB_ICON_PX;
    return r;
}

void Taskbar::rebuild_slots(const Launcher& launcher, int w) {
    (void)w;
    nslots_ = 0;
    for (int i = 0; i < nwins_ && nslots_ < MAX_WINS; i++) {
        if (!shown(wins_[i])) continue;
        slot_win_[nslots_] = i;
        slot_app_[nslots_] = launcher.find_by_title(wins_[i].title);
        nslots_++;
    }
    if (hovered_ >= nslots_) hovered_ = -1;
}

bool Taskbar::poll(WindowManager& wm, const Launcher& launcher) {
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

    // Jam RTC: berubah tiap menit ( + tanggal). Granularitas menit =
    // Partial sekali per menit, bukan tiap detik.
    uint32_t t[6];
    sys_get_time(t);
    char nt[8];
    put2(nt, t[3]);
    nt[2] = ':';
    put2(nt + 3, t[4]);
    nt[5] = '\0';
    char nd[16];
    put2(nd, t[2]);
    nd[2] = ' ';
    put2(nd + 3, t[1]);
    nd[5] = ' ';
    put4(nd + 6, t[0]);
    nd[10] = '\0';
    if (!title_eq(time_, nt) || !title_eq(date_, nd)) {
        for (int i = 0; i < 8; i++) {
            time_[i] = nt[i];
            if (!nt[i]) break;
        }
        for (int i = 0; i < 16; i++) {
            date_[i] = nd[i];
            if (!nd[i]) break;
        }
        changed = true;
    }

    if (changed) {
        // Lebar layar tak diketahui di sini; kecocokan app tak tergantung
        // lebar (slot dibatasi saat hit/gambar). Rebuild tanpa clip lebar.
        rebuild_slots(launcher, 0);
    }
    return changed;
}

int Taskbar::find_slot(Point p, int w, int h) const {
    if (p.y < h - TB_H) return -1;
    int avail = w - TB_PAD - TB_SYS_W - TB_PAD;
    int maxs = avail / (TB_SLOT_W + TB_SLOT_GAP);
    int n = nslots_ < maxs ? nslots_ : maxs;
    for (int s = 0; s < n; s++) {
        if (slot_rect(s, w, h).contains(p)) return s;
    }
    return -1;
}

bool Taskbar::update_hover(Point p, int w, int h) {
    int s = find_slot(p, w, h);
    if (s == hovered_) return false;
    hovered_ = s;
    return true;
}

int Taskbar::draw(Canvas& canvas, const IconCache& icons, int W, int H) const {
    int y0 = H - TB_H;
    // Strip: SATU fill_rect opaque. Tidak ada lagi pita/lubang transparan —
    // lubang alpha-0 hanya terlihat bila compositor menampilkan base_canvas,
    // dan window desktop full-screen opaque membuat base tak pernah tampil
    // (itu sebab ikon taskbar "hilang" di Phase 9 awal).
    Rect strip;
    strip.x = 0;
    strip.y = y0;
    strip.width = W;
    strip.height = TB_H;
    canvas.fill_rect(strip, TASK_BG);
    Rect edge;
    edge.x = 0;
    edge.y = y0;
    edge.width = W;
    edge.height = 1;
    canvas.fill_rect(edge, TASK_EDGE);

    int avail = W - TB_PAD - TB_SYS_W - TB_PAD;
    int maxs = avail / (TB_SLOT_W + TB_SLOT_GAP);
    int n = nslots_ < maxs ? nslots_ : maxs;
    int nimg = 0;
    for (int s = 0; s < n; s++) {
        Rect slot = slot_rect(s, W, H);
        Rect ir = slot_icon_rect(s, W, H);
        const WindowInfo& win = wins_[slot_win_[s]];
        bool hot = (s == hovered_);
        // Latar slot: normal = TASK_BTN redup (tanpa sorot); hover/fokus =
        // sorot penuh (ikon digambar di atasnya, urutan jelas).
        Color bgc = TASK_BTN;
        if (win.focused)
            bgc = TASK_ACTIVE;
        else if (hot)
            bgc = TASK_HOVER;
        if (hot || win.focused) canvas.fill_rect(slot, bgc);
        // Garis status fokus di bawah slot.
        if (win.focused) {
            Rect bar;
            bar.x = slot.x + 4;
            bar.y = slot.y + slot.height - TB_FOCUS_H - 2;
            bar.width = TB_SLOT_W - 8;
            bar.height = TB_FOCUS_H;
            canvas.fill_rect(bar, TASK_TXT);
        }
        char path[32];
        const AppEntry* app = slot_app_[s];
        resolve_icon_path(app ? app->icon : 0, path, sizeof(path));
        const IconPx* ic = icons.icon_for(path);
        if (ic) {
            // Cache menyimpan 48px; taskbar butuh 28px -> perkecil ke buffer
            // stack lalu RLE ke canvas (tanpa alokasi).
            if (ic->size == TB_ICON_PX) {
                draw_px(canvas, ir.x, ir.y, ic->px, TB_ICON_PX, TB_ICON_PX);
            } else {
                uint32_t tmp[TB_ICON_PX * TB_ICON_PX];
                scale_nearest(ic->px, ic->size, ic->size, tmp, TB_ICON_PX,
                              TB_ICON_PX);
                draw_px(canvas, ir.x, ir.y, tmp, TB_ICON_PX, TB_ICON_PX);
            }
            nimg++;
        } else {
            // Fallback tanpa gambar: kotak warna + inisial judul.
            Color fc = app ? app->color : APP_DEFAULT;
            canvas.fill_rect(ir, fc);
            if (win.title[0]) {
                char init[2];
                init[0] = win.title[0];
                init[1] = '\0';
                Point tp;
                tp.x = ir.x + (TB_ICON_PX - 8) / 2;
                tp.y = ir.y + (TB_ICON_PX - 16) / 2;
                canvas.draw_text(init, tp, TASK_TXT);
            }
        }
    }

    // Area sistem kanan: pemisah + jam (utama) + tanggal numerik (sekunder).
    int sx = W - TB_SYS_W;
    Rect sep;
    sep.x = sx;
    sep.y = y0 + 6;
    sep.width = 1;
    sep.height = TB_H - 12;
    canvas.fill_rect(sep, TASK_EDGE);
    Point p;
    p.y = y0 + 5;
    int nt = tlen(time_);
    p.x = W - TB_PAD - nt * 8;
    canvas.draw_text(time_, p, SYS_TXT);
    p.y = y0 + 24;
    int nd = tlen(date_);
    p.x = W - TB_PAD - nd * 8;
    canvas.draw_text(date_, p, SYS_DIM);
    return nimg;
}

}  // namespace desktop_impl
