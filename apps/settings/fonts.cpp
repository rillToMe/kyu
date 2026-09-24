// apps/settings/fonts.cpp — implementasi halaman Fonts.
#include <cstdint>

#include "fonts.hpp"

namespace settings {

namespace {

// Heap kz di atas sys_alloc (allocator Kyuzen existing, pola fontdemo).
void* fontAlloc(std::uint32_t n) { return sys_alloc(n ? n : 1); }
void fontFree(void* p) { sys_free(p); }
void* fontRealloc(void* p, std::uint32_t o, std::uint32_t n) {
    return sys_realloc(p, o, n);
}

void onFont(void* ud) {
    FontsPage::FontBinding* b = static_cast<FontsPage::FontBinding*>(ud);
    if (b && b->page) b->page->select(b->id);
}

void onFontSave(void* ud) {
    FontsPage* p = static_cast<FontsPage*>(ud);
    if (p) p->save();
}

void onFontLoad(void* ud) {
    FontsPage* p = static_cast<FontsPage*>(ud);
    if (p) p->load();
}

// Callback gambar FtText: toolkit memberi canvas mentah + origin widget.
void previewDraw(void* ud, std::uint32_t* canvas, int cw, int ch, int x, int y) {
    FontsPage* p = static_cast<FontsPage*>(ud);
    kz_font_t* f = (p) ? p->fonts.font() : nullptr;
    if (!f || !canvas) return;
    color_t fg = COLOR_RGB(0xE8, 0xE8, 0xEC);
    int dmg[4];
    kz_text_draw(canvas, (std::uint32_t)cw, (std::uint32_t)ch, f, x + 4, y + 24,
                 fg, "The quick brown fox", dmg);
    kz_text_draw(canvas, (std::uint32_t)cw, (std::uint32_t)ch, f, x + 4, y + 48,
                 fg, "KyuzenOS 0123456789", dmg);
}

}  // namespace

FontSettings::FontSettings() : font_(nullptr), data_(nullptr), index_((int)KZ_FONT_DEFAULT) {}

FontSettings::~FontSettings() { freeFont(); }

void FontSettings::freeFont() {
    if (font_) {
        kz_font_destroy(font_);
        font_ = nullptr;
    }
    if (data_) {
        sys_free(data_);
        data_ = nullptr;
    }
}

bool FontSettings::load(int id) {
    freeFont();
    id = kz_font_id_sanitize(id);
    index_ = id;
    const char* path = KZ_FONT_FILES[id];
    std::uint32_t sz = sys_file_size(const_cast<char*>(path));
    if (sz < 1000 || sz > 8u * 1024u * 1024u) return false;
    char* buf = static_cast<char*>(sys_alloc(sz));
    if (!buf) return false;
    if (sys_read_file_to_buffer(const_cast<char*>(path), buf, sz) != 1) {
        sys_free(buf);
        return false;
    }
    kz_heap_t heap = {fontAlloc, fontFree, fontRealloc};
    kz_font_blob_t blob = {reinterpret_cast<const std::uint8_t*>(buf), sz};
    kz_font_t* f = kz_font_load(&blob, &heap, &kz_ft_backend);
    if (!f || kz_font_set_size(f, 20) != 0) {
        if (f) kz_font_destroy(f);
        sys_free(buf);
        return false;
    }
    data_ = buf;
    font_ = f;
    return true;
}

bool FontSettings::save() {
    char buf[KZ_FONT_CFG_LEN];
    kz_font_cfg_encode(index_, buf);
    // create tidak overwrite (kfs_create_file) -> hapus dulu (pola kernel).
    fs_delete(const_cast<char*>(KZ_FONT_CFG_PATH));
    return sys_create_file(const_cast<char*>(KZ_FONT_CFG_PATH), buf,
                           KZ_FONT_CFG_LEN) != 0;
}

bool FontSettings::loadSaved() {
    int id = (int)KZ_FONT_DEFAULT;
    if (sys_file_size(const_cast<char*>(KZ_FONT_CFG_PATH)) == KZ_FONT_CFG_LEN) {
        char buf[KZ_FONT_CFG_LEN];
        if (sys_read_file_to_buffer(const_cast<char*>(KZ_FONT_CFG_PATH), buf,
                                    KZ_FONT_CFG_LEN) == 1)
            id = kz_font_cfg_decode(buf, KZ_FONT_CFG_LEN);
    }
    return load(id);
}

const char* FontSettings::currentName() const {
    return KZ_FONT_NAMES[kz_font_id_sanitize(index_)];
}

void FontsPage::build(ui_window_t* w) {
    win = w;
    ui_widget_t* box = ui_vbox_create(w, 6);
    ui_layout_add(box, ui_label_create(w, "Fonts"));
    ui_layout_add(box, ui_label_create(w, "Interface font (1-5, S = save)"));
    for (int i = 0; i < (int)KZ_FONT_COUNT; i++) {
        bindings[i].page = this;
        bindings[i].id = i;
        ui_widget_t* b = ui_button_create(w, KZ_FONT_NAMES[i]);
        ui_button_set_click(b, onFont, &bindings[i]);
        ui_layout_add(box, b);
    }
    preview = ui_fttext_create(w, 440, 64);
    ui_fttext_set_draw(preview, previewDraw, this);
    ui_layout_add(box, preview);
    status = ui_label_create(w, "Font: ...");
    ui_layout_add(box, status);

    ui_widget_t* save = ui_button_create(w, "Save Font");
    ui_button_set_click(save, onFontSave, this);
    ui_layout_add(box, save);
    ui_widget_t* load = ui_button_create(w, "Load Font");
    ui_button_set_click(load, onFontLoad, this);
    ui_layout_add(box, load);

    root = box;
    this->load();  // pakai font tersimpan (default Inter Regular)
}

void FontsPage::select(int id) {
    fonts.load(id);
    char msg[80];
    int k = 0;
    const char* p = "Font: ";
    while (p[k]) { msg[k] = p[k]; k++; }
    const char* nm = fonts.currentName();
    for (int i = 0; nm[i] && k < 60; i++) msg[k++] = nm[i];
    if (!fonts.font()) {
        const char* f = " (load failed)";
        for (int i = 0; f[i] && k < 78; i++) msg[k++] = f[i];
    }
    msg[k] = '\0';
    ui_label_set_text(status, msg);
    ui_fttext_refresh(preview);  // preview live, tanpa reboot
}

void FontsPage::save() {
    fonts.save();
    select(fonts.currentId());
}

void FontsPage::load() {
    fonts.loadSaved();
    select(fonts.currentId());
}

}  // namespace settings
