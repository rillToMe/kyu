// user_apps/filemanager/app.hpp — aplikasi File Manager (kelas utama, pola
// gal::Gallery: satu kelas app dengan perintah publik + state privat).
//
// Pembagian lapisan (lihat file masing-masing):
//   platform.hpp — header platform C (extern "C")
//   fs.*         — adapter filesystem: SATU-SATUNYA pemakai syscall FS
//   model.*      — model direktori + riwayat navigasi + label/ikon entri (tanpa UI)
//   icons.*      — cache ikon tipe (decode sekali, dua ukuran)
//   app.*        — lapisan UI/state: widget libui, perintah, dialog, keyboard
//
// UI tidak pernah memanggil syscall dan tidak pernah menyusun state filesystem
// sendiri: setiap perubahan lewat model/adapter, lalu view dibangun ulang dari
// snapshot (satu arah: filesystem → model → widget).
#ifndef FM_APP_HPP
#define FM_APP_HPP

#include <string>
#include <vector>

#include "icons.hpp"
#include "model.hpp"
#include "platform.hpp"

namespace fm {

// Hanya direktori yang benar-benar ada yang muncul di sidebar (diverifikasi
// lewat adapter, bukan daftar hardcoded).
struct Shortcut {
    const char* label;
    const char* path;
    bool is_system;
};

class FileManagerApp {
public:
    FileManagerApp() = default;
    ~FileManagerApp();

    // Jalankan aplikasi. `initial` = direktori awal (argv[1]), nullptr = "/".
    int run(const char* initial);

    // --- perintah (dipakai menu/toolbar/shortcut lewat thunk statis) --------
    void cmdBack();
    void cmdForward();
    void cmdUp();
    void cmdRefresh();
    void cmdOpen();
    void cmdNewFolder();
    void cmdNewFile();
    void cmdRename();
    void cmdDelete();
    void cmdClose();
    void cmdViewMode(ViewMode mode);
    void cmdToggleView();
    void cmdToggleSidebar();
    void cmdSort(SortKey key, bool descending);
    void cmdOpenShortcut(int shortcut_index);

private:
    // --- state ------------------------------------------------------------
    ui_window_t* win_ = nullptr;
    ui_widget_t* menubar_ = nullptr;
    ui_widget_t* menu_file_ = nullptr;
    ui_widget_t* menu_view_ = nullptr;
    ui_widget_t* menu_go_ = nullptr;
    ui_widget_t* toolbar_ = nullptr;
    ui_widget_t* path_box_ = nullptr;
    ui_widget_t* rename_bar_ = nullptr;
    ui_widget_t* rename_label_ = nullptr;
    ui_widget_t* rename_box_ = nullptr;
    ui_widget_t* content_ = nullptr;
    ui_widget_t* sidebar_ = nullptr;
    ui_widget_t* table_ = nullptr;
    ui_widget_t* grid_ = nullptr;
    ui_widget_t* status_ = nullptr;
    ui_widget_t* ctx_item_ = nullptr;
    ui_widget_t* ctx_background_ = nullptr;

    DirectoryModel model_;
    PathHistory history_;
    IconCache icons_;
    std::vector<Shortcut> shortcuts_;
    ViewMode view_ = ViewMode::List;
    bool sidebar_visible_ = true;
    int selected_ = -1;

    // Item menu Go memakai userdata = POINTER BINDING (bukan `this`) karena
    // satu callback melayani banyak item; binding dimiliki app dan alamatnya
    // stabil (di-reserve sebelum push_back) selama menu hidup.
    struct GoBinding {
        FileManagerApp* app;
        int shortcut_index;
    };
    std::vector<GoBinding> go_bindings_;

    // Bar rename inline (New Folder/New File/Rename): `from_` kosong = belum ada.
    bool rename_active_ = false;
    std::string rename_from_;
    std::string delete_target_;
    std::string error_title_;
    std::string error_text_;

    // Klik-ganda di list view (GridView sudah menangani miliknya sendiri).
    std::uint64_t last_click_ms_ = 0;
    int last_click_row_ = -1;

    // --- view -------------------------------------------------------------
    void buildMenus();
    void buildToolbar();
    void buildContent();
    void buildContextMenus();
    void collectShortcuts();
    void reloadView(const char* keep_selected);
    void fillTable();
    void fillGrid();
    void applyViewMode();
    void applySidebarVisibility();
    void updateStatus();
    void updateMenus();
    void updateSidebarSelection();
    void setSelected(int index);

    // --- navigasi ---------------------------------------------------------
    void navigateTo(const std::string& path, bool with_history);
    void reloadModel();
    std::string currentPath() const { return model_.path(); }
    bool childPath(int index, std::string& out) const;
    void openEntry(int index);
    void releaseFocus();

    // --- operasi berkas ---------------------------------------------------
    void beginRename(const char* label, const std::string& initial, const std::string& from);
    void cancelRename();
    void commitRename();
    void confirmDelete();
    void showError(const char* title, const std::string& subject, OpResult r);
    void toast(const char* text);
    void launchImage(const std::string& path);
    void launchText(const std::string& path);
    void launchApp(const std::string& path);
    void trace(const char* tag, const std::string& arg) const;

    // --- callback widget (thunk statis → method) --------------------------
    static FileManagerApp* self(void* ud) { return static_cast<FileManagerApp*>(ud); }
    static void onPathGo(void* ud);
    static void onRowChanged(void* ud);
    static void onGridChanged(void* ud);
    static void onGridActivate(void* ud);
    static void onSidebarChanged(void* ud);
    static void onRightClick(void* ud, int x, int y);
    static void onAppKey(void* ud, std::uint32_t ascii, std::uint32_t scancode,
                         std::uint32_t mods);
    static void onEscape(void* ud);
    static void onRenameOk(void* ud);
    static void onRenameCancel(void* ud);
    static void onDeleteConfirmed(void* ud, int index);
    static void onGoShortcut(void* ud);
};

}  // namespace fm

#endif // FM_APP_HPP
