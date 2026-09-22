// apps/filemanager/app.cpp — lapisan UI + state File Manager (implementasi).
//
// Aturan yang dijaga di file ini:
//   * Tidak ada syscall filesystem — semua lewat fs::FileSystem/DirectoryModel.
//   * Tidak ada aturan urutan/tipe berkas — itu di model.*.
//   * Setiap perubahan state → reloadView() (view selalu dibangun dari model).
//   * Damage: pembaruan lewat widget (masing-masing menandai rect-nya sendiri),
//     jadi toolkit melukis ulang hanya region yang berubah. Hanya perubahan tata
//     letak (mode tampilan, sidebar, bar rename) yang memicu satu frame repaint
//     penuh — itu memang perilaku ui_widget_set_visible() di toolkit.
#include "app.hpp"

#include <cstdint>

namespace fm {
namespace {

constexpr int kWindowW = 720;
constexpr int kWindowH = 520;
constexpr int kMargin = 8;
constexpr int kInnerW = kWindowW - kMargin * 2;             // 704
constexpr int kSpacing = 8;
constexpr int kPathH = 28;
constexpr int kContentH = 350;
constexpr int kStatusH = 20;
constexpr int kSidebarW = 132;
constexpr int kSidebarGap = 8;
constexpr int kViewW = kInnerW - kSidebarW - kSidebarGap;   // 564
constexpr int kMaxRows = 64;        // Table::MAX_ROWS
constexpr int kDoubleClickMs = 400;

// Entri boleh dijadikan target operasi? Nama yang lebih panjang dari kNameMax
// HANYA dipotong untuk tampilan — memakainya sebagai path bisa mengenai berkas
// yang SALAH, jadi entri seperti itu ditolak (lihat fs.hpp kNameMax).
bool canOperate(const FileEntry& e) { return e.path_ok && !e.name_truncated; }

// Salin std::string ke buffer C dengan batas (tanpa libc; app ini tidak
// menautkan libc untuk string).
void toCStr(const std::string& s, char* out, int cap) {
    if (cap <= 0) return;
    int n = 0;
    for (; n < cap - 1 && (std::size_t)n < s.size(); ++n) out[n] = s[n];
    out[n] = '\0';
}

const char* const kOkButtons[1] = { "OK" };
const char* const kDeleteButtons[2] = { "Delete", "Cancel" };

// Index item menu View — set_checked()/set_enabled() memakai index BARIS,
// termasuk separator (Menu menyimpan separator sebagai item). Urutan
// penambahan di buildMenus() harus tetap sesuai daftar ini.
enum {
    kViewItemList    = 0,
    kViewItemIcons   = 1,
    /* separator     = 2 */
    kViewItemSidebar = 3,
    /* separator     = 4 */
    kViewItemSortName = 5,
    kViewItemSortType = 6,
    kViewItemSortSize = 7,
    /* separator     = 8 */
    kViewItemAsc  = 9,
    kViewItemDesc = 10,
};
enum { kGoItemBack = 0, kGoItemForward = 1 };

// --- thunk callback C → method (pola Gallery) -------------------------------
void tBack(void* ud)      { static_cast<FileManagerApp*>(ud)->cmdBack(); }
void tForward(void* ud)   { static_cast<FileManagerApp*>(ud)->cmdForward(); }
void tUp(void* ud)        { static_cast<FileManagerApp*>(ud)->cmdUp(); }
void tRefresh(void* ud)   { static_cast<FileManagerApp*>(ud)->cmdRefresh(); }
void tOpen(void* ud)      { static_cast<FileManagerApp*>(ud)->cmdOpen(); }
void tNewFolder(void* ud) { static_cast<FileManagerApp*>(ud)->cmdNewFolder(); }
void tNewFile(void* ud)   { static_cast<FileManagerApp*>(ud)->cmdNewFile(); }
void tRename(void* ud)    { static_cast<FileManagerApp*>(ud)->cmdRename(); }
void tDelete(void* ud)    { static_cast<FileManagerApp*>(ud)->cmdDelete(); }
void tClose(void* ud)     { static_cast<FileManagerApp*>(ud)->cmdClose(); }
void tToggleView(void* ud)   { static_cast<FileManagerApp*>(ud)->cmdToggleView(); }
void tToggleSidebar(void* ud) { static_cast<FileManagerApp*>(ud)->cmdToggleSidebar(); }
void tViewList(void* ud)  { static_cast<FileManagerApp*>(ud)->cmdViewMode(ViewMode::List); }
void tViewIcon(void* ud)  { static_cast<FileManagerApp*>(ud)->cmdViewMode(ViewMode::Icon); }
void tSortName(void* ud)  { static_cast<FileManagerApp*>(ud)->cmdSort(SortKey::Name, false); }
void tSortType(void* ud)  { static_cast<FileManagerApp*>(ud)->cmdSort(SortKey::Type, false); }
void tSortSize(void* ud)  { static_cast<FileManagerApp*>(ud)->cmdSort(SortKey::Size, false); }
void tSortAsc(void* ud)   { static_cast<FileManagerApp*>(ud)->cmdSort(SortKey::Name, false); }
void tSortDesc(void* ud)  { static_cast<FileManagerApp*>(ud)->cmdSort(SortKey::Name, true); }

}  // namespace

// ------------------------------------------------------------
// Siklus hidup
// ------------------------------------------------------------
FileManagerApp::~FileManagerApp() {
    if (win_) ui_window_destroy(win_);
}

int FileManagerApp::run(const char* initial) {
    win_ = ui_window_create(kWindowW, kWindowH);
    if (!win_) return 1;
    ui_window_set_title(win_, "File Manager");

    // Widget dibuat DULU (sidebar/menu mengisi dirinya lewat widget yang ada),
    // lalu isi menu, lalu sidebar (butuh verifikasi direktori lewat adapter).
    buildContent();
    collectShortcuts();
    buildMenus();
    buildToolbar();
    buildContextMenus();
    icons_.load();

    ui_window_set_key(win_, &FileManagerApp::onAppKey, this);
    ui_window_set_escape(win_, &FileManagerApp::onEscape, this);

    const std::string start = (initial && initial[0]) ? std::string(initial) : std::string("/");
    std::string path = start;
    if (!FileSystem::isDirectory(path)) path = "/";
    model_.load(path, kMaxRows);
    history_.reset(model_.path());
    reloadView(nullptr);
    trace("start", model_.path());

    ui_window_run(win_);     // blocking sampai window ditutup
    return 0;
}

// ------------------------------------------------------------
// Pembangunan UI
// ------------------------------------------------------------
void FileManagerApp::buildMenus() {
    menubar_ = ui_menubar_create(win_);

    menu_file_ = ui_menubar_add_menu(menubar_, "File");
    ui_menu_add_item_acc(menu_file_, "New Folder", "Ctrl+N", tNewFolder, this);
    ui_menu_add_item_acc(menu_file_, "New File", 0, tNewFile, this);
    ui_menu_add_sep(menu_file_);
    ui_menu_add_item_acc(menu_file_, "Open", "Enter", tOpen, this);
    ui_menu_add_item_acc(menu_file_, "Rename", "F2", tRename, this);
    ui_menu_add_item_acc(menu_file_, "Delete", "Del", tDelete, this);
    ui_menu_add_sep(menu_file_);
    ui_menu_add_item_acc(menu_file_, "Refresh", "Ctrl+R", tRefresh, this);
    ui_menu_add_sep(menu_file_);
    ui_menu_add_item_acc(menu_file_, "Close", "Esc", tClose, this);

    menu_view_ = ui_menubar_add_menu(menubar_, "View");
    ui_menu_add_item(menu_view_, "List", tViewList, this);            // 0
    ui_menu_add_item(menu_view_, "Icons", tViewIcon, this);           // 1
    ui_menu_add_sep(menu_view_);                                     // 2
    ui_menu_add_item(menu_view_, "Sidebar", tToggleSidebar, this);    // 3
    ui_menu_add_sep(menu_view_);                                     // 4
    ui_menu_add_item(menu_view_, "Sort by Name", tSortName, this);    // 5
    ui_menu_add_item(menu_view_, "Sort by Type", tSortType, this);    // 6
    ui_menu_add_item(menu_view_, "Sort by Size", tSortSize, this);    // 7
    ui_menu_add_sep(menu_view_);                                     // 8
    ui_menu_add_item(menu_view_, "Ascending", tSortAsc, this);        // 9
    ui_menu_add_item(menu_view_, "Descending", tSortDesc, this);      // 10

    menu_go_ = ui_menubar_add_menu(menubar_, "Go");
    ui_menu_add_item_acc(menu_go_, "Back", "Alt+Left", tBack, this);          // 0
    ui_menu_add_item_acc(menu_go_, "Forward", "Alt+Right", tForward, this);   // 1
    ui_menu_add_item_acc(menu_go_, "Up", "Backspace", tUp, this);             // 2
    ui_menu_add_sep(menu_go_);                                               // 3
    // Satu callback melayani semua shortcut sidebar; userdata-nya binding milik
    // app (reserve dulu supaya alamat stabil selama menu hidup).
    go_bindings_.clear();
    go_bindings_.reserve(shortcuts_.size());
    for (std::size_t i = 0; i < shortcuts_.size(); ++i) {
        go_bindings_.push_back(GoBinding{ this, (int)i });
        ui_menu_add_item(menu_go_, shortcuts_[i].label, &FileManagerApp::onGoShortcut,
                         &go_bindings_.back());
    }

    ui_window_add_bar(win_, menubar_);
}

void FileManagerApp::buildToolbar() {
    toolbar_ = ui_toolbar_create(win_);
    ui_toolbar_add_button(toolbar_, "< Back", tBack, this);
    ui_toolbar_add_button(toolbar_, "Fwd >", tForward, this);
    ui_toolbar_add_button(toolbar_, "Up", tUp, this);
    ui_toolbar_add_button(toolbar_, "Refresh", tRefresh, this);
    ui_toolbar_add_button(toolbar_, "New Folder", tNewFolder, this);
    ui_toolbar_add_button(toolbar_, "List / Icons", tToggleView, this);
    ui_window_add_bar(win_, toolbar_);
}

void FileManagerApp::buildContent() {
    // Baris path: kolom path + tombol Go.
    ui_widget_t* path_row = ui_hbox_create(win_, 6);
    ui_widget_set_size(path_row, kInnerW, kPathH);
    path_box_ = ui_textbox_create(win_, kInnerW - 64);
    ui_widget_set_size(path_box_, kInnerW - 64, 24);
    ui_textbox_set_enter(path_box_, &FileManagerApp::onPathGo, this);
    ui_layout_add(path_row, path_box_);
    ui_widget_t* go = ui_button_create(win_, "Go");
    ui_widget_set_size(go, 58, 28);
    ui_button_set_click(go, &FileManagerApp::onPathGo, this);
    ui_layout_add(path_row, go);

    // Bar rename inline (hanya terlihat saat dipakai).
    rename_bar_ = ui_hbox_create(win_, 6);
    ui_widget_set_size(rename_bar_, kInnerW, kPathH);
    rename_label_ = ui_label_create(win_, "Rename:");
    ui_widget_set_size(rename_label_, 78, 16);
    ui_layout_add(rename_bar_, rename_label_);
    rename_box_ = ui_textbox_create(win_, kInnerW - 220);
    ui_widget_set_size(rename_box_, kInnerW - 220, 24);
    ui_textbox_set_enter(rename_box_, &FileManagerApp::onRenameOk, this);
    ui_layout_add(rename_bar_, rename_box_);
    ui_widget_t* ok = ui_button_create(win_, "OK");
    ui_widget_set_size(ok, 62, 28);
    ui_button_set_click(ok, &FileManagerApp::onRenameOk, this);
    ui_layout_add(rename_bar_, ok);
    ui_widget_t* cancel = ui_button_create(win_, "Cancel");
    ui_widget_set_size(cancel, 70, 28);
    ui_button_set_click(cancel, &FileManagerApp::onRenameCancel, this);
    ui_layout_add(rename_bar_, cancel);
    ui_widget_set_visible(rename_bar_, 0);

    // Isi: sidebar + (tabel | kisi).
    content_ = ui_hbox_create(win_, kSidebarGap);
    ui_widget_set_size(content_, kInnerW, kContentH);

    sidebar_ = ui_listview_create(win_, kSidebarW, kContentH);
    ui_widget_set_size(sidebar_, kSidebarW, kContentH);
    ui_listview_set_change(sidebar_, &FileManagerApp::onSidebarChanged, this);
    ui_layout_add(content_, sidebar_);

    table_ = ui_table_create(win_, kViewW, kContentH);
    ui_widget_set_size(table_, kViewW, kContentH);
    ui_table_add_column(table_, "Name", kViewW - 232);
    ui_table_add_column(table_, "Type", 150);
    ui_table_add_column(table_, "Size", 82);
    ui_table_set_change(table_, &FileManagerApp::onRowChanged, this);
    ui_table_set_empty_text(table_, "This folder is empty");
    ui_widget_set_right_click(table_, &FileManagerApp::onRightClick, this);
    ui_layout_add(content_, table_);

    grid_ = ui_gridview_create(win_, kViewW, kContentH);
    ui_widget_set_size(grid_, kViewW, kContentH);
    ui_gridview_set_cell(grid_, 112, 98, IconCache::kGridPx);
    ui_gridview_set_empty_text(grid_, "This folder is empty");
    ui_gridview_set_change(grid_, &FileManagerApp::onGridChanged, this);
    ui_gridview_set_activate(grid_, &FileManagerApp::onGridActivate, this);
    ui_widget_set_right_click(grid_, &FileManagerApp::onRightClick, this);
    ui_widget_set_visible(grid_, 0);
    ui_layout_add(content_, grid_);

    status_ = ui_statusbar_create(win_);
    ui_widget_set_size(status_, kInnerW, kStatusH);

    ui_widget_t* root = ui_vbox_create(win_, kSpacing);
    ui_layout_add(root, path_row);
    ui_layout_add(root, rename_bar_);
    ui_layout_add(root, content_);
    ui_layout_add(root, status_);
    ui_window_add(win_, root);
}

void FileManagerApp::buildContextMenus() {
    // Menu popup: dimiliki aplikasi, dibuat sekali, dipakai berulang.
    ctx_item_ = ui_menu_create(win_);
    ui_menu_add_item(ctx_item_, "Open", tOpen, this);
    ui_menu_add_sep(ctx_item_);
    ui_menu_add_item(ctx_item_, "Rename", tRename, this);
    ui_menu_add_item(ctx_item_, "Delete", tDelete, this);

    ctx_background_ = ui_menu_create(win_);
    ui_menu_add_item(ctx_background_, "New Folder", tNewFolder, this);
    ui_menu_add_item(ctx_background_, "New File", tNewFile, this);
    ui_menu_add_sep(ctx_background_);
    ui_menu_add_item(ctx_background_, "Refresh", tRefresh, this);
}

void FileManagerApp::collectShortcuts() {
    // Hanya direktori yang BENAR-BENAR ada (diverifikasi lewat adapter).
    const Shortcut candidates[] = {
        { "Home",      "/home/user",            false },
        { "Documents", "/home/user/Documents",  false },
        { "Downloads", "/home/user/Downloads",  false },
        { "Pictures",  "/home/user/Pictures",   false },
        { "Projects",  "/home/user/Projects",   false },
        { "Apps",      "/apps",                 true  },
        { "System",    "/system",               true  },
        { "Root",      "/",                     true  },
    };
    shortcuts_.clear();
    for (const Shortcut& s : candidates) {
        if (!FileSystem::isDirectory(s.path)) continue;
        shortcuts_.push_back(s);
        ui_listview_add_item(sidebar_, s.label);
    }
}

// ------------------------------------------------------------
// View: model → widget
// ------------------------------------------------------------
void FileManagerApp::reloadModel() {
    const std::string path = model_.path();
    if (!model_.load(path, kMaxRows))
        trace("readdir failed", path);
}

void FileManagerApp::reloadView(const char* keep_selected) {
    reloadModel();

    ui_table_clear(table_);
    ui_gridview_clear(grid_);
    fillTable();
    fillGrid();
    ui_textbox_set_text(path_box_, currentPath().c_str());

    const int keep = keep_selected ? model_.indexOf(keep_selected) : -1;
    setSelected(keep);        // -1 = tidak ada seleksi setelah refresh

    updateSidebarSelection();
    updateMenus();
    updateStatus();

    if (!model_.valid()) {
        error_title_ = "Could not open folder";
        error_text_ = currentPath();
        ui_dialog_show(win_, error_title_.c_str(),
                       "This folder could not be read.", kOkButtons, 1, nullptr, nullptr);
    }
}

void FileManagerApp::fillTable() {
    const std::vector<FileEntry>& entries = model_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const FileEntry& e = entries[i];
        char size[24];
        size[0] = '\0';
        if (e.kind == EntryKind::File && e.path_ok)
            media_format_size(e.size, size, (int)sizeof(size));
        const char* cells[3] = { e.name.c_str(), e.path_ok ? e.type_label.c_str()
                                                          : "Path too long",
                                 size };
        ui_table_add_row(table_, cells, 3);
        ui_table_set_row_icon(table_, (int)i, icons_.row(e.icon), IconCache::kRowPx,
                              IconCache::kRowPx);
    }
}

