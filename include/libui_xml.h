#ifndef LIBUI_XML_H
#define LIBUI_XML_H

#include <stdint.h>
#include "libui.h"   // ui_window_t/ui_widget_t/ui_click_cb (inflater = konsumen libui)

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// libui_xml — XML deklaratif -> widget/libui NATIVE (Phase E)
//
//   XML -> parse -> validasi -> inflater -> API libui biasa
//
// BUKAN framework kedua: tak ada layout engine, tema, CSS, scripting,
// XPath/DTD/entities. Parser freestanding (tanpa dep eksternal), batas
// deterministik, error berposisi baris/kolom. Inflasi TRANSaksional:
// gagal di tengah -> semua widget yang dibuat dihancurkan, window
// tak tersentuh. Lihat docs/design/gui/ui-xml-phase-e.md.
//
// PEMBAGIAN TANGGUNG JAWAB (aturan desain, bukan sekadar konvensi):
//   XML memuat STRUKTUR dan SEMANTIK: elemen apa, urutan apa, teks apa,
//   peran apa ("primary", "chevron", "settings").
//   TEMA memuat TAMPILAN: warna, radius, jarak, ukuran, tipografi.
// Karena itu TIDAK ADA atribut `color=`, `radius=`, `padding=`, `shadow=`.
// Kalau sebuah nilai visual tidak bisa diambil dari tema, itu tanda tokennya
// perlu ditambah di libui — bukan tanda XML perlu atribut baru.
//
// Atribut yang memang SEMANTIK (bukan visual) dan karena itu ada:
//   variant="primary|secondary|tertiary|danger"  — peran aksi
//   icon="settings"                              — nama ikon semantik
//   spacing="xs|sm|md|lg|xl"                     — jarak antar anak
//   size="sm|md|lg|xl"                           — ukuran optik ikon
//   theme-mode/theme-accent                      — pilihan tema pengguna
// ============================================================

typedef struct ui_xml_doc ui_xml_doc_t;   // pohon XML hasil parse (milik pemanggil)
typedef struct ui_xml_ctx ui_xml_ctx_t;   // konteks inflasi (ID map + grup radio)

// --- Error ---
// err boleh 0 (tak ada detail). Kode 0 = sukses. line/column 1-based;
// element/attribute = nama terkait ("" bila tak ada), disalin toolkit.
enum {
    UI_XML_OK = 0,
    UI_XML_OOM,                // alokasi gagal
    UI_XML_TOO_BIG,            // dokumen > UI_XML_MAX_DOC
    UI_XML_TOO_DEEP,           // kedalaman > UI_XML_MAX_DEPTH
    UI_XML_TOO_MANY_NODES,     // node > UI_XML_MAX_NODES
    UI_XML_TOO_MANY_ATTRS,     // atribut/elemen > UI_XML_MAX_ATTRS
    UI_XML_TOO_MANY_CHILDREN,  // anak/elemen > UI_XML_MAX_CHILDREN
    UI_XML_ATTR_TOO_LONG,      // nama/nilai atribut melebihi batas
    UI_XML_SYNTAX,             // tag/quote/komentar/deklarasi rusak
    UI_XML_MISMATCH,           // tutup tak cocok dengan buka
    UI_XML_UNCLOSED,           // EOF sebelum semua elemen tertutup
    UI_XML_BAD_ENTITY,         // entity tak dikenal / '&' mentah
    UI_XML_UNSUPPORTED,        // CDATA/DOCTYPE/PI fitluar subset
    UI_XML_EMPTY,              // dokumen kosong / tanpa akar
    UI_XML_MULTI_ROOT,         // >1 elemen akar
    UI_XML_BAD_TEXT,           // teks non-whitespace di posisi tak didukung
    UI_XML_UNKNOWN_ELEMENT,    // nama elemen di luar skema
    UI_XML_UNKNOWN_ATTRIBUTE,  // atribut di luar skema elemen itu
    UI_XML_MISSING_ATTRIBUTE,  // atribut wajib tak ada
    UI_XML_INVALID_VALUE,      // nilai di luar domain (int/bool/enum/warna)
    UI_XML_BAD_CHILD,          // anak tak legal untuk induk ini
    UI_XML_BAD_PLACEMENT,      // row/col grid hilang/di luar/overlap/penuh
    UI_XML_DUP_ID,             // id dipakai dua kali dalam satu konteks
    UI_XML_LIMIT               // batas konteks (ID/grup/radio/akar) terlampaui
};

