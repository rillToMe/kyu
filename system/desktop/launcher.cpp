// Kyuzen Desktop — implementasi launcher (port desktop generasi C).
#include "launcher.hpp"
#include "sys_abi.hpp"
#include "theme.hpp"
// Font UI (libs/text, bukan third_party — lolos isolasi desktop):
// registry + format config + renderer. Backend FT di-link dari arsip
// freestanding; tanpa KZFONT_USE_FREETYPE, kzraster = stub MISS dan
// draw() jatuh ke jalur bitmap lama (pixel-identical).
#include "kzfont.h"
#include "kzfonts.h"
#include "kzfontcfg.h"

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

// Manifest "<base>.app": satu "key=value" per baris (name/color/hidden/icon).
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
        } else if (klen == 4 && neq(k, "icon", 4)) {
            int n = vlen > ICON_NAME_MAX - 1 ? ICON_NAME_MAX - 1 : vlen;
            for (int j = 0; j < n; j++) e->icon[j] = v[j];
            e->icon[n] = '\0';
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

Launcher::Launcher() : napps_(0), checksum_(0), hover_(-1), selected_(-1),
                     auto_arrange_(true) {
    ui_font_id_ = (int)KZ_FONT_DEFAULT;
    ui_font_data_ = 0;
    ui_font_size_ = 0;
    ui_font_ = 0;
    for (int i = 0; i < MAX_APPS; i++) {
        apps_[i].label[0] = '\0';
        apps_[i].elf[0] = '\0';
        apps_[i].icon[0] = '\0';
        apps_[i].color = APP_DEFAULT;
        custom_[i] = false;
        pos_x_[i] = 0;
        pos_y_[i] = 0;
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
        e->icon[0] = '\0';

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
        for (int j = 0; apps_[i].icon[j]; j++)
            s = s * 31 + static_cast<unsigned char>(apps_[i].icon[j]);
    }
    bool changed = (s != checksum_);
    checksum_ = s;
    if (changed) {
        // Daftar bergeser: seleksi/posisi kustom berbasis indeks basi.
        hover_ = -1;
        selected_ = -1;
        for (int i = 0; i < MAX_APPS; i++) custom_[i] = false;
    } else {
        if (hover_ >= napps_) hover_ = -1;
        if (selected_ >= napps_) selected_ = -1;
    }
    return changed;
}

// --- UI font (FreeType, §3-§11) ---
// Heap libs/text di atas sys_alloc (allocator Kyuzen existing).
namespace {

void* uif_alloc(uint32_t n) { return sys_alloc(n ? n : 1); }
void uif_free(void* p) { sys_free(p); }
void* uif_realloc(void* p, uint32_t o, uint32_t n) {
    return sys_realloc(p, o, n);
}

}  // namespace

void Launcher::ui_font_free() {
    if (ui_font_) {
        kz_font_destroy(ui_font_);
        ui_font_ = 0;
    }
    if (ui_font_data_) {
        sys_free(ui_font_data_);
        ui_font_data_ = 0;
        ui_font_size_ = 0;
    }
}

// Baca font.ui (5 byte) -> id valid. Absen/rusak -> default (§9/§11).
// Semantik syscall: read mengembalikan 1/0 (bukan byte) — ukuran
// dipastikan via sys_file_size dulu.
int Launcher::ui_font_read_id() {
    if (sys_file_size((char*)KZ_FONT_CFG_PATH) != KZ_FONT_CFG_LEN)
        return (int)KZ_FONT_DEFAULT;
    char buf[KZ_FONT_CFG_LEN];
    for (int i = 0; i < KZ_FONT_CFG_LEN; i++) buf[i] = 0;
    if (sys_read_file_to_buffer((char*)KZ_FONT_CFG_PATH, buf,
                                KZ_FONT_CFG_LEN) != 1)
        return (int)KZ_FONT_DEFAULT;
    return kz_font_cfg_decode(buf, KZ_FONT_CFG_LEN);
}

void Launcher::ui_font_load(int id) {
    ui_font_free();
    id = kz_font_id_sanitize(id);
    ui_font_id_ = id;
    const char* path = KZ_FONT_FILES[id];
    uint32_t sz = sys_file_size((char*)path);
    if (sz < 1000 || sz > 8u * 1024u * 1024u) {
        // Gagal sunyi = label fallback; catat ukuran via serial agar
        // bisa didiagnosis (file hilang vs rusak).
        print(const_cast<char*>("[desktop] uifont gagal size "));
        print_num(sz);
        print(const_cast<char*>("\n"));
        return;  // fallback bitmap
    }
    char* buf = (char*)sys_alloc(sz);
    print(const_cast<char*>("[desktop] uifont alloc "));
    print_num(buf ? sz : 0);
    print(const_cast<char*>("\n"));
    if (!buf) return;
    if (sys_read_file_to_buffer((char*)path, buf, sz) != 1) {
        print(const_cast<char*>("[desktop] uifont gagal read\n"));
        sys_free(buf);
        return;
    }
    kz_heap_t heap;
    heap.alloc = uif_alloc;
    heap.free = uif_free;
    heap.realloc = uif_realloc;
    kz_font_blob_t blob;
    blob.data = (const uint8_t*)buf;
    blob.size = sz;
    kz_font_t* f = kz_font_load(&blob, &heap, &kz_ft_backend);
    if (!f || kz_font_set_size(f, LABEL_FONT_PX) != 0) {
        print(const_cast<char*>("[desktop] uifont gagal face\n"));
        if (f) kz_font_destroy(f);
        sys_free(buf);
        return;
    }
    ui_font_data_ = buf;
    ui_font_size_ = sz;
    ui_font_ = (struct kz_font*)f;
    // Satu print() (buffer lokal) agar baris marker tidak terbelah oleh
    // task lain yang menulis serial bersamaan (prompt shell vs desktop).
    char msg[48];
    const char* pre = "[desktop] uifont ";
    const char* nm = KZ_FONT_NAMES[id];
    int k = 0;
    while (pre[k]) {
        msg[k] = pre[k];
        k++;
    }
    for (int i = 0; nm[i] && k < 44; i++) msg[k++] = nm[i];
    msg[k++] = '\n';
    msg[k] = '\0';
    print(msg);
}

void Launcher::ui_font_init() { ui_font_load(ui_font_read_id()); }

bool Launcher::ui_font_poll() {
    int id = ui_font_read_id();
    if (id == ui_font_id_ && ui_font_) return false;
    if (id == ui_font_id_ && !ui_font_) {
        // File hilang setelah pernah gagal: coba lagi (FS mungkin telat).
        ui_font_load(id);
        return ui_font_ != 0;
    }
    ui_font_load(id);
    return true;  // caller: Damage::Full + redraw
}

int Launcher::grid_cols(int w) {
    int c = (w - ICON_X0) / CELL_W;
    return c < 1 ? 1 : c;
}

int Launcher::grid_rows(int h) {
    int r = (h - TB_H - ICON_Y0) / CELL_H;
    return r < 1 ? 1 : r;
}

int Launcher::grid_cap(int w, int h) const {
    int cap = grid_rows(h) * grid_cols(w);
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

// Kolom-mayor: isi ke bawah dulu (row cepat), lalu kolom baru di kanan.
void Launcher::box_grid_pos(int i, int cols, int rows, int* col, int* row) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    int c = i / rows;
    int r = i % rows;
    if (c >= cols) {  // luapan: jepit ke sel terakhir yang ada
        c = cols - 1;
        r = rows - 1;
    }
    if (col) *col = c;
    if (row) *row = r;
}

Rect Launcher::box_rect(int i, int cols, int rows) const {
    Rect r;
    if (!auto_arrange_ && i >= 0 && i < MAX_APPS && custom_[i]) {
        r.x = pos_x_[i];
        r.y = pos_y_[i];
        r.width = BOX_W;
        r.height = BOX_H;
        return r;
    }
    int c = 0, rr = 0;
    box_grid_pos(i, cols, rows, &c, &rr);
    r.x = ICON_X0 + c * CELL_W;
    r.y = ICON_Y0 + rr * CELL_H;
    r.width = BOX_W;
    r.height = BOX_H;
    return r;
}

void Launcher::describe(int i, int cols, int rows, DesktopIcon* out) const {
    if (!out) return;
    out->box = box_rect(i, cols, rows);
    box_grid_pos(i, cols, rows, &out->col, &out->row);
    out->state = ICON_ST_NORMAL;
    if (i == selected_)
        out->state = ICON_ST_SELECTED;
    else if (i == hover_)
        out->state = ICON_ST_HOVER;
}

int Launcher::find_icon(Point p, int cols, int cap) const {
    // Kompat lama: urutan baris-mayor, tapi hit memakai box penuh
    // (klik label ikut kena, bukan cuma kotak 48px).
    if (cols < 1) cols = 1;
    for (int i = 0; i < cap; i++) {
        Rect r;
        r.x = ICON_X0 + (i % cols) * CELL_W;
        r.y = ICON_Y0 + (i / cols) * CELL_H;
        r.width = BOX_W;
        r.height = BOX_H;
        if (r.contains(p)) return i;
    }
    return -1;
}

int Launcher::find_icon_at(Point p, int w, int h) const {
    int cols = grid_cols(w);
    int rows = grid_rows(h);
    int cap = grid_cap(w, h);
    for (int i = 0; i < cap; i++) {
        if (box_rect(i, cols, rows).contains(p)) return i;
    }
    return -1;
}

int Launcher::wrap_label(const char* src, char* l1, char* l2, int cap) {
    const int W = (cap - 1 < BOX_LBL_MAX) ? cap - 1 : BOX_LBL_MAX;
    if (cap <= 0) return 1;
    if (W <= 0 || !src || !l1 || !l2) {
        if (l1 && cap > 0) l1[0] = '\0';
        if (l2 && cap > 0) l2[0] = '\0';
        return 1;
    }
    char b1[BOX_LBL_MAX + 1], b2[BOX_LBL_MAX + 1];
    b1[0] = '\0';
    b2[0] = '\0';
    char* cur = b1;
    int curlen = 0;
    int line = 0;  // 0 = baris1, 1 = baris2 (terakhir)
    bool overflow = false;  // masih ada sisa setelah baris2 -> "..."
    int i = 0;
    while (src[i]) {
        while (src[i] == ' ') i++;  // spasi ganda = satu pemisah
        if (!src[i]) break;
        int ws = i;
        while (src[i] && src[i] != ' ') i++;
        int L = i - ws;
        if (L > W) {
            // Kata lebih panjang dari baris: potong keras per karakter.
            if (curlen > 0) {
                if (line == 0) {
                    line = 1;
                    cur = b2;
                    curlen = 0;
                } else {
                    overflow = true;
                    break;
                }
            }
            int off = 0;
            while (off < L) {
                int take = W - curlen;
                if (take <= 0) {
                    if (line == 0) {
                        line = 1;
                        cur = b2;
                        curlen = 0;
                        take = W;
                    } else {
                        overflow = true;
                        break;
                    }
                }
                if (L - off > take && line == 0) {
                    // Baris1 penuh, sisa lanjut ke baris2.
                    for (int j = 0; j < take; j++)
                        cur[curlen++] = src[ws + off + j];
                    off += take;
                    cur[curlen] = '\0';
                    line = 1;
                    cur = b2;
                    curlen = 0;
                    continue;
                }
                if (L - off > take) {  // baris2 tak muat -> elipsis
                    for (int j = 0; j < take; j++)
                        cur[curlen++] = src[ws + off + j];
                    off += take;
                    overflow = true;
                    break;
                }
                for (int j = 0; j < L - off; j++) cur[curlen++] = src[ws + off + j];
                off = L;
                cur[curlen] = '\0';
            }
            if (overflow) break;
            continue;
        }
        int need = L + (curlen > 0 ? 1 : 0);
        if (curlen + need <= W) {
            if (curlen > 0) cur[curlen++] = ' ';
            for (int j = 0; j < L; j++) cur[curlen++] = src[ws + j];
            cur[curlen] = '\0';
        } else if (line == 0) {
            line = 1;
            cur = b2;
            curlen = 0;
            for (int j = 0; j < L; j++) cur[curlen++] = src[ws + j];
            cur[curlen] = '\0';
        } else {
            overflow = true;  // baris2 penuh -> elipsis
            break;
        }
    }
    if (overflow) {
        // "..." HANYA di sini: menempel bila muat, sonst potong 7 + "...".
        int bl = 0;
        while (b2[bl]) bl++;
        if (bl + 3 <= W) {
            b2[bl++] = '.';
            b2[bl++] = '.';
            b2[bl++] = '.';
            b2[bl] = '\0';
        } else if (W >= 4) {
            b2[W - 3] = '.';
            b2[W - 2] = '.';
            b2[W - 1] = '.';
            b2[W] = '\0';
        }
    }
    int m = 0;
    while (b1[m] && m < cap - 1) {
        l1[m] = b1[m];
        m++;
    }
    l1[m] = '\0';
    m = 0;
    while (b2[m] && m < cap - 1) {
        l2[m] = b2[m];
        m++;
    }
    l2[m] = '\0';
    return (line == 0 && !overflow) ? 1 : 2;
}

void Launcher::set_auto_arrange(bool on) {
    auto_arrange_ = on;
    if (on) clear_custom();
}

void Launcher::set_custom_pos(int i, int x, int y) {
    if (i < 0 || i >= MAX_APPS) return;
    custom_[i] = true;
    pos_x_[i] = x;
    pos_y_[i] = y;
}

void Launcher::clear_custom() {
    for (int i = 0; i < MAX_APPS; i++) custom_[i] = false;
}

void Launcher::sort_by_name() {
    // Bubble: napps_ <= 32, tanpa alokasi, stabil untuk equal.
    for (int i = 0; i < napps_; i++) {
        for (int j = i + 1; j < napps_; j++) {
            const char* a = apps_[i].label;
            const char* b = apps_[j].label;
            int k = 0;
            while (a[k] && a[k] == b[k]) k++;
            if ((unsigned char)a[k] > (unsigned char)b[k]) {
                AppEntry t = apps_[i];
                apps_[i] = apps_[j];
                apps_[j] = t;
                bool cb = custom_[i];
                custom_[i] = custom_[j];
                custom_[j] = cb;
                int tx = pos_x_[i];
                pos_x_[i] = pos_x_[j];
                pos_x_[j] = tx;
                int ty = pos_y_[i];
                pos_y_[i] = pos_y_[j];
                pos_y_[j] = ty;
            }
        }
    }
    hover_ = -1;
    selected_ = -1;
}

void Launcher::icon_path(int i, char* out) const {
    resolve_icon_path(apps_[i].icon, out, 32);
}

// Basename "/apps/<nama>.elf" == title? (cocok judul window taskbar bila
// manifest name beda dari judul window).
static bool elf_base_eq(const char* elf, const char* title) {
    const char* b = elf;
    for (int i = 0; elf[i]; i++)
        if (elf[i] == '/') b = &elf[i + 1];
    int i = 0;
    while (b[i] && title[i] && b[i] == title[i]) i++;
    if (title[i]) return false;
    // Sisa basename harus ".elf" persis.
    return b[i] == '.' && b[i + 1] == 'e' && b[i + 2] == 'l' &&
           b[i + 3] == 'f' && b[i + 4] == '\0';
}

const AppEntry* Launcher::find_by_title(const char* title) const {
    if (!title || !title[0]) return 0;
    for (int i = 0; i < napps_; i++) {
        int j = 0;
        while (apps_[i].label[j] && apps_[i].label[j] == title[j]) j++;
        if (apps_[i].label[j] == '\0' && title[j] == '\0') return &apps_[i];
    }
    for (int i = 0; i < napps_; i++)
        if (elf_base_eq(apps_[i].elf, title)) return &apps_[i];
    return 0;
}

namespace {

// Outline putus-putus 1px (segmen 4 on / 2 off) untuk box selected.
void draw_dotted_edge(Canvas& canvas, const Rect& b, Color c) {
    for (int x = b.x; x < b.x + b.width; x += 6) {
        int seg = b.x + b.width - x;
        if (seg > 4) seg = 4;
        Rect t;
        t.x = x;
        t.y = b.y;
        t.width = seg;
        t.height = 1;
        canvas.fill_rect(t, c);
        Rect bo;
        bo.x = x;
        bo.y = b.y + b.height - 1;
        bo.width = seg;
        bo.height = 1;
        canvas.fill_rect(bo, c);
    }
    for (int y = b.y; y < b.y + b.height; y += 6) {
        int seg = b.y + b.height - y;
        if (seg > 4) seg = 4;
        Rect l;
        l.x = b.x;
        l.y = y;
        l.width = 1;
        l.height = seg;
        canvas.fill_rect(l, c);
        Rect r;
        r.x = b.x + b.width - 1;
        r.y = y;
        r.width = 1;
        r.height = seg;
        canvas.fill_rect(r, c);
    }
}

}  // namespace

int Launcher::draw(Canvas& canvas, const IconCache& icons, int w, int h) const {
    int cols = grid_cols(w);
    int rows = grid_rows(h);
    int cap = grid_cap(w, h);
    int nimg = 0;
    for (int i = 0; i < cap; i++) {
        DesktopIcon di;
        describe(i, cols, rows, &di);
        const Rect& b = di.box;
        if (di.state == ICON_ST_SELECTED)
            canvas.fill_rect(b, ICON_SEL);
        else if (di.state == ICON_ST_HOVER)
            canvas.fill_rect(b, ICON_HOVER);
        // Ikon 48px center-x di atas box (fallback = kotak warna).
        int ix = b.x + (BOX_W - ICON_SZ) / 2;
        int iy = b.y + BOX_ICON_Y;
        char path[32];
        resolve_icon_path(apps_[i].icon, path, sizeof(path));
        const IconPx* ic = icons.icon_for(path);
        if (ic) {
            // Ikon ke canvas window (libgui tak punya draw-image): RLE
            // fill_rect per baris. Tanpa gambar -> kotak warna manifest.
            int dx = ix + (ICON_SZ - ic->size) / 2;
            int dy = iy + (ICON_SZ - ic->size) / 2;
            if (dx < ix) dx = ix;
            if (dy < iy) dy = iy;
            draw_px(canvas, dx, dy, ic->px, ic->size, ic->size);
            nimg++;
        } else {
            Rect ir;
            ir.x = ix;
            ir.y = iy;
            ir.width = ICON_SZ;
            ir.height = ICON_SZ;
            canvas.fill_rect(ir, apps_[i].color);
        }
        // Label maks 2 baris, center-x box; "..." cuma bila baris2 luber.
        char l1[BOX_LBL_MAX + 1], l2[BOX_LBL_MAX + 1];
        int nlines = wrap_label(apps_[i].label, l1, l2, sizeof(l1));
        int n1 = slen(l1);
        if (!draw_ft_label(canvas, b, l1, BOX_LBL_DY)) {
            // Fallback bitmap lama: font tak termuat atau measure kosong
            // (mis. backend stub di host test). Tepat muat box (10x8=80).
            Point p;
            p.x = b.x + (BOX_W - n1 * 8) / 2;
            p.y = b.y + BOX_LBL_DY;
            canvas.draw_text(l1, p, ICON_TXT);
        }
        if (nlines > 1 && l2[0]) {
            int n2 = slen(l2);
            if (!draw_ft_label(canvas, b, l2, BOX_LBL_DY + BOX_LBL_LINE_H)) {
                Point p;
                p.x = b.x + (BOX_W - n2 * 8) / 2;
                p.y = b.y + BOX_LBL_DY + BOX_LBL_LINE_H;
                canvas.draw_text(l2, p, ICON_TXT);
            }
        }
        if (di.state == ICON_ST_SELECTED) draw_dotted_edge(canvas, b, ICON_SEL_EDGE);
    }
    return nimg;
}

// Satu baris label via FreeType + shadow halus (§13-§18).
// Return false -> caller pakai jalur bitmap lama.
//   - lebar ukur (advance) untuk centering; shadow TAK ikut layout.
//   - shadow: offset (1,1), hitam alpha ~90 — subtle, bukan outline.
//   - damage: bbox teks ∪ shadow lewat draw_raw (existing API).
namespace {

// Parameter satu pass gambar (foreground/shadow) untuk draw_raw.
struct FtLine {
    kz_font_t* font;
    const char* text;
    color_t color;
    int x;
    int baseline;
};

void ft_line_cb(void* ud, uint32_t* px, int cw, int ch, int dmg[4]) {
    FtLine* L = static_cast<FtLine*>(ud);
    if (!L || !L->font || !L->text) {
        if (dmg) {
            dmg[0] = dmg[1] = dmg[2] = dmg[3] = 0;
        }
        return;
    }
    kz_text_draw(px, static_cast<uint32_t>(cw), static_cast<uint32_t>(ch),
                 L->font, L->x, L->baseline, L->color, L->text, dmg);
}

}  // namespace

bool Launcher::draw_ft_label(Canvas& canvas, const Rect& b,
                             const char* line, int dy) const {
    if (!ui_font_) return false;
    // Font proporsional bisa lebih lebar dari potongan char: susutkan
    // sampai muat box (BOX_W - 8), minimal 1 char + "...".
    char fit[BOX_LBL_MAX + 1];
    int n = 0;
    while (line[n] && n < BOX_LBL_MAX) {
        fit[n] = line[n];
        n++;
    }
    fit[n] = '\0';
    uint32_t tw = 0, th = 0;
    if (kz_text_measure(ui_font_, fit, &tw, &th) != 0 || tw == 0)
        return false;
    while (tw > (uint32_t)(BOX_W - 8) && n > 4) {
        n--;
        fit[n - 3] = '.';
        fit[n - 2] = '.';
        fit[n - 1] = '.';
        fit[n] = '\0';
        if (kz_text_measure(ui_font_, fit, &tw, &th) != 0 || tw == 0)
            return false;
    }
    int tx = b.x + (BOX_W - static_cast<int>(tw)) / 2;
    if (tx < b.x + 4) tx = b.x + 4;  // jepit: tak boleh meluber ke box sebelah
    int base = b.y + dy + LABEL_FONT_PX + 3;  // baseline: teks di tengah
                                              // area baris 18px
    color_t fg = { ICON_TXT.r, ICON_TXT.g, ICON_TXT.b, 255 };
    color_t sh = { 0, 0, 0, 90 };
    FtLine fl;
    fl.font = ui_font_;
    fl.text = fit;
    fl.x = tx + 1;  // pass 1: shadow
    fl.baseline = base + 1;
    fl.color = sh;
    canvas.draw_raw(ft_line_cb, &fl);
    fl.x = tx;  // pass 2: foreground
    fl.baseline = base;
    fl.color = fg;
    canvas.draw_raw(ft_line_cb, &fl);
    return true;
}

}  // namespace desktop_impl