void FileManagerApp::fillGrid() {
    const std::vector<FileEntry>& entries = model_.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const FileEntry& e = entries[i];
        int index = ui_gridview_add_item(grid_, e.name.c_str());
        if (index < 0) continue;
        ui_gridview_set_thumb(grid_, index, icons_.grid(e.icon), IconCache::kGridPx,
                              IconCache::kGridPx);
        // set_thumb(px = 0) defaultnya placeholder LOADING; tidak ada yang
        // sedang dimuat di sini, jadi minta kotak kosong.
        if (!icons_.grid(e.icon))
            ui_gridview_set_placeholder(grid_, index, UI_GRID_PH_EMPTY);
    }
}

void FileManagerApp::setSelected(int index) {
    const int count = (int)model_.entries().size();
    selected_ = (index >= 0 && index < count) ? index : -1;
    ui_table_set_selected(table_, selected_);
    if (selected_ >= 0) ui_gridview_set_selected(grid_, selected_);
    updateStatus();
}

void FileManagerApp::updateStatus() {
    char right[200];
    right[0] = '\0';
    const std::vector<FileEntry>& entries = model_.entries();
    if (selected_ >= 0 && selected_ < (int)entries.size()) {
        const FileEntry& e = entries[selected_];
        std::string text = e.name;
        text += "  -  ";
        text += e.type_label;
        if (e.kind == EntryKind::File && e.path_ok) {
            char size[24];
            media_format_size(e.size, size, (int)sizeof(size));
            text += "  -  ";
            text += size;
        }
        toCStr(text, right, (int)sizeof(right));
    } else {
        const int folders = model_.folderCount();
        const int files = model_.fileCount();
        char num[16];
        std::string text;
        media_format_int(num, (int)sizeof(num), folders);
        text = num;
        text += (folders == 1) ? " folder, " : " folders, ";
        media_format_int(num, (int)sizeof(num), files);
        text += num;
        text += (files == 1) ? " file" : " files";
        if (model_.truncated()) {
            text += "  (showing first ";
            media_format_int(num, (int)sizeof(num), kMaxRows);
            text += num;
            text += ")";
        }
        toCStr(text, right, (int)sizeof(right));
    }
    ui_statusbar_set_text(status_, currentPath().c_str(), right);
}