// Batas parser (lihat ui-xml-phase-e.md § limits — dari kendala repo,
// bukan angka arbitrer: Layout 16 anak, Grid 32 sel, ComboBox 16 item).
enum {
    UI_XML_MAX_DOC = 65536,      // 64K — layar settings muat <4K
    UI_XML_MAX_DEPTH = 16,       // window>vbox>tab>page>vbox>grid>... tak sedalam ini
    UI_XML_MAX_NODES = 512,      // 16 layout × 16 anak × 2 lapis masih muat
    UI_XML_MAX_ATTRS = 16,       // elemen terpadat (grid) <12 atribut
    UI_XML_MAX_CHILDREN = 64,    // parser longgar; batas native (16/32/8) di inflater
    UI_XML_MAX_ATTR_NAME = 32,   // atribut terpanjang "theme-custom" (12)
    UI_XML_MAX_ATTR_VALUE = 256, // TextBox MAX_TEXT=256
    UI_XML_MAX_ID = 48,
    UI_XML_MAX_IDS = 32,         // ID per konteks
    UI_XML_MAX_GROUPS = 8,       // grup radio bernama per konteks
    UI_XML_MAX_ROOTS = 16        // anak teratas per window (root VBox menampung 16)
};

typedef struct ui_xml_error {
    int code;                  // UI_XML_* (0 = sukses)
    int line, column;          // 1-based; 0 bila tak terkait posisi
    char element[48];          // elemen terkait ("" bila tak ada)
    char attribute[40];        // atribut terkait ("" bila tak ada)
} ui_xml_error_t;

// --- Parse ---
// data TIDAK perlu NUL-terminated (size eksplisit). Return 0 bila gagal
// (detail di err). Dokumen memiliki seluruh string/node; hancurkan pakai
// ui_xml_doc_destroy. UI hasil inflasi TAK bergantung pada dokumen.
ui_xml_doc_t* ui_xml_parse(const char* data, unsigned size, ui_xml_error_t* err);
void ui_xml_doc_destroy(ui_xml_doc_t* doc);

// --- Konteks inflasi ---
// Satu konteks = satu hasil inflasi (ID + grup radio milik konteks).
// Hancurkan SETELAH window dihancurkan (grup radio dilepas aman dua arah,
// tapi urutan ini yang terdokumentasi). Tanpa global state.
ui_xml_ctx_t* ui_xml_ctx_create(ui_window_t* win);
void ui_xml_ctx_destroy(ui_xml_ctx_t* ctx);

// --- Inflasi ---
// Bangun widget native dari dokumen ke window konteks (transaksional:
// 1 = seluruh pohon terpasang; 0 = gagal, window tak berubah, tak ada
// widget tersisa). Dokumen boleh dihancurkan setelah inflasi.
int ui_xml_inflate(ui_xml_ctx_t* ctx, const ui_xml_doc_t* doc, ui_xml_error_t* err);

// Varian DETACHED untuk konsumen yang me-parenting sendiri (mis. page yang
// dimasukkan ke layout lain): widget TIDAK dipasang ke window; caller
// mengambil alih via ui_xml_root_* lalu ui_xml_release (wajib, kalau tidak
// ctx_destroy akan menghapus roots). ID/groups tetap milik konteks.
int ui_xml_inflate_detached(ui_xml_ctx_t* ctx, const ui_xml_doc_t* doc,
                            ui_xml_error_t* err);
int ui_xml_root_count(ui_xml_ctx_t* ctx);
ui_widget_t* ui_xml_root_at(ui_xml_ctx_t* ctx, int index);   // 0 bila di luar
void ui_xml_release(ui_xml_ctx_t* ctx);   // roots jadi milik caller

// --- Lookup + binding ---
// Cari widget hasil inflasi ber-id (0 bila tak ada). Binding callback
// TANPA kode di XML: pemanggil C yang mengikat fungsi native.
// event: UI_XML_ON_CLICK / UI_XML_ON_CHANGE (pemetaan per-tipe widget
// terdokumentasi; pasangan tak didukung -> 0).
enum { UI_XML_ON_CLICK = 0, UI_XML_ON_CHANGE = 1 };
ui_widget_t* ui_xml_find(ui_xml_ctx_t* ctx, const char* id);
int ui_xml_bind(ui_xml_ctx_t* ctx, const char* id, int event,
                ui_click_cb cb, void* userdata);   // 1 = terikat

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBUI_XML_H
