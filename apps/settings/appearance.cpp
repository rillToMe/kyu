// apps/settings/appearance.cpp — implementasi halaman Appearance.
#include "appearance.hpp"
#include "libui_xml.h"

namespace settings {

// Warna ditulis hex satu-token (libs/gui/color) — urutan field tetap 6 warna
// ABI ui_theme_t (sama seperti versi C lama + widget_demo).
const ui_theme_t kThemeDark = {
    COLOR_HEX(0x121212), COLOR_HEX(0xE0E0E0),
    COLOR_HEX(0xE94560), COLOR_HEX(0x0F3460),
    COLOR_HEX(0xFFFFFF), COLOR_HEX(0x2A4A7E),
};
const ui_theme_t kThemeLight = {
    COLOR_HEX(0xF0F0F0), COLOR_HEX(0x222222),
    COLOR_HEX(0xD32F2F), COLOR_HEX(0xCFD8DC),
    COLOR_HEX(0x222222), COLOR_HEX(0x90A4AE),
};
const ui_theme_t kThemeGreen = {
    COLOR_HEX(0x0D1F14), COLOR_HEX(0xDFF2E0),
    COLOR_HEX(0x4CAF50), COLOR_HEX(0x1B4D2E),
    COLOR_HEX(0xE8F5E9), COLOR_HEX(0x2E7D46),
};

namespace {

void onTheme(void* ud) {
    AppearancePage::ThemeBinding* b =
        static_cast<AppearancePage::ThemeBinding*>(ud);
    if (b && b->page) b->page->applyTheme(b->theme, b->name);
}

}  // namespace

void AppearancePage::build(ui_window_t* w) {
    win = w;
    // Phase F: form statis dideklarasikan sebagai XML (konsumen nyata
    // pertama libui_xml); callback tetap C++ native via ui_xml_bind.
    // Bentuk ini dikunci host test (libui_xml_test F-blok) + probe QEMU.
    static const char DOC[] =
        "<window><vbox spacing=\"6\" id=\"appearance_root\">"
        "<label text=\"Appearance\"/>"
        "<label text=\"Choose how KyuzenOS looks.\"/>"
        "<button id=\"theme_dark\" text=\"Dark\"/>"
        "<button id=\"theme_light\" text=\"Light\"/>"
        "<button id=\"theme_green\" text=\"Green\"/>"
        "<label id=\"appearance_status\" text=\"Theme: (from settings.ui)\"/>"
        "</vbox></window>";
    unsigned n = 0;
    while (DOC[n]) n++;
    ui_xml_error_t err;
    ui_xml_doc_t* doc = ui_xml_parse(DOC, n, &err);
    ui_xml_ctx_t* cx = doc ? ui_xml_ctx_create(w) : nullptr;
    // DETACHED: root di-parenting ke pages oleh settings.cpp (commit ke
    // window akan double-parenting: widget yang sama di dua layout).
    if (!doc || !cx || !ui_xml_inflate_detached(cx, doc, &err)) {
        // Fallback minimal (XML statis ini praktis tak pernah gagal —
        // host test menguncinya; ini hanya anti-null-root).
        if (cx) ui_xml_ctx_destroy(cx);
        if (doc) ui_xml_doc_destroy(doc);
        root = ui_vbox_create(w, 6);
        status = ui_label_create(w, "Appearance");
        ui_layout_add(root, status);
        return;
    }
    ui_xml_doc_destroy(doc);
    static const char* ids[3] = {"theme_dark", "theme_light", "theme_green"};
    const ui_theme_t* themes[3] = {&kThemeDark, &kThemeLight, &kThemeGreen};
    const char* names[3] = {"Dark", "Light", "Green"};
    for (int i = 0; i < 3; i++) {
        bindings[i].page = this;
        bindings[i].theme = themes[i];
        bindings[i].name = names[i];
        ui_xml_bind(cx, ids[i], UI_XML_ON_CLICK, onTheme, &bindings[i]);
    }
    root = ui_xml_find(cx, "appearance_root");
    status = ui_xml_find(cx, "appearance_status");
    ui_xml_release(cx);         // roots milik pages (lihat settings.cpp)
    ui_xml_ctx_destroy(cx);     // tanpa grup radio: aman langsung
}

void AppearancePage::applyTheme(const ui_theme_t* theme, const char* name) {
    if (!win || !theme || !name) return;
    ui_window_set_theme(win, theme);
    if (!ui_settings_save(win)) {
        ui_label_set_text(status, "Theme applied, save failed");
        return;
    }
    char msg[48];
    int k = 0;
    const char* p = "Theme: ";
    while (p[k]) { msg[k] = p[k]; k++; }
    for (int i = 0; name[i] && k < 46; i++) msg[k++] = name[i];
    msg[k] = '\0';
    ui_label_set_text(status, msg);
}

}  // namespace settings