void FileManagerApp::updateMenus() {
    ui_menu_set_checked(menu_view_, kViewItemList, view_ == ViewMode::List);
    ui_menu_set_checked(menu_view_, kViewItemIcons, view_ == ViewMode::Icon);
    ui_menu_set_checked(menu_view_, kViewItemSidebar, sidebar_visible_);
    const SortSpec& s = model_.sortSpec();
    ui_menu_set_checked(menu_view_, kViewItemSortName, s.key == SortKey::Name);
    ui_menu_set_checked(menu_view_, kViewItemSortType, s.key == SortKey::Type);
    ui_menu_set_checked(menu_view_, kViewItemSortSize, s.key == SortKey::Size);
    ui_menu_set_checked(menu_view_, kViewItemAsc, !s.descending);
    ui_menu_set_checked(menu_view_, kViewItemDesc, s.descending);
    ui_menu_set_enabled(menu_go_, kGoItemBack, history_.canGoBack());
    ui_menu_set_enabled(menu_go_, kGoItemForward, history_.canGoForward());
}

void FileManagerApp::updateSidebarSelection() {
    int sel = -1;
    for (std::size_t i = 0; i < shortcuts_.size(); ++i)
        if (samePath(shortcuts_[i].path, currentPath())) { sel = (int)i; break; }
    ui_listview_set_selected(sidebar_, sel);
}

