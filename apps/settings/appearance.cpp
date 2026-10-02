// apps/settings/appearance.cpp — implementasi halaman Appearance.
//
// MIGRASI UI:
//   1. Tiga tema 6-warna hardcoded (Dark/Light/Green) diganti jalur tema yang
//      sudah ada di libui: `ui_theme_config_t` (mode × aksen). Palet penuh
//      diturunkan toolkit, jadi halaman ini tidak perlu tahu satu warna pun.
//   2. Form deklarasinya pindah ke file XML SUNGGUHAN
//      (ui/xml/settings_appearance.xml) yang di-embed saat build lewat
//      cmake/KyuzenUiXml.cmake. Tidak ada lagi string XML di dalam .cpp:
//      dokumen bisa di-review dan disorot editor.
//
// Radio dipakai untuk MODE (dua pilihan eksklusif = bentuk pilihan). Aksen
// memakai tombol karena aksen adalah "coba cepat", bukan pilihan bentuk.
#include "appearance.hpp"

#include "ui_xml_data.h"   // generated: ui_xml_settings_appearance[+_len]

namespace settings {

namespace {

struct AccentDef { ui_theme_accent_t value; const char* name; const char* id; };
// Urutan sejajar dengan tombol di settings_appearance.xml.
const AccentDef kAccents[6] = {
    {UI_ACCENT_NEUTRAL, "Neutral", "accent_neutral"},
    {UI_ACCENT_BLUE, "Blue", "accent_blue"},
    {UI_ACCENT_PURPLE, "Purple", "accent_purple"},
    {UI_ACCENT_GREEN, "Green", "accent_green"},
    {UI_ACCENT_ORANGE, "Orange", "accent_orange"},
    {UI_ACCENT_RED, "Red", "accent_red"},
};

void onMode(void* ud) {
    AppearancePage::ModeBinding* b =
        static_cast<AppearancePage::ModeBinding*>(ud);
    if (b && b->page) b->page->setMode(b->mode);
}

void onAccent(void* ud) {
    AppearancePage::AccentBinding* b =
        static_cast<AppearancePage::AccentBinding*>(ud);
    if (b && b->page) b->page->setAccent(b->accent, b->name);
}

}  // namespace

void AppearancePage::build(ui_window_t* w) {
    win = w;
    ui_xml_error_t err;
    ui_xml_doc_t* doc = ui_xml_parse(ui_xml_settings_appearance,
                                     ui_xml_settings_appearance_len, &err);
    if (!doc) {
        // Fallback minimal (dokumen terkunci host test; ini anti-null-root).
        root = ui_vbox_create(w, UI_SPACE_LG);
        status = ui_label_create(w, "Appearance");
        ui_layout_add(root, status);
        return;
    }
    ui_xml_ctx_t* cx = ui_xml_ctx_create(w);
    if (!cx) {
        ui_xml_doc_destroy(doc);
        root = ui_vbox_create(w, UI_SPACE_LG);
        status = ui_label_create(w, "Appearance");
        ui_layout_add(root, status);
        return;
    }
    // DETACHED: root di-parenting ke pages oleh settings.cpp (commit ke
    // window akan double-parenting: widget yang sama di dua layout).
    if (!ui_xml_inflate_detached(cx, doc, &err)) {
        ui_xml_ctx_destroy(cx);
        ui_xml_doc_destroy(doc);
        root = ui_vbox_create(w, UI_SPACE_LG);
        status = ui_label_create(w, "Appearance");
        ui_layout_add(root, status);
        return;
    }
    ui_xml_doc_destroy(doc);

    // --- Mode: grup radio dari XML (group="mode") ---
    mode_dark = ui_xml_find(cx, "mode_dark");
    mode_light = ui_xml_find(cx, "mode_light");
    mode_bindings[0].page = this; mode_bindings[0].mode = UI_THEME_DARK;
    mode_bindings[1].page = this; mode_bindings[1].mode = UI_THEME_LIGHT;
    ui_xml_bind(cx, "mode_dark", UI_XML_ON_CHANGE, onMode, &mode_bindings[0]);
    ui_xml_bind(cx, "mode_light", UI_XML_ON_CHANGE, onMode, &mode_bindings[1]);

    // --- Aksen: satu tombol per entri (id dari XML) ---
    for (int i = 0; i < 6; i++) {
        accent_bindings[i].page = this;
        accent_bindings[i].accent = kAccents[i].value;
        accent_bindings[i].name = kAccents[i].name;
        ui_xml_bind(cx, kAccents[i].id, UI_XML_ON_CLICK, onAccent,
                    &accent_bindings[i]);
    }

    root = ui_xml_find(cx, "appearance_root");
    status = ui_xml_find(cx, "appearance_status");
    ui_xml_release(cx);         // roots milik pages (lihat settings.cpp)
    // Grup radio (`group="mode"` di XML) DIMILIKI konteks dan dihancurkan
    // bersama ctx_destroy. Setelah itu radio kembali mandiri: masing-masing
    // masih bisa dipilih, tapi tidak lagi eksklusif satu sama lain.
    //
    // Karena mode HARUS eksklusif, grup dibuat ulang di sini dan anggota
    // didaftarkan ulang — inilah harga "grup radio milik konteks XML", dan
    // ditulis eksplisit supaya tidak jadi jebakan bagi pembaca berikutnya.
    ui_xml_ctx_destroy(cx);
    mode_group = ui_radio_group_create();
    if (mode_dark) ui_radio_set_group(mode_dark, mode_group);
    if (mode_light) ui_radio_set_group(mode_light, mode_group);
    syncMode();
}

void AppearancePage::setMode(ui_theme_mode_t mode) {
    mode_ = mode;
    apply();
}

void AppearancePage::setAccent(ui_theme_accent_t accent, const char* name) {
    accent_ = accent;
    accent_name_ = name ? name : "";
    apply();
}

void AppearancePage::apply() {
    if (!win) return;
    ui_theme_config_t cfg;
    cfg.mode = mode_;
    cfg.accent = accent_;
    cfg.custom = color_hex(0x000000);
    ui_window_set_theme_config(win, &cfg);
    setStatusTheme();
}

void AppearancePage::setStatusTheme() {
    if (!status) return;
    char msg[64];
    int k = 0;
    const char* p = "Theme: ";
    while (*p && k < 56) msg[k++] = *p++;
    const char* m = (mode_ == UI_THEME_DARK) ? "Dark" : "Light";
    while (*m && k < 56) msg[k++] = *m++;
    if (k < 56) { msg[k++] = ' '; msg[k++] = '/'; msg[k++] = ' '; }
    for (int i = 0; accent_name_[i] && k < 62; i++) msg[k++] = accent_name_[i];
    msg[k] = '\0';
    // Tema tersimpan dulu; kalau gagal, tetap beri tahu status terpasang.
    if (!ui_settings_save(win)) {
        ui_label_set_text(status, "Theme applied, save failed");
        return;
    }
    ui_label_set_text(status, msg);
}

void AppearancePage::syncMode() {
    if (mode_dark) ui_radio_set_selected(mode_dark, mode_ == UI_THEME_DARK);
    if (mode_light) ui_radio_set_selected(mode_light, mode_ == UI_THEME_LIGHT);
}

}  // namespace settings
