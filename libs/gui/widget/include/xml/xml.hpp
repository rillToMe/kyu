// libs/widget/include/xml/xml.hpp — representasi + parser + inflater XML (Phase E).
//
// LAPISAN DAUN: hanya dikonsumsi abi C (libui_xml_*); tak ada widget yang
// meng-include ini. Parser (xml_parser.cpp) freestanding murni: tanpa
// widget/window include, tanpa libc — hanya runtime/memory.hpp (_ui_alloc/
// _ui_free/_ui_strlen/_ui_strncmp/_ui_strdup) + helper lokal.
//
// Representasi SENGAJA tanpa field teks (beda dari sketsa awal XML): teks
// non-whitespace di posisi mana pun adalah error (UI dari atribut saja),
// jadi tak ada yang perlu disimpan.
#ifndef KWIDGET_XML_XML_HPP
#define KWIDGET_XML_XML_HPP

#include "runtime/memory.hpp"

namespace ui {

// Forward (definisi lengkap milik lapisan widget; xml hanya menunjuk).
class Widget;
class Window;
class RadioGroup;

namespace xml {

// Batas = cermin enum UI_XML_MAX_* di libui_xml.h (satu sumber angka di sini;
// header C hanya dokumentasi).
enum {
    MAX_DOC = 65536,
    MAX_DEPTH = 16,
    MAX_NODES = 512,
    MAX_ATTRS = 16,
    MAX_CHILDREN = 64,
    MAX_ATTR_NAME = 32,
    MAX_ATTR_VALUE = 256,
    MAX_ID = 48,
    // 64, bukan 32: satu chrome aplikasi nyata menghabiskan id dengan cepat —
    // menubar dengan 3 menu (7+8+3 item ber-id) + 6 tombol toolbar + statusbar
    // + baris path + bar rename + sidebar + slot isi sudah melewati 32. Batas
    // lama berasal dari masa toolkit hanya punya form sederhana; efeknya
    // dokumen yang sepenuhnya wajar ditolak dengan UI_XML_LIMIT. Biaya
    // menaikkannya kecil: Context dialokasikan sekali per window.
    MAX_IDS = 64,
    MAX_GROUPS = 8,
    MAX_ROOTS = 16
};

// Kode error = cermin enum UI_XML_* di libui_xml.h (urutan SAMA persis).
enum {
    OK = 0, OOM, TOO_BIG, TOO_DEEP, TOO_MANY_NODES, TOO_MANY_ATTRS,
    TOO_MANY_CHILDREN, ATTR_TOO_LONG, SYNTAX, MISMATCH, UNCLOSED, BAD_ENTITY,
    UNSUPPORTED, EMPTY, MULTI_ROOT, BAD_TEXT, UNKNOWN_ELEMENT,
    UNKNOWN_ATTRIBUTE, MISSING_ATTRIBUTE, INVALID_VALUE, BAD_CHILD,
    BAD_PLACEMENT, DUP_ID, LIMIT
};

struct Error {
    int code, line, column;
    char element[48];
    char attribute[40];
};

struct Attr { char* name; char* value; };

struct Node {
    char* name;
    Attr* attrs;
    int nattr;
    Node** children;
    int nchild;
    int line, column;   // posisi '<' pembuka (1-based)
};

struct Document { Node* root; };

// Error ringan tanpa alokasi (string disalin pemanggil ke ui_xml_error_t).
void set_error(Error* e, int code, int line, int col,
               const char* elem, const char* attr);

// --- Parser (xml_parser.cpp) ---
// data boleh tak NUL-terminated; size eksplisit. Return 0 bila gagal.
Document* parse(const char* data, unsigned size, Error* err);
void free_doc(Document* doc);

// --- Inflater (xml_inflate.cpp) ---
// Context = milik pemanggil C; hidup selama window hasil inflasi.
// Kind widget untuk binding bertipe (RTTI mati; kind dicatat saat inflasi).
// NILAI LAMA TIDAK BOLEH BERGESER (dipakai binding bertipe oleh app):
// tambahkan kind baru di AKHIR sebelum K_KIND_MAX.
enum {
    K_LABEL = 1, K_BUTTON, K_TEXTBOX, K_CHECKBOX, K_RADIO, K_COMBO,
    K_SLIDER, K_PROGRESS, K_SEPARATOR, K_IMAGE, K_LISTVIEW, K_TAB,
    K_SCROLLVIEW, K_VBOX, K_HBOX, K_GRID,
    // Design system: elemen struktural & primitif baru.
    K_SECTION, K_SWITCH, K_ICON,
    // Chrome (dipasang lewat ui_window_add_bar, bukan ke layout root).
    K_MENUBAR, K_MENU, K_TOOLBAR, K_STATUSBAR,
    K_KIND_MAX
};

// Peran penempatan elemen hasil inflasi. Inflater mencatatnya supaya `commit()`
// bisa memasang widget ke tempat yang BENAR tanpa app perlu tahu:
//   ROOT   — masuk layout root window (ui_window_add), perilaku lama
//   BAR    — pita chrome di PUNCAK window (ui_window_add_bar)
//   BBAR   — pita chrome di DASAR window (ui_window_add_bottom_bar)
// Elemen chrome otomatis berperan BAR/BBAR: menubar+toolbar ke puncak,
// statusbar ke dasar. Ini yang membuat deklarasi chrome mungkin di XML tanpa
// menambah API penempatan baru di sisi aplikasi.
enum { PLACE_ROOT = 0, PLACE_BAR = 1, PLACE_BBAR = 2 };

// Satu id yang terdaftar. `w` = widget pemiliknya; `index` = nomor baris/item
// DI DALAM widget itu untuk elemen yang bukan Widget sendiri (item menu,
// tombol toolbar). Untuk widget biasa `index` = -1.
struct IdEntry {
    char id[MAX_ID + 1];
    Widget* w;
    int kind;
    int index;
};
struct GroupEntry { char name[32]; RadioGroup* g; };
struct Context {
    Window* win;
    IdEntry ids[MAX_IDS];
    int nids;
    GroupEntry groups[MAX_GROUPS];
    int ngroups;
    Widget* roots[MAX_ROOTS];   // detached sampai commit
    int nroots;
    // Peran penempatan per root (PLACE_ROOT / PLACE_BAR). Diisi saat inflasi
    // elemen root; dipakai commit() supaya chrome (menubar/toolbar/statusbar)
    // dipasang lewat add_bar dan sisanya lewat add.
    int root_place[MAX_ROOTS];
    bool committed;
    // Tema window (diterapkan saat commit; has_theme = ada atribut tema).
    bool has_theme;
    int theme_mode, theme_accent;     // nilai enum UI_THEME_*/UI_ACCENT_*
    unsigned theme_custom;            // 0xRRGGBB bila accent == CUSTOM
};

// Bangun pohon native DETACHED dari doc (validasi penuh). 1 = sukses
// (roots terisi, siap commit atau diambil caller); 0 = gagal (rollback
// internal: tak ada widget tersisa).
int inflate(Context* ctx, const Document* doc, Error* err);
int inflate(Context* ctx, const Document* doc, Error* err);
// Pasang roots + tema ke window (hanya setelah inflate sukses).
void commit(Context* ctx);
// Buang roots detached + grup (jalur gagal ATAU dtor konteks).
// Urutan: roots dulu (dtor radio keluar grup), lalu grup (kosong).
void rollback(Context* ctx);
// Cari widget ber-id dalam konteks (0 bila tak ada).
Widget* find(Context* ctx, const char* id);
// Registrasi id (cek duplikat + batas). 1 = ok.
int add_id(Context* ctx, const char* id, Widget* w, Error* err);
// Registrasi id untuk elemen yang BUKAN Widget: satu baris di dalam Menu
// (item menu) atau satu tombol di dalam Toolbar. `kind` diisi pemanggil
// (K_MENU / K_TOOLBAR) dan `index` = nomor baris/tombol. Binding memakai ini
// supaya aplikasi menyebut item dengan NAMA, bukan posisi.
int add_menu_item_id(Context* ctx, const char* id, Widget* owner, int index,
                     Error* err);
// Grup radio bernama (buat bila belum ada; batas MAX_GROUPS). 0 = penuh.
RadioGroup* group(Context* ctx, const char* name, Error* err);

} // namespace xml
} // namespace ui

#endif // KWIDGET_XML_XML_HPP
