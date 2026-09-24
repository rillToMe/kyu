// Kyuzen Desktop — launcher: discovery /apps + manifest .app + grid ikon.
//
// Kebijakan implementasi (bukan framework): scan FS tiap *.elf, enrich via
// "<base>.app" (name/color/hidden/icon), tanpa daftar hardcode. State eksplisit
// di objek (bukan global).
#ifndef KYUZEN_DESKTOP_IMPL_LAUNCHER_HPP
#define KYUZEN_DESKTOP_IMPL_LAUNCHER_HPP

#include <stdint.h>
#include <kyuzen/desktop/canvas.hpp>
#include <kyuzen/desktop/geometry.hpp>
#include <kyuzen/desktop/system.hpp>
#include "app_icons.hpp"

// Opaque libs/text (definisi penuh hanya di .cpp; global agar cocok
// dengan typedef struct kz_font di kzfont.h).
struct kz_font;

namespace desktop_impl {

using kyuzen::desktop::Canvas;
using kyuzen::desktop::Color;
using kyuzen::desktop::Point;
using kyuzen::desktop::Rect;

const int MAX_APPS = 32;

// Satu entri launcher: label tampil + path spawn + warna ikon +
// nama file ikon ("icon=" manifest; kosong = default terpusat).
struct AppEntry {
    char label[32];
    char elf[32];  // path lengkap "/apps/<nama>"
    char icon[ICON_NAME_MAX];  // nama berkas di akar FS, atau ""
    Color color;
};

// State visual box ikon (gaya Windows: hover terang, selected + outline).
// Nama berawalan ICON_ST_* agar tak bentrok warna ICON_HOVER/ICON_SEL.
enum IconState {
    ICON_ST_NORMAL = 0,
    ICON_ST_HOVER = 1,
    ICON_ST_SELECTED = 2,
};

// Satu ikon desktop siap gambar/hit: bounding box tetap (BOX_W x BOX_H)
// + state + koordinat grid. Diisi describe(); index = entri apps_.
struct DesktopIcon {
    Rect box;
    int state;
    int col;
    int row;
};

// "0x1565C0" (atau desimal) → Color opaque. Berhenti di karakter non-digit.
// Diekspos untuk host test (perilaku parse = kontrak).
Color parse_color(const char* s);

class Launcher {
public:
    Launcher();

    // Scan /apps; true = hasil beda dari sebelumnya (perlu render Full).
    bool discover();
    int count() const { return napps_; }
    const AppEntry& entry(int i) const { return apps_[i]; }

    // --- UI font (FreeType via libs/text; §3/§4/§19) ---
    // Default = Inter Regular (KZ_FONT_DEFAULT). Bitmap 8x16 tetap
    // fallback bila font tak termuat (draw() memilih jalur per kondisi,
    // pixel-identical dengan lama saat fallback).
    void ui_font_init();   // sekali di shell start (baca font.ui + muat)
    bool ui_font_poll();   // true = font.ui berubah -> reload (caller Full)
    int ui_font_id() const { return ui_font_id_; }

    // Geometri grid (murni, tanpa state layar): kolom yang muat di lebar w,
    // kapasitas ikon di atas taskbar untuk layar w×h.
    static int grid_cols(int w);
    static int grid_rows(int h);
    int grid_cap(int w, int h) const;
    static Rect icon_rect(int i, int cols);
    // Indeks ikon di titik p, atau -1 (di luar ikon / di luar kapasitas).
    int find_icon(Point p, int cols, int cap) const;

    // --- Grid engine kolom-mayor (default ala Windows: isi ke bawah dulu,
    // lalu kolom baru di kanan; berhenti di screen_height - taskbar_height).
    // box_grid_pos: col = i/rows, row = i%rows. box_rect = box 80x100 di
    // posisi itu, atau posisi kustom bila auto-arrange mati.
    static void box_grid_pos(int i, int cols, int rows, int* col, int* row);
    Rect box_rect(int i, int cols, int rows) const;
    // Deskripsi lengkap ikon i (box + state + grid) untuk gambar/hit.
    void describe(int i, int cols, int rows, DesktopIcon* out) const;
    // Hit-test sesi layar penuh (kolom-mayor + posisi kustom), atau -1.
    int find_icon_at(Point p, int w, int h) const;
    // Bungkus label jadi maks 2 baris selebar box (word-wrap spasi; kata
    // tanpa spasi dipotong keras). "..." HANYA bila baris2 masih luber.
    // l1/l2 NUL-terminated (cap termasuk NUL); l2 kosong = 1 baris.
    // Return jumlah baris (1 atau 2).
    static int wrap_label(const char* src, char* l1, char* l2, int cap);

    // Path ikon FS untuk entri i (resolusi terpusat: kustom -> default).
    // Buffer milik pemanggil (32 byte). Selalu NUL-terminated.
    void icon_path(int i, char* out) const;

    // Entri yang judul/nama-elf-nya cocok (untuk ikon slot taskbar).
    // Cocok label manifest dulu, lalu basename elf tanpa ".elf".
    // null = tak dikenal -> ikon default.
    const AppEntry* find_by_title(const char* title) const;

    // Gambar ikon (pixel PNG, RLE ke canvas window) + label tiap sel;
    // entri tanpa gambar digambar kotak warna manifest. w×h = ukuran layar
    // (eksplisit, sama seperti grid_cap; host test bisa menguji tanpa window).
    // Return jumlah ikon bergambar yang digambar (diagnostik serial).
    int draw(Canvas& canvas, const IconCache& icons, int w, int h) const;

    // --- State visual + mode layout (milik shell via setter) ---
    int hovered() const { return hover_; }
    int selected() const { return selected_; }
    void set_hover(int i) { hover_ = i; }
    void set_selected(int i) { selected_ = i; }
    // Auto-arrange (default true = grid kolom-mayor). Mati = posisi kustom
    // (free drag) dipakai; menyalakan lagi membuang posisi kustom.
    bool auto_arrange() const { return auto_arrange_; }
    void set_auto_arrange(bool on);
    void set_custom_pos(int i, int x, int y);  // no-op bila i tak valid
    void clear_custom();
    void sort_by_name();  // urut label A-Z; buang seleksi + posisi kustom

private:
    AppEntry apps_[MAX_APPS];
    int napps_;
    unsigned checksum_;
    int hover_;     // indeks hover, atau -1 (box highlight)
    int selected_;  // indeks selected, atau -1 (klik kiri)
    bool auto_arrange_;         // true = grid kolom-mayor (default)
    bool custom_[MAX_APPS];     // posisi kustom valid (mode free drag)
    int pos_x_[MAX_APPS];       // sudut box kustom (koordinat layar)
    int pos_y_[MAX_APPS];
    // State font UI (opaque: kz_font_t hanya dikenal .cpp agar header
    // tetap ringan; lifetime = Launcher).
    int ui_font_id_;
    char* ui_font_data_;  // blob TTF (sys_alloc; 0 bila tak termuat)
    uint32_t ui_font_size_;
    ::kz_font* ui_font_;
    // Muat font id (destroy state lama dulu). Invalid -> default.
    void ui_font_load(int id);
    void ui_font_free();
    int ui_font_read_id();
    // Label FreeType + shadow (false = fallback bitmap). Definisi di .cpp
    // (butuh kzfont.h, hanya di sana agar header tetap ringan).
    // dy = offset baris dari atas box (BOX_LBL_DY / +LINE_H).
    bool draw_ft_label(Canvas& canvas, const Rect& b, const char* line,
                       int dy) const;
};

}  // namespace desktop_impl

#endif