void FileManagerApp::applyViewMode() {
    ui_widget_set_visible(table_, view_ == ViewMode::List);
    ui_widget_set_visible(grid_, view_ == ViewMode::Icon);
}

void FileManagerApp::applySidebarVisibility() {
    ui_widget_set_visible(sidebar_, sidebar_visible_);
}

// ------------------------------------------------------------
// Navigasi
// ------------------------------------------------------------
void FileManagerApp::releaseFocus() {
    // Toolkit hanya memanggil hook tombol aplikasi saat TIDAK ada widget
    // fokus; hook itu yang memegang F2/Delete/panah/Backspace. Bar rename
    // sengaja tidak memanggil ini (kolom nama harus tetap memegang fokus).
    if (rename_active_) return;
    ui_window_focus(win_, nullptr);
}

void FileManagerApp::navigateTo(const std::string& path, bool with_history) {
    if (path.empty()) return;
    if ((int)path.size() >= kPathMax) {
        error_title_ = "Could not open folder";
        ui_dialog_show(win_, error_title_.c_str(),
                       "This path is too long for the filesystem API.", kOkButtons, 1,
                       nullptr, nullptr);
        return;
    }
    if (!FileSystem::isDirectory(path)) {
        error_title_ = "Could not open folder";
        error_text_ = path;
        ui_dialog_show(win_, error_title_.c_str(),
                       FileSystem::isFile(path) ? "This path is a file, not a folder."
                                                : "This folder no longer exists.",
                       kOkButtons, 1, nullptr, nullptr);
        trace("navigate failed", path);
        return;
    }
    if (samePath(path, currentPath())) { reloadView(nullptr); return; }
    if (with_history) history_.record(currentPath());
    cancelRename();
    std::string target = path;
    model_.clear();
    model_.load(target, kMaxRows);
    reloadView(nullptr);
    trace("path", currentPath());
}

