// apps/uigallery/gallery.cpp — implementasi Galeri UI.
//
// Struktur (pola aplikasi KyuzenOS yang benar):
//   MenuBar   → chrome aplikasi (judul menu)
//   Toolbar   → aksi cepat ber-ikon
//   HBox      → [sidebar daftar navigasi] [halaman ber-scroll]
//   StatusBar → umpan balik aksi terakhir
//
// Semua kontrol di sini adalah widget PRODUKSI libui. Tidak ada komponen
// tiruan: kalau bahasa visual berubah, galeri ikut berubah.
#include "gallery.hpp"

namespace gal {

namespace {

const NavItem kNav[] = {
    {GalPage::Typography, "Typography", UI_ICON_FONT},
    {GalPage::Colors, "Colors", UI_ICON_PALETTE},
    {GalPage::Buttons, "Buttons", UI_ICON_MORE},
    {GalPage::Inputs, "Inputs", UI_ICON_EDIT},
    {GalPage::Selection, "Selection", UI_ICON_CHECK},
    {GalPage::Lists, "Lists", UI_ICON_FILE},
    {GalPage::Navigation, "Navigation", UI_ICON_CHEVRON_RIGHT},
    {GalPage::Overlays, "Overlays", UI_ICON_INFO},
};
constexpr int kNavCount = static_cast<int>(sizeof(kNav) / sizeof(kNav[0]));

constexpr int kWinW = 960;
constexpr int kWinH = 640;
constexpr int kSidebarW = 200;
constexpr int kStatusH = 24;

// Aksen yang bisa dipilih di halaman Colors.
struct AccentChoice { ui_theme_accent_t value; const char* name; };
const AccentChoice kAccents[] = {
    {UI_ACCENT_NEUTRAL, "Neutral"}, {UI_ACCENT_BLUE, "Blue"},
    {UI_ACCENT_PURPLE, "Purple"},   {UI_ACCENT_GREEN, "Green"},
    {UI_ACCENT_ORANGE, "Orange"},   {UI_ACCENT_RED, "Red"},
};
constexpr int kAccentCount = static_cast<int>(sizeof(kAccents) / sizeof(kAccents[0]));

// ------------------------------------------------------------
// Thunk C. Toolkit memanggil callback C; semua state hidup di UiGalleryApp
// (satu instance, dibuat di main) — tanpa global mutable.
// ------------------------------------------------------------
void onNav(void* ud) {
    static_cast<UiGalleryApp*>(ud)->onNavChanged();
}

// Tombol "status": userdata = struct kecil berisi app + teks.
struct StatusAction {
    UiGalleryApp* app;
    const char* text;
};

// Tabel aksi statis (umur = umur proses; tanpa alokasi dinamis). Ukurannya
// cukup untuk seluruh tombol contoh di galeri.
StatusAction g_actions[32];
int g_action_n = 0;

void onStatusAction(void* ud) {
    StatusAction* a = static_cast<StatusAction*>(ud);
    if (a && a->app) a->app->setStatus(a->text);
}

// Bikin tombol dengan umpan balik status. variant/icon 0 = default.
ui_widget_t* mkButton(ui_window_t* w, UiGalleryApp* app, const char* label,
                      const char* status, int variant, int icon) {
    ui_widget_t* b = ui_button_create(w, label);
    if (variant) ui_button_set_variant(b, variant);
    if (icon) ui_button_set_icon(b, icon);
    if (g_action_n < 32) {
        g_actions[g_action_n].app = app;
        g_actions[g_action_n].text = status;
        ui_button_set_click(b, onStatusAction, &g_actions[g_action_n]);
        g_action_n++;
    }
    return b;
}

// ---- callback kontrol contoh ----
void onSwitch(void* ud) {
    UiGalleryApp* app = static_cast<UiGalleryApp*>(ud);
    // Tidak ada API pembaca di sini: status cukup melaporkan bahwa berubah.
    app->setStatus("Switch toggled");
}
void onCheck(void* ud) {
    static_cast<UiGalleryApp*>(ud)->setStatus("Checkbox toggled");
}
void onSlide(void* ud) {
    static_cast<UiGalleryApp*>(ud)->setStatus("Slider changed");
}
void onCombo(void* ud) {
    static_cast<UiGalleryApp*>(ud)->setStatus("Combo selection changed");
}
void onList(void* ud) {
    static_cast<UiGalleryApp*>(ud)->setStatus("List row selected");
}

// Mode tema (terang/gelap).
struct ModeAction { UiGalleryApp* app; ui_theme_mode_t mode; };
ModeAction g_modes[2];
void onMode(void* ud) {
    ModeAction* a = static_cast<ModeAction*>(ud);
    if (a && a->app) a->app->setMode(a->mode);
}

// Aksen.
struct AccentAction { UiGalleryApp* app; ui_theme_accent_t accent; };
AccentAction g_accents[kAccentCount];
void onAccent(void* ud) {
    AccentAction* a = static_cast<AccentAction*>(ud);
    if (a && a->app) a->app->setAccent(a->accent);
}

// Dialog demo.
void onDialogResult(void* ud, int index) {
    UiGalleryApp* app = static_cast<UiGalleryApp*>(ud);
    app->setStatus(index == 0 ? "Dialog: confirmed"
                              : (index < 0 ? "Dialog: cancelled (ESC)"
                                           : "Dialog: dismissed"));
}
ui_window_t* g_dialog_win = nullptr;
UiGalleryApp* g_dialog_app = nullptr;
void onShowDialog(void* ud) {
    (void)ud;
    if (!g_dialog_win) return;
    static const char* btns[2] = {"Confirm", "Cancel"};
    ui_dialog_show(g_dialog_win, "Confirm action",
                   "This demonstrates the modal surface:\n"
                   "one primary action, one quiet one.",
                   btns, 2, onDialogResult, g_dialog_app);
}

void onShowToast(void* ud) {
    (void)ud;
    if (g_dialog_win) ui_window_notify(g_dialog_win, "Saved", 2200);
}

}  // namespace

UiGalleryApp::UiGalleryApp()
    : win_(nullptr), sidebar_(nullptr), scroll_(nullptr), stack_(nullptr),
      status_(nullptr), mode_label_(nullptr), accent_label_(nullptr),
      current_(GalPage::Typography), demo_switch_(nullptr),
      demo_checkbox_(nullptr), demo_slider_(nullptr), demo_combo_(nullptr),
      demo_list_(nullptr), mode_(UI_THEME_DARK), accent_(UI_ACCENT_NEUTRAL) {
    for (int i = 0; i < kPageCount; i++) pages_[i] = nullptr;
}

UiGalleryApp::~UiGalleryApp() {
    if (win_) ui_window_destroy(win_);
}

// ------------------------------------------------------------
int UiGalleryApp::run() {
    int ww = kWinW, hh = kWinH;
    uint32_t sw = 0, sh = 0;
    if (sys_get_screen_size(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        if (static_cast<int>(sw) < ww) ww = static_cast<int>(sw) - 40;
        if (static_cast<int>(sh) < hh) hh = static_cast<int>(sh) - 60;
    }
    if (ww < 360) ww = 360;
    if (hh < 260) hh = 260;

    win_ = ui_window_create(static_cast<uint32_t>(ww),
                            static_cast<uint32_t>(hh));
    if (!win_) return 1;
    ui_window_set_title(win_, "UI Gallery");
    ui_settings_load(win_);     // hormati tema tersimpan (kalau ada)
    build();
    showPage(GalPage::Typography);
    ui_window_run(win_);
    return 0;
}

void UiGalleryApp::setStatus(const char* text) {
    if (status_) ui_label_set_text(status_, text);
}

void UiGalleryApp::syncLabels() {
    if (mode_label_)
        ui_label_set_text(mode_label_,
                          mode_ == UI_THEME_DARK ? "Mode: dark" : "Mode: light");
    if (accent_label_) {
        const char* nm = "Neutral";
        for (int i = 0; i < kAccentCount; i++)
            if (kAccents[i].value == accent_) nm = kAccents[i].name;
        char buf[40];
        int k = 0;
        const char* p = "Accent: ";
        while (*p && k < 38) buf[k++] = *p++;
        for (int i = 0; nm[i] && k < 38; i++) buf[k++] = nm[i];
        buf[k] = '\0';
        ui_label_set_text(accent_label_, buf);
    }
}

void UiGalleryApp::applyTheme() {
    ui_theme_config_t cfg;
    cfg.mode = mode_;
    cfg.accent = accent_;
    cfg.custom = color_hex(0x000000);
    ui_window_set_theme_config(win_, &cfg);
    syncLabels();
}

void UiGalleryApp::setMode(ui_theme_mode_t mode) {
    mode_ = mode;
    applyTheme();
    setStatus(mode == UI_THEME_DARK ? "Theme: dark" : "Theme: light");
}

void UiGalleryApp::setAccent(ui_theme_accent_t accent) {
    accent_ = accent;
    applyTheme();
    setStatus("Accent changed");
}

// ------------------------------------------------------------
// Bangun UI
// ------------------------------------------------------------
void UiGalleryApp::build() {
    g_dialog_win = win_;
    g_dialog_app = this;

    // MenuBar: chrome aplikasi. Isinya sengaja sedikit — galeri bukan app
    // penuh fitur, dan menu yang berisi banyak item kosong bukan contoh bagus.
    ui_widget_t* bar = ui_menubar_create(win_);
    ui_menubar_add_menu(bar, "File");
    ui_menubar_add_menu(bar, "View");
    ui_menubar_add_menu(bar, "Help");
    ui_window_add_bar(win_, bar);

    // Toolbar: aksi cepat ber-ikon (satu bahasa dengan sidebar).
    ui_widget_t* tb = ui_toolbar_create(win_);
    g_action_n = 0;   // tabel aksi dipakai ulang dari awal
    ui_toolbar_add_button_icon(tb, "Dialog", UI_ICON_INFO, onShowDialog, this);
    ui_toolbar_add_button_icon(tb, "Toast", UI_ICON_CHECK, onShowToast, this);
    ui_window_add_bar(win_, tb);

    // StatusBar: umpan balik aksi (dibuat lebih dulu supaya callback aman).
    ui_widget_t* sb = ui_statusbar_create(win_);
    ui_widget_set_size(sb, static_cast<int>(kWinW), kStatusH);
    ui_window_add_bar(win_, sb);

    // Root: sidebar + halaman.
    const int content_h = kWinH - 160;
    ui_widget_t* root = ui_hbox_create(win_, UI_SPACE_LG);

    sidebar_ = ui_listview_create(win_, kSidebarW, content_h);
    for (int i = 0; i < kNavCount; i++)
        ui_listview_add_row(sidebar_, kNav[i].title, 0, kNav[i].icon, 0);
    ui_listview_set_change(sidebar_, onNav, this);
    ui_layout_add(root, sidebar_);

    // Kolom kanan: judul halaman + umpan balik aksi + halaman ber-scroll.
    ui_widget_t* right = ui_vbox_create(win_, UI_SPACE_SM);
    status_ = ui_label_create(win_, "Ready");
    ui_layout_add(right, status_);

    scroll_ = ui_scrollview_create(win_, kWinW - kSidebarW - 96, content_h - 32);
    stack_ = ui_vbox_create(win_, UI_SPACE_XL);

    // Halaman dibangun lebih dulu, lalu ditambahkan berurutan.
    pages_[static_cast<int>(GalPage::Typography)] = pageTypography(win_);
    pages_[static_cast<int>(GalPage::Colors)] = pageColors(win_);
    pages_[static_cast<int>(GalPage::Buttons)] = pageButtons(win_);
    pages_[static_cast<int>(GalPage::Inputs)] = pageInputs(win_);
    pages_[static_cast<int>(GalPage::Selection)] = pageSelection(win_);
    pages_[static_cast<int>(GalPage::Lists)] = pageLists(win_);
    pages_[static_cast<int>(GalPage::Navigation)] = pageNavigation(win_);
    pages_[static_cast<int>(GalPage::Overlays)] = pageOverlays(win_);
    for (int i = 0; i < kPageCount; i++)
        if (pages_[i]) ui_layout_add(stack_, pages_[i]);

    ui_scrollview_set_child(scroll_, stack_);
    ui_layout_add(right, scroll_);
    ui_layout_add(root, right);
    ui_window_add(win_, root);

    ui_statusbar_set_text(sb, "Ready", "UI Gallery  |  KyuzenOS design system");
    syncLabels();
}

// ------------------------------------------------------------
// Halaman: Tipografi
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageTypography(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Hierarchy comes from type, not from boxes."));

    ui_widget_t* sec = ui_section_create(w, "Type roles", UI_SPACE_SM);
    static const char* roles[8] = {
        "display", "title", "section", "body",
        "body_emphasis", "label", "caption", "mono"
    };
    static const char* samples[8] = {
        "Display", "Title", "SECTION", "Body text",
        "Body emphasis", "Label", "Caption text", "monospace 0123"
    };
    for (int i = 0; i < 8; i++) {
        ui_widget_t* row = ui_hbox_create(w, UI_SPACE_LG);
        ui_widget_t* nm = ui_label_create(w, roles[i]);
        ui_widget_set_size(nm, 130, 20);
        ui_layout_add(row, nm);
        ui_layout_add(row, ui_label_create(w, samples[i]));
        ui_layout_add(sec, row);
    }
    ui_layout_add(root, sec);
    return root;
}

// ------------------------------------------------------------
// Halaman: Warna
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageColors(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Semantic roles. Both modes are first-class."));

    ui_widget_t* sec = ui_section_create(w, "Mode", UI_SPACE_SM);
    ui_widget_t* mrow = ui_hbox_create(w, UI_SPACE_SM);
    g_modes[0].app = this; g_modes[0].mode = UI_THEME_DARK;
    g_modes[1].app = this; g_modes[1].mode = UI_THEME_LIGHT;
    ui_widget_t* bd = ui_button_create(w, "Dark");
    ui_button_set_click(bd, onMode, &g_modes[0]);
    ui_widget_t* bl = ui_button_create(w, "Light");
    ui_button_set_click(bl, onMode, &g_modes[1]);
    ui_layout_add(mrow, bd);
    ui_layout_add(mrow, bl);
    mode_label_ = ui_label_create(w, "Mode: dark");
    ui_layout_add(mrow, mode_label_);
    ui_layout_add(sec, mrow);
    ui_layout_add(root, sec);

    ui_widget_t* sec2 = ui_section_create(w, "Accent", UI_SPACE_SM);
    ui_widget_t* arow = ui_hbox_create(w, UI_SPACE_SM);
    for (int i = 0; i < kAccentCount; i++) {
        g_accents[i].app = this;
        g_accents[i].accent = kAccents[i].value;
        ui_widget_t* b = ui_button_create(w, kAccents[i].name);
        ui_button_set_click(b, onAccent, &g_accents[i]);
        ui_layout_add(arow, b);
    }
    ui_layout_add(sec2, arow);
    accent_label_ = ui_label_create(w, "Accent: Neutral");
    ui_layout_add(sec2, accent_label_);
    ui_layout_add(root, sec2);
    return root;
}

// ------------------------------------------------------------
// Halaman: Tombol
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageButtons(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Hierarchy without shadows: one primary, quiet alternatives."));

    ui_widget_t* sec = ui_section_create(w, "Variants", UI_SPACE_SM);
    ui_widget_t* r1 = ui_hbox_create(w, UI_SPACE_SM);
    ui_layout_add(r1, mkButton(w, this, "Primary", "Primary clicked",
                               UI_BUTTON_PRIMARY, 0));
    ui_layout_add(r1, mkButton(w, this, "Secondary", "Secondary clicked",
                               UI_BUTTON_SECONDARY, 0));
    ui_layout_add(r1, mkButton(w, this, "Tertiary", "Tertiary clicked",
                               UI_BUTTON_TERTIARY, 0));
    ui_layout_add(r1, mkButton(w, this, "Delete", "Destructive clicked",
                               UI_BUTTON_DANGER, UI_ICON_TRASH));
    ui_layout_add(sec, r1);

    ui_widget_t* r2 = ui_hbox_create(w, UI_SPACE_SM);
    ui_layout_add(r2, mkButton(w, this, "", "Icon button clicked", 0,
                               UI_ICON_SETTINGS));
    ui_layout_add(r2, mkButton(w, this, "Open folder", "Icon+text clicked", 0,
                               UI_ICON_FOLDER));
    ui_layout_add(sec, r2);

    ui_widget_t* r3 = ui_hbox_create(w, UI_SPACE_SM);
    ui_widget_t* db = mkButton(w, this, "Disabled", "should not fire",
                               UI_BUTTON_SECONDARY, 0);
    ui_widget_set_enabled(db, 0);
    ui_layout_add(r3, db);
    ui_widget_t* dbt = mkButton(w, this, "Disabled primary", "should not fire",
                                UI_BUTTON_PRIMARY, 0);
    ui_widget_set_enabled(dbt, 0);
    ui_layout_add(r3, dbt);
    ui_layout_add(sec, r3);
    ui_layout_add(root, sec);

    ui_widget_t* sec2 = ui_section_create(w, "Icons", UI_SPACE_SM);
    ui_widget_t* irow = ui_hbox_create(w, UI_SPACE_SM);
    static const int icons[8] = {
        UI_ICON_SETTINGS, UI_ICON_DISPLAY, UI_ICON_PALETTE, UI_ICON_FONT,
        UI_ICON_NETWORK, UI_ICON_POWER, UI_ICON_SEARCH, UI_ICON_CLOSE
    };
    for (int i = 0; i < 8; i++)
        ui_layout_add(irow, mkButton(w, this, "", "Icon clicked", 0, icons[i]));
    ui_layout_add(sec2, irow);
    ui_layout_add(root, sec2);
    return root;
}

// ------------------------------------------------------------
// Halaman: Input
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageInputs(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Inputs are recessed wells: focus is the accent border."));

    ui_widget_t* sec = ui_section_create(w, "Text fields", UI_SPACE_SM);
    ui_widget_t* t1 = ui_textbox_create(w, 300);
    ui_textbox_set_text(t1, "Editable value");
    ui_layout_add(sec, t1);
    ui_widget_t* t2 = ui_textbox_create(w, 300);
    ui_textbox_set_text(t2, "invalid@value");
    ui_textbox_set_error(t2, 1);
    ui_layout_add(sec, t2);
    ui_widget_t* t3 = ui_textbox_create(w, 300);
    ui_textbox_set_text(t3, "Disabled field");
    ui_widget_set_enabled(t3, 0);
    ui_layout_add(sec, t3);
    ui_layout_add(root, sec);

    ui_widget_t* sec2 = ui_section_create(w, "Slider", UI_SPACE_SM);
    demo_slider_ = ui_slider_create(w, 0, 100);
    ui_slider_set_value(demo_slider_, 40);
    ui_slider_set_change(demo_slider_, onSlide, this);
    ui_layout_add(sec2, demo_slider_);
    ui_layout_add(root, sec2);

    ui_widget_t* sec3 = ui_section_create(w, "Progress", UI_SPACE_SM);
    ui_widget_t* pb = ui_progressbar_create(w, 300);
    ui_progressbar_set_value(pb, 65);
    ui_layout_add(sec3, pb);
    ui_layout_add(root, sec3);
    return root;
}

// ------------------------------------------------------------
// Halaman: Pemilihan
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageSelection(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Checkbox = form value. Switch = setting that applies immediately."));

    ui_widget_t* sec = ui_section_create(w, "Checkboxes", UI_SPACE_SM);
    demo_checkbox_ = ui_checkbox_create(w, "Enable notifications");
    ui_checkbox_set_toggle(demo_checkbox_, onCheck, this);
    ui_layout_add(sec, demo_checkbox_);
    ui_layout_add(sec, ui_checkbox_create(w, "Start on login"));
    ui_widget_t* cdis = ui_checkbox_create(w, "Managed by policy");
    ui_widget_set_enabled(cdis, 0);
    ui_layout_add(sec, cdis);
    ui_layout_add(root, sec);

    ui_widget_t* sec2 = ui_section_create(w, "Switches", UI_SPACE_SM);
    demo_switch_ = ui_switch_create(w, "Wi-Fi");
    ui_switch_set_change(demo_switch_, onSwitch, this);
    ui_layout_add(sec2, demo_switch_);
    ui_layout_add(sec2, ui_switch_create(w, "Bluetooth"));
    ui_widget_t* sdis = ui_switch_create(w, "Airplane mode");
    ui_widget_set_enabled(sdis, 0);
    ui_layout_add(sec2, sdis);
    ui_layout_add(root, sec2);

    ui_widget_t* sec3 = ui_section_create(w, "Radio", UI_SPACE_SM);
    ui_radio_group_t* g = ui_radio_group_create();
    ui_widget_t* ra = ui_radio_create(w, "Balanced");
    ui_widget_t* rb = ui_radio_create(w, "Performance");
    ui_radio_set_group(ra, g);
    ui_radio_set_group(rb, g);
    ui_radio_set_selected(ra, 1);
    ui_layout_add(sec3, ra);
    ui_layout_add(sec3, rb);
    ui_layout_add(root, sec3);
    return root;
}

// ------------------------------------------------------------
// Halaman: Daftar
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageLists(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Rows compose icon, title, description, trailing and chevron."));

    ui_widget_t* sec = ui_section_create(w, "Settings-style rows", UI_SPACE_SM);
    demo_list_ = ui_listview_create(w, 520, 280);
    ui_listview_add_row(demo_list_, "Display",
                        "Resolution, scaling, night light", UI_ICON_DISPLAY, 1);
    ui_listview_add_row(demo_list_, "Appearance",
                        "Theme, accent color, wallpaper", UI_ICON_PALETTE, 1);
    ui_listview_add_row(demo_list_, "Fonts", "Interface font and size",
                        UI_ICON_FONT, 1);
    ui_listview_add_row(demo_list_, "Network", "Wi-Fi, Ethernet, VPN",
                        UI_ICON_NETWORK, 1);
    ui_listview_add_row(demo_list_, "Power", "Sleep, battery, startup",
                        UI_ICON_POWER, 1);
    ui_listview_set_row_disabled(demo_list_, 4, 1);
    ui_listview_set_change(demo_list_, onList, this);
    ui_layout_add(sec, demo_list_);
    ui_layout_add(root, sec);

    ui_widget_t* sec2 = ui_section_create(w, "Combo box", UI_SPACE_SM);
    demo_combo_ = ui_combobox_create(w, 240);
    ui_combobox_add_item(demo_combo_, "Neutral");
    ui_combobox_add_item(demo_combo_, "Blue");
    ui_combobox_add_item(demo_combo_, "Purple");
    ui_combobox_set_selected(demo_combo_, 0);
    ui_combobox_set_change(demo_combo_, onCombo, this);
    ui_layout_add(sec2, demo_combo_);
    ui_layout_add(root, sec2);
    return root;
}

// ------------------------------------------------------------
// Halaman: Navigasi
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageNavigation(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Tabs: active = accent underline, quiet strip, no boxes."));

    ui_widget_t* sec = ui_section_create(w, "Tabs", UI_SPACE_SM);
    ui_widget_t* tabs = ui_tab_create(w, 520, 220);
    ui_tab_add(tabs, "General", ui_vbox_create(w, UI_SPACE_SM));
    ui_tab_add(tabs, "Advanced", ui_vbox_create(w, UI_SPACE_SM));
    ui_tab_add(tabs, "Expert", ui_vbox_create(w, UI_SPACE_SM));
    ui_tab_set_enabled(tabs, 2, 0);
    ui_layout_add(sec, tabs);
    ui_layout_add(root, sec);
    return root;
}

// ------------------------------------------------------------
// Halaman: Overlay
// ------------------------------------------------------------
ui_widget_t* UiGalleryApp::pageOverlays(ui_window_t* w) {
    ui_widget_t* root = ui_vbox_create(w, UI_SPACE_LG);
    ui_layout_add(root, ui_label_create(w,
        "Floating surfaces use elevation tokens, not decoration."));

    ui_widget_t* sec = ui_section_create(w, "Dialogs & toasts", UI_SPACE_SM);
    ui_widget_t* row = ui_hbox_create(w, UI_SPACE_SM);
    ui_widget_t* db = ui_button_create(w, "Show dialog");
    ui_button_set_variant(db, UI_BUTTON_PRIMARY);
    ui_button_set_click(db, onShowDialog, this);
    ui_layout_add(row, db);
    ui_widget_t* tb2 = ui_button_create(w, "Show toast");
    ui_button_set_click(tb2, onShowToast, this);
    ui_layout_add(row, tb2);
    ui_layout_add(sec, row);
    ui_layout_add(root, sec);
    return root;
}

// ------------------------------------------------------------
// Navigasi halaman
// ------------------------------------------------------------
void UiGalleryApp::onNavChanged() {
    int sel = ui_listview_selected(sidebar_);
    if (sel < 0 || sel >= kNavCount) return;
    showPage(kNav[sel].page);
}

void UiGalleryApp::showPage(GalPage page) {
    current_ = page;
    for (int i = 0; i < kPageCount; i++) {
        if (pages_[i])
            ui_widget_set_visible(pages_[i], i == static_cast<int>(page));
    }
    ui_listview_set_selected(sidebar_, static_cast<int>(page));
}

}  // namespace gal
