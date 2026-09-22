// Kyuzen Desktop — implementasi wallpaper.
#include <new>  // std::nothrow (heap dinamis libc port, Phase 9.5)

#include "wallpaper.hpp"
#include "app_icons.hpp"
#include "sys_abi.hpp"

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

// Ambil nilai kunci "wallpaper=" dari isi manifest (pola parse_manifest
// launcher, khusus satu kunci). out selalu NUL-terminated (boleh "").
void parse_wallpaper_key(const char* b, char* out, int cap) {
    if (cap > 0) out[0] = '\0';
    if (!b || cap <= 1) return;
    int i = 0;
    while (b[i]) {
        int ls = i;
        while (b[i] && b[i] != '\n') i++;
        int le = i;
        if (b[i]) i++;
        int eq = ls;
        while (eq < le && b[eq] != '=') eq++;
        if (eq >= le) continue;
        if (eq - ls == 9 && neq(&b[ls], "wallpaper", 9)) {
            const char* v = &b[eq + 1];
            int vlen = le - (eq + 1);
            while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == ' '))
                vlen--;
            int n = vlen > cap - 1 ? cap - 1 : vlen;
            for (int j = 0; j < n; j++) out[j] = v[j];
            out[n] = '\0';
            return;
        }
    }
}

}  // namespace

Wallpaper::Wallpaper() : px_(0), w_(0), h_(0) {}

int Wallpaper::pick_builtin(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < WALL_BUILTIN_N; i++) {
        const char* b = WALL_BUILTINS[i];
        int j = 0;
        while (name[j] && name[j] == b[j]) j++;
        if (!name[j] && !b[j]) return i;
    }
    return -1;
}

void Wallpaper::config_path(char* out, int cap, const char* name) {
    if (cap <= 0) return;
    int o = 0;
    if (name && name[0] != '/') out[o++] = '/';
    for (int i = 0; name && name[i] && o < cap - 1; i++)
        out[o++] = name[i];
    out[o] = '\0';
}

bool Wallpaper::load(int w, int h) {
    if (px_) {
        delete[] px_;
        px_ = 0;
    }
    w_ = h_ = 0;
    if (w <= 0 || h <= 0) return false;

    // 1. Pilihan dari manifest desktop sendiri.
    char sel[32];
    sel[0] = '\0';
    if (sys_file_exists(const_cast<char*>("/apps/desktop.app"))) {
        char mbuf[512];
        for (int j = 0; j < 512; j++) mbuf[j] = 0;
        if (sys_read_file_to_buffer(const_cast<char*>("/apps/desktop.app"),
                                    mbuf, sizeof(mbuf) - 1))
            parse_wallpaper_key(mbuf, sel, sizeof(sel));
    }
    // 2. Rantai fallback: pilihan -> bawaan.
    int idx = pick_builtin(sel);
    if (idx < 0) idx = pick_builtin(WALL_DEFAULT);

    for (int t = 0; t < 2 && idx >= 0; t++) {
        char path[32];
        config_path(path, sizeof(path), WALL_BUILTINS[idx]);
        int iw = 0, ih = 0;
        uint32_t* raw = png_decode(path, &iw, &ih);
        if (raw) {
            // Buffer layar (8MB @1080p) dari ALLOCATOR STANDAR — inilah jalur
            // yang dulu meledak: arena malloc llvm-libc hanya 1 MiB sehingga
            // `new` 8MB = NULL -> operator new[] -> abort() -> ud2/BSOD INT 6.
            // Phase 9.5 membuat heap tumbuh on-demand (libs/c/libc-port/src/
            // kyuzen_heap.hpp), jadi jalur normal ini dipakai lagi dan sekaligus
            // jadi bukti runtime bahwa operator new[] 8.3 MiB berhasil.
            // Varian nothrow dipilih agar kegagalan = NULL -> fallback gradasi,
            // bukan abort (app ini dikompilasi -fno-exceptions).
            uint32_t npx = static_cast<uint32_t>(w) *
                           static_cast<uint32_t>(h);
            uint32_t* scr = new (std::nothrow) uint32_t[npx];
            if (scr) {
                scale_nearest(raw, iw, ih, scr, w, h);
                px_ = scr;
                w_ = w;
                h_ = h;
            }
            png_free(raw);
            if (px_) return true;
        }
        // Pilihan gagal -> coba bawaan (sekali).
        idx = (t == 0) ? pick_builtin(WALL_DEFAULT) : -1;
        if (t == 0 && pick_builtin(sel) == pick_builtin(WALL_DEFAULT))
            break;  // pilihan == bawaan: tak perlu coba dua kali
    }
    return false;
}

void Wallpaper::draw_bg(Canvas& canvas, Rect region, int h_tb) const {
    if (px_) {
        draw_photo_into(canvas, region);
        return;
    }
    draw_gradient(canvas, region, h_tb);
}

void Wallpaper::draw_gradient(Canvas& canvas, Rect region, int h_tb) const {
    int W = canvas.width();
    int H = canvas.height() - h_tb;
    if (W <= 0 || H <= 0) return;
    int x0 = region.x < 0 ? 0 : region.x;
    int y0 = region.y < 0 ? 0 : region.y;
    int x1 = region.x + region.width;
    if (x1 > W) x1 = W;
    int y1 = region.y + region.height;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;

    // Interpolasi vertikal integer WALL_BG -> WALL_BG2 per strip. Warna
    // dihitung dari posisi ABSOLUT (awal strip), sehingga render parsial
    // (restore area kartu preview) menghasilkan warna yang sama.
    int band = y0 - (y0 % WALL_BAND_H);
    for (int y = band; y < y1; y += WALL_BAND_H) {
        int by = y < y0 ? y0 : y;
        int bh = y + WALL_BAND_H - by;
        if (by + bh > y1) bh = y1 - by;
        if (bh <= 0) continue;
        int t = H <= 1 ? 0 : (y * 255) / (H - 1);
        uint8_t r = static_cast<uint8_t>(
            (WALL_BG.r * (255 - t) + WALL_BG2.r * t) / 255);
        uint8_t g = static_cast<uint8_t>(
            (WALL_BG.g * (255 - t) + WALL_BG2.g * t) / 255);
        uint8_t b = static_cast<uint8_t>(
            (WALL_BG.b * (255 - t) + WALL_BG2.b * t) / 255);
        Rect r_band;
        r_band.x = x0;
        r_band.y = by;
        r_band.width = x1 - x0;
        r_band.height = bh;
        canvas.fill_rect(r_band, rgb(r, g, b));
    }

    const char* brand = "KyuzenOS";
    int n = slen(brand);
    int bx = W - n * 8 - 16;
    // Hanya bila region mencakup seluruh kotak merek (render parsial tak
    // boleh menulis di luar areanya).
    if (x0 <= bx && x1 >= bx + n * 8 && y0 <= 12 && y1 >= 28) {
        Point p;
        p.x = bx;
        p.y = 12;
        canvas.draw_text(brand, p, WALL_TXT);
    }
}

}  // namespace desktop_impl
