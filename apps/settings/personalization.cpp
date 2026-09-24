// apps/settings/personalization.cpp — implementasi halaman Personalization.
#include <cstdint>

#include "personalization.hpp"

namespace settings {

namespace {

// Nama builtin = WALL_BUILTINS system/desktop/theme.hpp (sinkron manual).
const char* const kWallpapers[PersonalizationPage::kWallpaperCount] = {
    "island.png",      "black-hole.png", "city-lanscaps.png",
    "city-town.png",   "kimi-no-nawa.png", "meadow.png",
};
const char kDefaultWallpaper[] = "island.png";
// Override persisten pilihan Settings. BUKAN /apps/desktop.app: berkas itu
// ditulis ulang kernel dari modul ISO setiap boot (kernel.c auto-install),
// jadi pilihan di sana hilang saat reboot. Berkas root non-modul selamat
// (preseden settings.ui/font.ui). Desktop membaca berkas ini dulu, manifest
// hanya fallback/default instalasi.
const char kWallpaperPath[] = "/wallpaper.ui";
const char kManifestPath[] = "/apps/desktop.app";
constexpr int kManifestMax = 512;
// Kotak preview tetap: Image::set_file() me-reset w/h widget ke ukuran
// natural PNG (untuk ScrollView pan imageview) — paksa kembali ke sini tiap
// ganti berkas agar layout tak membalon.
constexpr int kPreviewW = 320;
constexpr int kPreviewH = 180;

bool streq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == '\0' && b[i] == '\0';
}

void onWallpaper(void* ud) {
    PersonalizationPage::WallpaperBinding* b =
        static_cast<PersonalizationPage::WallpaperBinding*>(ud);
    if (b && b->page) b->page->select(b->index);
}

void onApply(void* ud) {
    PersonalizationPage* p = static_cast<PersonalizationPage*>(ud);
    if (p) p->apply();
}

// Path "/<nama>" untuk ui_image (berkas di akar FS via modul limine).
void imagePath(const char* name, char* out, int cap) {
    if (cap <= 0) return;
    int o = 0;
    if (name && name[0] != '/') out[o++] = '/';
    for (int i = 0; name && name[i] && o < cap - 1; i++) out[o++] = name[i];
    out[o] = '\0';
}

}  // namespace

const char* PersonalizationPage::builtinName(int index) {
    if (index < 0 || index >= kWallpaperCount) return kDefaultWallpaper;
    return kWallpapers[index];
}

int PersonalizationPage::builtinIndex(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < kWallpaperCount; i++)
        if (streq(name, kWallpapers[i])) return i;
    return -1;
}

// Baca pilihan tersimpan: /wallpaper.ui dulu, lalu kunci "wallpaper="
// manifest. Absen/rusak -> default (bukan crash).
void PersonalizationPage::readSaved(char* out, int cap) {
    if (cap > 0) out[0] = '\0';
    if (cap <= 1) return;
    if (sys_file_exists(const_cast<char*>(kWallpaperPath))) {
        char wbuf[32];
        for (int i = 0; i < 32; i++) wbuf[i] = '\0';
        if (sys_read_file_to_buffer(const_cast<char*>(kWallpaperPath), wbuf,
                                    sizeof(wbuf) - 1)) {
            int n = 0;
            while (wbuf[n] && wbuf[n] != '\n' && wbuf[n] != '\r' && n < cap - 1)
                n++;
            wbuf[n] = '\0';
            if (builtinIndex(wbuf) >= 0) {
                for (int j = 0; j <= n && j < cap; j++) out[j] = wbuf[j];
                return;
            }
        }
    }
    char mbuf[kManifestMax];
    for (int i = 0; i < kManifestMax; i++) mbuf[i] = '\0';
    if (!sys_file_exists(const_cast<char*>(kManifestPath))) {
        for (int i = 0; kDefaultWallpaper[i] && i < cap - 1; i++)
            out[i] = kDefaultWallpaper[i];
        return;
    }
    if (!sys_read_file_to_buffer(const_cast<char*>(kManifestPath), mbuf,
                                 kManifestMax - 1))
        return;
    int i = 0;
    while (mbuf[i]) {
        int ls = i;
        while (mbuf[i] && mbuf[i] != '\n') i++;
        int le = i;
        if (mbuf[i]) i++;
        int eq = ls;
        while (eq < le && mbuf[eq] != '=') eq++;
        if (eq >= le) continue;
        bool is_wall = (eq - ls == 9);
        const char* want = "wallpaper";
        for (int j = 0; j < 9 && is_wall; j++)
            if (mbuf[ls + j] != want[j]) is_wall = false;
        if (!is_wall) continue;
        int vlen = le - (eq + 1);
        while (vlen > 0 && (mbuf[eq + 1 + vlen - 1] == '\r' ||
                            mbuf[eq + 1 + vlen - 1] == ' '))
            vlen--;
        int n = vlen > cap - 1 ? cap - 1 : vlen;
        for (int j = 0; j < n; j++) out[j] = mbuf[eq + 1 + j];
        out[n] = '\0';
        if (builtinIndex(out) < 0) out[0] = '\0';
        if (!out[0]) {
            for (int j = 0; kDefaultWallpaper[j] && j < cap - 1; j++)
                out[j] = kDefaultWallpaper[j];
        }
        return;
    }
}

