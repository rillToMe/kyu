// Kyuzen Desktop — implementasi launcher (port desktop generasi C).
#include "launcher.hpp"
#include "sys_abi.hpp"
#include "theme.hpp"

namespace desktop_impl {

namespace {

int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

bool neq(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

// Manifest "<base>.app": satu "key=value" per baris (name/color/hidden).
// Baris tanpa '=' diabaikan. `exec` TIDAK dipakai (binary dari scan).
void parse_manifest(const char* b, AppEntry* e, bool* hidden) {
    int i = 0;
    while (b[i]) {
        int ls = i;
        while (b[i] && b[i] != '\n') i++;
        int le = i;
        if (b[i]) i++;
        int eq = ls;
        while (eq < le && b[eq] != '=') eq++;
        if (eq >= le) continue;
        const char* k = &b[ls];
        int klen = eq - ls;
        const char* v = &b[eq + 1];
        int vlen = le - (eq + 1);
        while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' '))
            vlen--;
        if (klen == 4 && neq(k, "name", 4)) {
            int n = vlen > 31 ? 31 : vlen;
            for (int j = 0; j < n; j++) e->label[j] = v[j];
            e->label[n] = '\0';
        } else if (klen == 5 && neq(k, "color", 5)) {
            e->color = parse_color(v);
        } else if (klen == 6 && neq(k, "hidden", 6)) {
            *hidden = (vlen > 0 && v[0] != '0');
        }
    }
}

}  // namespace

Color parse_color(const char* s) {
    unsigned v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (int i = 2;; i++) {
            char c = s[i];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                break;
            v = v * 16 + static_cast<unsigned>(d);
        }
    } else {
        for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++)
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
    }
    v &= 0xFFFFFFu;
    return rgb(static_cast<uint8_t>((v >> 16) & 0xFF),
               static_cast<uint8_t>((v >> 8) & 0xFF),
               static_cast<uint8_t>(v & 0xFF));
}

Launcher::Launcher() : napps_(0), checksum_(0) {
    for (int i = 0; i < MAX_APPS; i++) {
        apps_[i].label[0] = '\0';
        apps_[i].elf[0] = '\0';
        apps_[i].color = APP_DEFAULT;
    }
}

bool Launcher::discover() {
    const int kMaxFiles = 64;
    const int kManifestMax = 512;
    file_info_t files[kMaxFiles];
    char mbuf[kManifestMax];
    int n = sys_get_file_list(const_cast<char*>("/apps"), files, kMaxFiles);
    if (n < 0) n = 0;

    napps_ = 0;
    for (int i = 0; i < n && napps_ < MAX_APPS; i++) {
        const char* fn = files[i].filename;
        int L = slen(fn);
        if (L < 5 || L > 22 || !neq(fn + L - 4, ".elf", 4)) continue;

        AppEntry* e = &apps_[napps_];
        int base = L - 4;
        for (int j = 0; j < base; j++) e->label[j] = fn[j];
        e->label[base] = '\0';
        build_app_path(e->elf, sizeof(e->elf), fn);
        e->color = APP_DEFAULT;

        char man[32];  // "/apps/<base>.app"
        int m = 0;
        const char* ap = "/apps/";
        while (ap[m]) {
            man[m] = ap[m];
            m++;
        }
        for (int j = 0; j < base && m < 26; j++) man[m++] = fn[j];
        man[m++] = '.';
        man[m++] = 'a';
        man[m++] = 'p';
        man[m++] = 'p';
        man[m] = '\0';

        bool hidden = false;
        if (sys_file_exists(man)) {
            for (int j = 0; j < kManifestMax; j++) mbuf[j] = 0;
            if (sys_read_file_to_buffer(man, mbuf, sizeof(mbuf) - 1))
                parse_manifest(mbuf, e, &hidden);
        }
        if (!hidden) napps_++;
    }

    unsigned s = static_cast<unsigned>(napps_);
    for (int i = 0; i < napps_; i++) {
        unsigned rgb24 = (static_cast<unsigned>(apps_[i].color.r) << 16) |
                         (static_cast<unsigned>(apps_[i].color.g) << 8) |
                         static_cast<unsigned>(apps_[i].color.b);
        s = s * 31 + rgb24;
        for (int j = 0; apps_[i].label[j]; j++)
            s = s * 31 + static_cast<unsigned char>(apps_[i].label[j]);
    }
    bool changed = (s != checksum_);
    checksum_ = s;
    return changed;
}

int Launcher::grid_cols(int w) {
    int c = (w - ICON_X0) / CELL_W;
    return c < 1 ? 1 : c;
}

int Launcher::grid_cap(int w, int h) const {
    int rows = (h - TB_H - ICON_Y0) / CELL_H;
    if (rows < 1) rows = 1;
    int cap = rows * grid_cols(w);
    return cap > napps_ ? napps_ : cap;
}

Rect Launcher::icon_rect(int i, int cols) {
    Rect r;
    r.x = ICON_X0 + (i % cols) * CELL_W;
    r.y = ICON_Y0 + (i / cols) * CELL_H;
    r.width = ICON_SZ;
    r.height = ICON_SZ;
    return r;
}

int Launcher::find_icon(Point p, int cols, int cap) const {
    for (int i = 0; i < cap; i++) {
        Rect r = icon_rect(i, cols);
        if (r.contains(p)) return i;
    }
    return -1;
}

void Launcher::draw(Canvas& canvas) const {
    int cols = grid_cols(canvas.width());
    int cap = grid_cap(canvas.width(), canvas.height());
    for (int i = 0; i < cap; i++) {
        Rect r = icon_rect(i, cols);
        canvas.fill_rect(r, apps_[i].color);
        char lbl[LBL_MAX + 1];
        int n = slen(apps_[i].label);
        if (n > LBL_MAX) n = LBL_MAX;
        for (int j = 0; j < n; j++) lbl[j] = apps_[i].label[j];
        lbl[n] = '\0';
        Point p;
        p.x = r.x + (ICON_SZ - n * 8) / 2;
        p.y = r.y + ICON_SZ + 4;
        canvas.draw_text(lbl, p, ICON_TXT);
    }
}

}  // namespace desktop_impl