void FileManagerApp::cmdBack() {
    std::string target;
    if (!history_.back(currentPath(), target)) return;
    cancelRename();
    model_.load(target, kMaxRows);
    reloadView(nullptr);
    trace("path", currentPath());
}

void FileManagerApp::cmdForward() {
    std::string target;
    if (!history_.forward(currentPath(), target)) return;
    cancelRename();
    model_.load(target, kMaxRows);
    reloadView(nullptr);
    trace("path", currentPath());
}

void FileManagerApp::cmdUp() {
    const std::string parent = parentOf(currentPath());
    if (samePath(parent, currentPath())) return;      // sudah di akar
    navigateTo(parent, true);
}

void FileManagerApp::cmdRefresh() {
    releaseFocus();
    reloadView(nullptr);
}

void FileManagerApp::cmdClose() { ui_window_request_close(win_); }

void FileManagerApp::cmdOpenShortcut(int shortcut_index) {
    if (shortcut_index < 0 || shortcut_index >= (int)shortcuts_.size()) return;
    navigateTo(shortcuts_[shortcut_index].path, true);
}

bool FileManagerApp::childPath(int index, std::string& out) const {
    const std::vector<FileEntry>& entries = model_.entries();
    if (index < 0 || index >= (int)entries.size()) return false;
    if (!canOperate(entries[index])) return false;
    return FileSystem::join(currentPath(), entries[index].name, out);
}

void FileManagerApp::openEntry(int index) {
    const std::vector<FileEntry>& entries = model_.entries();
    if (index < 0 || index >= (int)entries.size()) return;
    const FileEntry& e = entries[index];
    std::string full;
    if (!childPath(index, full)) {
        error_title_ = "Could not open";
        ui_dialog_show(win_, error_title_.c_str(),
                       "This path is too long for the filesystem API.", kOkButtons, 1,
                       nullptr, nullptr);
        return;
    }
    if (e.kind == EntryKind::Dir) { navigateTo(full, true); return; }
    if (e.icon == IconKind::App) { launchApp(full); return; }
    if (e.icon == IconKind::Image) { launchImage(full); return; }
    if (e.icon == IconKind::Text) { launchText(full); return; }

    // Tidak ada handler: katakan apa adanya (tidak membangun registry app).
    error_title_ = "No application is associated with this file";
    error_text_ = e.name;
    ui_dialog_show(win_, error_title_.c_str(),
                   "KyuzenOS has no handler for this file type yet.", kOkButtons, 1,
                   nullptr, nullptr);
}

