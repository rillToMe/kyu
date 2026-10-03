// apps/settings/settings.cpp — SettingsApp: window, sidebar, page switching.
//
// MIGRASI UI (design system KyuzenOS): logika bisnis TIDAK disentuh. Yang
// berubah hanya lapisan presentasi:
//   * sidebar memakai ListView baris KAYA (ikon + judul) dengan padding
//     konten yang sama dengan halaman, jadi navigasi terbaca sebagai daftar
//     yang sama dengan isi halaman;
//   * halaman memakai <section> (judul + garis) alih-alih deretan label
//     telanjang, sehingga hierarki datang dari tipografi;
//   * jarak memakai token UI_SPACE_* (bukan angka yang dipilih per baris).
#include "settings.hpp"

#include "services/uifont.hpp"   // font UI sistem (font.ui) -> toolkit

namespace settings {

namespace {

// Sidebar digenerate dari satu koleksi (satu callback generik, bukan satu
// callback per tombol). Ikon memakai sistem ikon libui — semantik, bukan
// glyph: System/Personalization/Appearance/Fonts/About.
const NavigationItem kNav[] = {
    {SettingsPage::System, "System", UI_ICON_DISPLAY},
    {SettingsPage::Personalization, "Personalization", UI_ICON_IMAGE},
    {SettingsPage::Appearance, "Appearance", UI_ICON_PALETTE},
    {SettingsPage::Fonts, "Fonts", UI_ICON_FONT},
    {SettingsPage::About, "About", UI_ICON_INFO},
};
constexpr int kNavCount = 5;

constexpr int kWinW = 780;
constexpr int kWinH = 560;
constexpr int kSidebarW = 190;
// Jarak antara sidebar dan konten, dan margin tepi konten.
constexpr int kGap = UI_SPACE_LG;

void onSidebar(void* ud) { SettingsApp::self(ud)->onSidebarChanged(); }

void onKeyThunk(void* ud, std::uint32_t ascii, std::uint32_t scancode,
                std::uint32_t mods) {
    (void)scancode;
    (void)mods;
    SettingsApp::self(ud)->onKey(ascii);
}

}  // namespace

SettingsApp::SettingsApp()
    : win_(nullptr), sidebar_(nullptr), current_(SettingsPage::System) {}

SettingsApp::~SettingsApp() {
    if (win_) ui_window_destroy(win_);
}

void SettingsApp::trace(const char* tag) const {
    char b[64];
    int k = 0;
    const char* p = "[settings] ";
    while (*p && k < 50) b[k++] = *p++;
    while (tag && *tag && k < 60) b[k++] = *tag++;
    b[k++] = '\n';
    b[k] = '\0';
    print(b);
}

int SettingsApp::run() {
    // Ukuran window mengikuti layar (jangan lebih besar dari screen).
    int ww = kWinW, hh = kWinH;
    std::uint32_t sw = 0, sh = 0;
    if (sys_get_screen_size(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        if ((int)sw < ww) ww = (int)sw > 40 ? (int)sw - 40 : (int)sw;
        if ((int)sh < hh) hh = (int)sh > 60 ? (int)sh - 60 : (int)sh;
    }
    win_ = ui_window_create((std::uint32_t)ww, (std::uint32_t)hh);
    if (!win_) return 1;
    ui_window_set_title(win_, "Settings");
    ui_settings_load(win_);  // pakai tema tersimpan (kalau ada)
    // Font UI sistem DULU, sebelum widget dibuat: ukuran kontrol dan label
    // dihitung dari metrik font aktif. Settings ikut memakai font yang sedang
    // dipilih pengguna — termasuk saat pengguna baru menggantinya di halaman
    // Fonts, sehingga pratinjau dan UI memakai font yang sama.
    if (ui::uifont_install()) trace("font: system");
    ui_window_set_key(win_, onKeyThunk, this);

    buildUi();
    showPage(SettingsPage::System);
    trace("start");

    ui_window_run(win_);  // blocking; keluar via X / ESC
    return 0;
}

void SettingsApp::buildUi() {
    const int content_h = 560 - 60;
    const int content_w = 720 - kSidebarW - kGap * 3;

    // MenuBar + StatusBar: chrome yang sama dengan aplikasi lain. Tanpa ini
    // Settings terasa seperti dialog, bukan aplikasi.
    ui_widget_t* bar = ui_menubar_create(win_);
    ui_menubar_add_menu(bar, "File");
    ui_menubar_add_menu(bar, "View");
    ui_menubar_add_menu(bar, "Help");
    ui_window_add_bar(win_, bar);

    ui_widget_t* sb = ui_statusbar_create(win_);
    ui_statusbar_set_text(sb, "Settings", "Changes are saved immediately");
    ui_widget_set_size(sb, 720, UI_SPACE_XL);
    ui_window_add_bottom_bar(win_, sb);

    ui_widget_t* root = ui_hbox_create(win_, kGap);

    // Sidebar = daftar bernavigasi: ikon + judul. Barisnya memakai tinggi
    // daftar standar sehingga sidebar dan halaman sejajar.
    sidebar_ = ui_listview_create(win_, kSidebarW, content_h);
    for (int i = 0; i < kNavCount; i++)
        ui_listview_add_row(sidebar_, kNav[i].title, 0, kNav[i].icon, 0);
    ui_listview_set_change(sidebar_, onSidebar, this);
    ui_layout_add(root, sidebar_);

    // Satu ScrollView menampung semua page; page non-aktif disembunyikan
    // (layout melewati anak tersembunyi — pola bar cari Notepad).
    ui_widget_t* scroll = ui_scrollview_create(win_, content_w, content_h);
    ui_widget_t* pages = ui_vbox_create(win_, UI_SPACE_XL);

    system_.build(win_);
    personalization_.build(win_);
    appearance_.build(win_);
    fonts_.build(win_);
    about_.build(win_);

    ui_layout_add(pages, system_.root);
    ui_layout_add(pages, personalization_.root);
    ui_layout_add(pages, appearance_.root);
    ui_layout_add(pages, fonts_.root);
    ui_layout_add(pages, about_.root);

    ui_scrollview_set_child(scroll, pages);
    ui_layout_add(root, scroll);
    ui_window_add(win_, root);
}

void SettingsApp::showPage(SettingsPage page) {
    current_ = page;
    const int idx = static_cast<int>(page);
    ui_widget_t* roots[kNavCount] = {
        system_.root, personalization_.root, appearance_.root,
        fonts_.root, about_.root,
    };
    for (int i = 0; i < kNavCount; i++)
        ui_widget_set_visible(roots[i], i == idx);
    ui_listview_set_selected(sidebar_, idx);
    if (page == SettingsPage::System) system_.refresh();
    trace(kNav[idx].title);
}

void SettingsApp::onSidebarChanged() {
    const int sel = ui_listview_selected(sidebar_);
    if (sel < 0 || sel >= kNavCount) return;
    showPage(kNav[sel].page);
}

// Keyboard: 1..5 = pilih font (live preview), S = simpan font,
// N = wallpaper berikutnya, A = terapkan wallpaper.
// (Pola viewer versi C lama: shortcut tanpa mouse; memungkinkan verifikasi
// headless via sendkey QEMU — dipakai tools/fontdemo/run-qemu-sysfont.sh dan
// tests/host/probes/_settings_probe.py.)
void SettingsApp::onKey(std::uint32_t ascii) {
    if (ascii >= '1' && ascii <= '5') {
        showPage(SettingsPage::Fonts);
        fonts_.select((int)(ascii - '1'));
    } else if (ascii == 's' || ascii == 'S') {
        fonts_.save();
    } else if (ascii == 'n' || ascii == 'N') {
        showPage(SettingsPage::Personalization);
        personalization_.cycleNext();
    } else if (ascii == 'a' || ascii == 'A') {
        personalization_.apply();
    }
}

}  // namespace settings