// Simpan pilihan ke /wallpaper.ui (isi = nama + newline). Berkas kecil ini
// dibaca desktop saat start maupun tiap poll rescan (live reload ≤5 dtk).
// create tidak overwrite -> hapus dulu (pola kernel/settings lama).
bool PersonalizationPage::writeSaved(const char* name) {
    if (!name || !name[0] || builtinIndex(name) < 0) return false;
    char out[32];
    int o = 0;
    for (int j = 0; name[j] && o < 30; j++) out[o++] = name[j];
    out[o++] = '\n';
    out[o] = '\0';
    fs_delete(const_cast<char*>(kWallpaperPath));
    if (!sys_create_file(const_cast<char*>(kWallpaperPath), out,
                         (std::uint32_t)o))
        return false;
    return true;
}

void PersonalizationPage::build(ui_window_t* w) {
    win = w;
    ui_widget_t* box = ui_vbox_create(w, 6);
    ui_layout_add(box, ui_label_create(w, "Personalization"));
    ui_layout_add(box, ui_label_create(w, "Wallpaper"));

    // Preview: ui_image_* existing (PNG akar FS, skala nearest ke rect).
    // Gagal decode -> kotak kosong (bukan crash) per kontrak toolkit.
    char saved[32];
    readSaved(saved, sizeof(saved));
    selected = builtinName(builtinIndex(saved));
    char path[40];
    imagePath(selected, path, sizeof(path));
    preview = ui_image_create(w, path, kPreviewW, kPreviewH);    ui_layout_add(box, preview);

    ui_layout_add(box, ui_label_create(w, "Choose your wallpaper"));
    ui_layout_add(box, ui_label_create(w, "Keys: N = next, A = apply"));
    for (int i = 0; i < kWallpaperCount; i++) {
        bindings[i].page = this;
        bindings[i].index = i;
        ui_widget_t* b = ui_button_create(w, kWallpapers[i]);
        ui_button_set_click(b, onWallpaper, &bindings[i]);
        ui_layout_add(box, b);
    }

    ui_widget_t* apply = ui_button_create(w, "Apply");
    ui_button_set_click(apply, onApply, this);
    ui_layout_add(box, apply);

    status = ui_label_create(w, "Wallpaper: (from desktop.app)");
    ui_layout_add(box, status);
    root = box;
}

void PersonalizationPage::select(int index) {
    if (index < 0 || index >= kWallpaperCount) return;
    selected = kWallpapers[index];
    char path[40];
    imagePath(selected, path, sizeof(path));
    ui_image_set_file(preview, path);
    ui_widget_set_size(preview, kPreviewW, kPreviewH);
    char msg[64];
    int k = 0;
    const char* p = "Preview: ";
    while (p[k]) { msg[k] = p[k]; k++; }
    for (int i = 0; selected[i] && k < 62; i++) msg[k++] = selected[i];
    msg[k] = '\0';
    ui_label_set_text(status, msg);
}

void PersonalizationPage::apply() {
    if (!selected) return;
    if (!writeSaved(selected)) {
        ui_label_set_text(status, "Wallpaper unavailable, using default");
        return;
    }
    // Persist DULU (sumber kebenaran), lalu minta Desktop reload seketika
    // via sys_hot_reload(WALLPAPER) generik (syscall 85). Desktop membaca
    // konfigurasi yang sama saat reboot.
    if (sys_hot_reload(KZ_HOT_RELOAD_WALLPAPER, 0) == 0) {
        char msg[64];
        int k = 0;
        const char* p = "Wallpaper applied: ";
        while (p[k]) { msg[k] = p[k]; k++; }
        for (int i = 0; selected[i] && k < 62; i++) msg[k++] = selected[i];
        msg[k] = '\0';
        ui_label_set_text(status, msg);
    } else {
        ui_label_set_text(status, "Wallpaper saved, but Desktop unavailable");
    }
}

void PersonalizationPage::cycleNext() {
    int cur = builtinIndex(selected);
    select((cur < 0 ? 0 : (cur + 1) % kWallpaperCount));
}

}  // namespace settings