void FileManagerApp::cmdOpen() {
    releaseFocus();
    openEntry(selected_);
}

// ------------------------------------------------------------
// Operasi berkas
// ------------------------------------------------------------
void FileManagerApp::beginRename(const char* label, const std::string& initial,
                                 const std::string& from) {
    rename_from_ = from;
    ui_label_set_text(rename_label_, label);
    ui_textbox_set_text(rename_box_, initial.c_str());
    // Isi terpilih: ketikan pertama MENGGANTI nama (ganti-nama gaya Explorer).
    ui_textbox_select_all(rename_box_);
    ui_widget_set_visible(rename_bar_, 1);
    rename_active_ = true;
    ui_window_focus(win_, rename_box_);
    trace("rename bar open", initial);
}

void FileManagerApp::cancelRename() {
    if (!rename_active_) return;
    rename_active_ = false;
    rename_from_.clear();
    ui_widget_set_visible(rename_bar_, 0);
    ui_window_focus(win_, nullptr);
}

void FileManagerApp::commitRename() {
    const char* typed = ui_textbox_text(rename_box_);
    const std::string name = typed ? typed : "";
    trace("commit", name);

    if (name.empty()) {
        error_title_ = "Rename";
        ui_dialog_show(win_, error_title_.c_str(), "The name cannot be empty.",
                       kOkButtons, 1, nullptr, nullptr);
        return;
    }
    if (!FileSystem::validName(name)) {
        error_title_ = "Rename";
        error_text_ = name;
        ui_dialog_show(win_, error_title_.c_str(),
                       "A name cannot contain '/' and must not be empty.",
                       kOkButtons, 1, nullptr, nullptr);
        return;
    }
    if (rename_from_.empty() || name == rename_from_) { cancelRename(); return; }

    std::string from, to;
    if (!FileSystem::join(currentPath(), rename_from_, from) ||
        !FileSystem::join(currentPath(), name, to)) {
        error_title_ = "Could not rename";
        ui_dialog_show(win_, error_title_.c_str(),
                       "The resulting path is too long for the filesystem API.",
                       kOkButtons, 1, nullptr, nullptr);
        return;
    }
    OpResult r = FileSystem::rename(from, to);
    if (r != OpResult::Ok) {
        // Bar tetap terbuka supaya nama bisa diperbaiki; alasan ditampilkan.
        error_title_ = "Could not rename";
        error_text_ = name;
        ui_dialog_show(win_, error_title_.c_str(), describe(r), kOkButtons, 1,
                       nullptr, nullptr);
        trace("rename failed", from);
        return;
    }
    trace("rename ok", to);
    cancelRename();
    reloadView(name.c_str());
    toast("Renamed");
}

void FileManagerApp::cmdNewFolder() {
    releaseFocus();
    const std::string name = FileSystem::uniqueName(currentPath(), "New Folder", "");
    std::string full;
    if (!FileSystem::join(currentPath(), name, full)) {
        error_title_ = "Could not create folder";
        ui_dialog_show(win_, error_title_.c_str(),
                       describe(OpResult::PathTooLong), kOkButtons, 1, nullptr, nullptr);
        return;
    }
    OpResult r = FileSystem::makeDirectory(full);
    if (r != OpResult::Ok) {
        error_title_ = "Could not create folder";
        error_text_ = full;
        ui_dialog_show(win_, error_title_.c_str(), describe(r), kOkButtons, 1,
                       nullptr, nullptr);
        trace("mkdir failed", full);
        return;
    }
    trace("mkdir ok", full);
    reloadView(name.c_str());
    beginRename("New folder:", name, name);
}

void FileManagerApp::cmdNewFile() {
    releaseFocus();
    const std::string name = FileSystem::uniqueName(currentPath(), "New File", ".txt");
    std::string full;
    if (!FileSystem::join(currentPath(), name, full)) {
        error_title_ = "Could not create file";
        ui_dialog_show(win_, error_title_.c_str(),
                       describe(OpResult::PathTooLong), kOkButtons, 1, nullptr, nullptr);
        return;
    }
    OpResult r = FileSystem::createFile(full);
    if (r != OpResult::Ok) {
        error_title_ = "Could not create file";
        error_text_ = full;
        ui_dialog_show(win_, error_title_.c_str(), describe(r), kOkButtons, 1,
                       nullptr, nullptr);
        trace("create failed", full);
        return;
    }
    trace("create ok", full);
    reloadView(name.c_str());
    beginRename("New file:", name, name);
}

void FileManagerApp::cmdRename() {
    releaseFocus();
    const std::vector<FileEntry>& entries = model_.entries();
    if (selected_ < 0 || selected_ >= (int)entries.size()) {
        toast("Select an item first");
        return;
    }
    const FileEntry& e = entries[selected_];
    if (!canOperate(e)) {
        error_title_ = "Rename";
        ui_dialog_show(win_, error_title_.c_str(),
                       "This path is too long to operate on.", kOkButtons, 1, nullptr,
                       nullptr);
        return;
    }
    beginRename("Rename:", e.name, e.name);
}

