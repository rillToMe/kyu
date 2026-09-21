// Kyuzen Desktop — implementasi cache + resolusi ikon terpusat.
#include "app_icons.hpp"

namespace desktop_impl {

namespace {

int slen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

bool streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

}  // namespace

void resolve_icon_path(const char* icon_field, char* out, int out_cap) {
    if (out_cap <= 0) return;
    if (!icon_field || !icon_field[0]) {
        int n = slen(ICON_DEFAULT_PATH);
        if (n > out_cap - 1) n = out_cap - 1;
        for (int i = 0; i < n; i++) out[i] = ICON_DEFAULT_PATH[i];
        out[n] = '\0';
        return;
    }
    // Normalisasi: "x.png" dan "/x.png" -> "/x.png" (akar FS).
    int o = 0;
    if (icon_field[0] != '/') out[o++] = '/';
    for (int i = 0; icon_field[i] && o < out_cap - 1; i++)
        out[o++] = icon_field[i];
    out[o] = '\0';
}

void scale_nearest(const uint32_t* src, int sw, int sh, uint32_t* dst, int dw,
                   int dh) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++)
            dst[y * dw + x] = src[sy * sw + (x * sw / dw)];
    }
}

void draw_px(Canvas& canvas, int x, int y, const uint32_t* px, int w, int h) {
    blit_px(canvas, x, y, px, w, h);
}

IconCache::IconCache() : tick_(0) {
    for (int i = 0; i < ICON_CACHE_N; i++) {
        slots_[i].path[0] = '\0';
        slots_[i].px = 0;
        slots_[i].used = false;
        slots_[i].age = 0;
    }
}

const IconPx* IconCache::load_into(int slot, const char* path) const {
    int dw = 0, dh = 0;
    uint32_t* raw = png_decode(path, &dw, &dh);
    if (!raw) return 0;
    uint32_t* small = new uint32_t[ICON_CACHE_PX * ICON_CACHE_PX];
    if (!small) {
        png_free(raw);
        return 0;
    }
    scale_nearest(raw, dw, dh, small, ICON_CACHE_PX, ICON_CACHE_PX);
    png_free(raw);
    delete[] slots_[slot].px;
    slots_[slot].px = small;
    int n = slen(path);
    if (n > 31) n = 31;
    for (int i = 0; i < n; i++) slots_[slot].path[i] = path[i];
    slots_[slot].path[n] = '\0';
    slots_[slot].used = true;
    slots_[slot].age = ++tick_;
    static IconPx out;
    out.px = small;
    out.size = ICON_CACHE_PX;
    return &out;
}

const IconPx* IconCache::icon_for(const char* path) const {
    if (!path || !path[0]) path = ICON_DEFAULT_PATH;
    for (int i = 0; i < ICON_CACHE_N; i++) {
        if (slots_[i].used && streq(slots_[i].path, path)) {
            slots_[i].age = ++tick_;
            static IconPx hit;
            hit.px = slots_[i].px;
            hit.size = ICON_CACHE_PX;
            return &hit;
        }
    }
    // Miss: slot kosong dulu, sonst timpa yang paling lama.
    int victim = -1;
    for (int i = 0; i < ICON_CACHE_N; i++) {
        if (!slots_[i].used) {
            victim = i;
            break;
        }
    }
    if (victim < 0) {
        victim = 0;
        for (int i = 1; i < ICON_CACHE_N; i++)
            if (slots_[i].age < slots_[victim].age) victim = i;
    }
    const IconPx* got = load_into(victim, path);
    if (got) return got;
    // Fallback terpusat: kustom gagal -> default.png. Default gagal -> null
    // (pemanggil menggambar kotak warna). Tidak ada duplikasi di modul lain.
    if (!streq(path, ICON_DEFAULT_PATH)) return icon_for(ICON_DEFAULT_PATH);
    return 0;
}

}  // namespace desktop_impl