void FileManagerApp::cmdDelete() {
    releaseFocus();
    const std::vector<FileEntry>& entries = model_.entries();
    if (selected_ < 0 || selected_ >= (int)entries.size()) {
        toast("Select an item first");
        return;
    }
    const FileEntry& e = entries[selected_];
    if (!canOperate(e)) {
        error_title_ = "Delete";
        ui_dialog_show(win_, error_title_.c_str(),
                       "This path is too long to operate on.", kOkButtons, 1, nullptr,
                       nullptr);
        return;
    }
    delete_target_ = e.name;
    std::string text = (e.kind == EntryKind::Dir) ? "Delete this folder?\n"
                                                 : "Delete this file?\n";
    text += e.name;
    error_title_ = (e.kind == EntryKind::Dir) ? "Delete folder" : "Delete file";
    error_text_ = text;
    ui_dialog_show(win_, error_title_.c_str(), error_text_.c_str(), kDeleteButtons, 2,
                   &FileManagerApp::onDeleteConfirmed, this);
}

void FileManagerApp::confirmDelete() {
    // Dipanggil dari dialog (index 0 = Delete). Menghapus folder tidak kosong
    // DITOLAK: filesystem tidak punya recursive delete, dan menambahkannya di
    // sini akan menyalahi semantik yang ada.
    std::string full;
    if (!FileSystem::join(currentPath(), delete_target_, full)) return;
    OpResult r = FileSystem::remove(full);
    if (r != OpResult::Ok) {
        showError("Could not delete", full, r);
        trace("delete failed", full);
        return;
    }
    trace("delete ok", full);
    delete_target_.clear();
    reloadView(nullptr);
    toast("Deleted");
}

void FileManagerApp::showError(const char* title, const std::string& subject, OpResult r) {
    error_title_ = title ? title : "Error";
    error_text_ = subject;
    ui_dialog_show(win_, error_title_.c_str(), describe(r), kOkButtons, 1, nullptr,
                   nullptr);
}

void FileManagerApp::toast(const char* text) { ui_window_notify(win_, text, 1400); }

void FileManagerApp::trace(const char* tag, const std::string& arg) const {
    print(const_cast<char*>("[filemanager] "));
    print(const_cast<char*>(tag));
    if (!arg.empty()) {
        print(const_cast<char*>(" "));
        print(const_cast<char*>(arg.c_str()));
    }
    print(const_cast<char*>("\n"));
}

// ------------------------------------------------------------
// Membuka berkas dengan aplikasi terkait
// ------------------------------------------------------------
void FileManagerApp::launchApp(const std::string& path) {
    std::string elf = "/apps/";
    elf += media_basename(path.c_str());
    char* argv[1];
    argv[0] = const_cast<char*>(path.c_str());
    if (sys_spawn_argv(const_cast<char*>(elf.c_str()), 1, argv) < 0) {
        error_title_ = "Could not open";
        error_text_ = elf;
        ui_dialog_show(win_, error_title_.c_str(), "The application could not be started.",
                       kOkButtons, 1, nullptr, nullptr);
        trace("spawn failed", elf);
    }
}

void FileManagerApp::launchImage(const std::string& path) {
    char* argv[2];
    argv[0] = const_cast<char*>("/apps/imageview.elf");
    argv[1] = const_cast<char*>(path.c_str());
    if (sys_spawn_argv(argv[0], 2, argv) < 0) {
        error_title_ = "Could not open image";
        error_text_ = path;
        ui_dialog_show(win_, error_title_.c_str(), "Image Viewer could not be started.",
                       kOkButtons, 1, nullptr, nullptr);
        trace("spawn imageview failed", path);
    }
}

void FileManagerApp::launchText(const std::string& path) {
    // Notepad membaca berkas awal dari "edit.tmp" (konvensi antar-app yang sudah
    // ada: isinya PATH berkas, bukan isi berkas).
    if (sys_file_exists(const_cast<char*>("edit.tmp")))
        fs_delete(const_cast<char*>("edit.tmp"));
    if (sys_create_file(const_cast<char*>("edit.tmp"), const_cast<char*>(path.c_str()),
                        (std::uint32_t)path.size()) != 1) {
        error_title_ = "Could not open file";
        error_text_ = path;
        ui_dialog_show(win_, error_title_.c_str(),
                       "Could not hand the file to the text editor.", kOkButtons, 1,
                       nullptr, nullptr);
        return;
    }
    ui_window_destroy(win_);
    win_ = nullptr;                 // window sudah dilepas: jangan dihancurkan lagi
    sys_exec(const_cast<char*>("/apps/notepad.elf"));   // tidak pernah kembali
}

// ------------------------------------------------------------
// Callback widget
// ------------------------------------------------------------
void FileManagerApp::onPathGo(void* ud) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    a->releaseFocus();
    const char* typed = ui_textbox_text(a->path_box_);
    a->navigateTo(typed ? typed : "", true);
}

void FileManagerApp::onRowChanged(void* ud) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    const int row = ui_table_selected(a->table_);
    const std::uint64_t now = sys_uptime();
    const bool dbl = (row >= 0 && row == a->last_click_row_ && row == a->selected_ &&
                      now - a->last_click_ms_ < kDoubleClickMs);
    a->last_click_ms_ = now;
    a->last_click_row_ = row;
    a->setSelected(row);
    if (dbl) a->openEntry(row);
}

void FileManagerApp::onGridChanged(void* ud) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    a->setSelected(ui_gridview_selected(a->grid_));
    a->releaseFocus();   // aplikasi yang menangani panah/F2/Delete di kedua mode
}

void FileManagerApp::onGridActivate(void* ud) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    a->openEntry(ui_gridview_selected(a->grid_));
}

void FileManagerApp::onSidebarChanged(void* ud) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    a->releaseFocus();
    const int i = ui_listview_selected(a->sidebar_);
    if (i < 0 || i >= (int)a->shortcuts_.size()) return;
    a->navigateTo(a->shortcuts_[i].path, true);
}

void FileManagerApp::onRightClick(void* ud, int x, int y) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    const int index = (a->view_ == ViewMode::List) ? ui_table_row_at(a->table_, y)
                                                   : ui_gridview_cell_at(a->grid_, x, y);
    if (index >= 0 && index < (int)a->model_.entries().size()) {
        a->setSelected(index);                       // klik kanan memilih entri itu
        ui_window_popup_menu(a->win_, a->ctx_item_, x, y);
    } else {
        a->setSelected(-1);                          // klik kanan di latar
        ui_window_popup_menu(a->win_, a->ctx_background_, x, y);
    }
}

void FileManagerApp::onAppKey(void* ud, std::uint32_t ascii, std::uint32_t scancode,
                              std::uint32_t mods) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);

    if (mods & KEY_MOD_CTRL) {
        switch (ascii) {
        case 'l': case 'L': ui_window_focus(a->win_, a->path_box_); return;
        case 'r': case 'R': a->cmdRefresh(); return;
        case 'n': case 'N': a->cmdNewFolder(); return;
        default: return;
        }
    }
    if (mods & KEY_MOD_ALT) {
        if (scancode == 0x14B) a->cmdBack();        // Alt+Left
        if (scancode == 0x14D) a->cmdForward();     // Alt+Right
        return;
    }
    switch (scancode) {
    case 0x01C: a->cmdOpen(); break;                // Enter
    case 0x00E: a->cmdUp(); break;                  // Backspace
    case 0x03C: a->cmdRename(); break;              // F2
    case 0x153: a->cmdDelete(); break;              // Delete (extended)
    case 0x148: case 0x14B: a->setSelected(a->selected_ - 1); break;   // Up/Left
    case 0x150: case 0x14D: a->setSelected(a->selected_ + 1); break;   // Down/Right
    case 0x147: a->setSelected(0); break;                              // Home
    case 0x14F: a->setSelected((int)a->model_.entries().size() - 1); break;  // End
    default:
        if (ascii >= 32) {
            // Ketikan printable yang sampai ke aplikasi = tidak ada widget input
            // yang memegang fokus (mis. bar rename belum terbuka) → dicatat.
            std::string one(1, (char)ascii);
            a->trace("printable without focus", one);
        }
        break;
    }
}

void FileManagerApp::onEscape(void* ud) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    if (a->rename_active_) { a->cancelRename(); return; }
    ui_window_request_close(a->win_);
}

void FileManagerApp::onRenameOk(void* ud) {
    static_cast<FileManagerApp*>(ud)->commitRename();
}

void FileManagerApp::onRenameCancel(void* ud) {
    static_cast<FileManagerApp*>(ud)->cancelRename();
}

void FileManagerApp::onDeleteConfirmed(void* ud, int index) {
    FileManagerApp* a = static_cast<FileManagerApp*>(ud);
    if (index != 0) return;          // Cancel / ESC
    a->confirmDelete();
}

void FileManagerApp::onGoShortcut(void* ud) {
    // userdata = GoBinding* (lihat buildMenus), bukan `this`.
    GoBinding* b = static_cast<GoBinding*>(ud);
    if (b && b->app) b->app->cmdOpenShortcut(b->shortcut_index);
}

// ------------------------------------------------------------
// Mode tampilan / urutan / sidebar
// ------------------------------------------------------------
void FileManagerApp::cmdViewMode(ViewMode mode) {
    trace("view", mode == ViewMode::Icon ? "icons" : "list");
    if (view_ == mode) return;
    releaseFocus();
    view_ = mode;
    applyViewMode();
    reloadView(nullptr);
}

void FileManagerApp::cmdToggleView() {
    cmdViewMode(view_ == ViewMode::List ? ViewMode::Icon : ViewMode::List);
}

void FileManagerApp::cmdToggleSidebar() {
    releaseFocus();
    sidebar_visible_ = !sidebar_visible_;
    applySidebarVisibility();
    updateMenus();
}

void FileManagerApp::cmdSort(SortKey key, bool descending) {
    releaseFocus();
    SortSpec spec = model_.sortSpec();
    spec.key = key;
    spec.descending = descending;
    model_.sort(spec);          // urutan hanya di model: tidak ada I/O
    setSelected(selected_);
    ui_table_clear(table_);
    ui_gridview_clear(grid_);
    fillTable();
    fillGrid();
    updateStatus();
    updateMenus();
}

}  // namespace fm
