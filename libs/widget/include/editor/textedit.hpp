// libs/widget/include/editor/textedit.hpp — dipindah apa adanya dari apps/libui.cpp.
#ifndef KWIDGET_EDITOR_TEXTEDIT_HPP
#define KWIDGET_EDITOR_TEXTEDIT_HPP

#include "core/widget.hpp"
#include "core/painter.hpp"
#include "runtime/memory.hpp"

namespace ui {

// ------------------------------------------------------------
// TextEdit — editor multi-baris (Phase 10). Buffer teks polos 8K,
// kursor + scroll roda/otomatis, opsional readonly (terminal output).
// Font 8x16: 8px/kolom, 16px/baris.
// ------------------------------------------------------------
class TextEdit : public Widget {
public:
    enum { MAX_TEXT = 8192, LINE_H = 16, CHAR_W = 8 };
    char text[MAX_TEXT];
    int len;
    int cur;
    int scroll_top;    // baris pertama yang tampak
    bool readonly;
    ui_click_cb enter_cb;   // terminal: Enter diserahkan ke app (tanpa sisip '\n')
    void* enter_data;

    // Terminal: prefix prompt berwarna (gaya shell Linux). Baris yang DIAWALI
    // `ps1` digambar dengan `ps1_len` karakter pertama memakai `ps1_color`,
    // sisanya theme.fg. ps1_len == 0 = fitur mati (notepad & co).
    char ps1[32];
    int ps1_len;
    color_t ps1_color;
    // --- Phase 11: seleksi, clipboard, undo/redo, word wrap (notepad) ---
    int sel_anchor;            // jangkar seleksi (-1 = tak ada). cur = ujung lain
    bool wrap;                 // word wrap aktif? (terminal: false)
    ui_click_cb change_cb;     // dipanggil setiap isi teks berubah (modified flag)
    void* change_data;
    // Undo/redo berbasis OPERASI (bukan snapshot penuh): tiap perubahan teks
    // menyimpan bagian yang dihapus + yang disisipkan, jadi mengetik 60 karakter
    // tetap 60 langkah undo tanpa menyalin buffer 8K per huruf.
    struct EditOp {
        int pos;               // titik perubahan (indeks dokumen)
        int del_len; char* del;// teks yang DIHAPUS (untuk undo)
        int ins_len; char* ins;// teks yang DISISIPKAN (untuk redo)
    };
    enum { MAX_OPS = 64 };
    EditOp ops[MAX_OPS];
    int n_ops;                 // jumlah op terpakai
    int op_pos;                // jumlah op yang "sudah dilakukan" (posisi undo)
    bool undo_on;              // app meminta undo/redo (notepad: ya)

    TextEdit(int width, int height) : len(0), cur(0), scroll_top(0),
                                      readonly(false), enter_cb(0), enter_data(0),
                                      ps1_len(0), ps1_color(COLOR_RGB(0x7C, 0xC7, 0xFF)),
                                      sel_anchor(-1), wrap(false),
                                      change_cb(0), change_data(0),
                                      n_ops(0), op_pos(0), undo_on(false) {
        w = width; h = height;
        text[0] = '\0';
        cursor_kind = UI_CURSOR_IBEAM;
    }
    virtual ~TextEdit() { ops_clear(); }
    // Bebaskan semua memori op (undo/redo) — juga saat window ditutup.
    void ops_clear() {
        for (int i = 0; i < n_ops; i++) {
            _ui_free(ops[i].del); _ui_free(ops[i].ins);
            ops[i].del = ops[i].ins = 0;
        }
        n_ops = 0; op_pos = 0;
    }
    static char* dup_n(const char* s, int n) {
        char* c = (char*)_ui_alloc((unsigned)(n > 0 ? n : 1) + 1u);
        if (!c) return 0;
        for (int i = 0; i < n; i++) c[i] = s[i];
        c[n] = '\0';
        return c;
    }
    void op_clear_range(int from) {              // buang op dari indeks `from`
        for (int i = from; i < n_ops; i++) {
            _ui_free(ops[i].del); _ui_free(ops[i].ins);
            ops[i].del = ops[i].ins = 0;
        }
        if (n_ops > from) n_ops = from;
        if (op_pos > n_ops) op_pos = n_ops;
    }
    void op_record(int pos, const char* del, int del_len, const char* ins, int ins_len) {
        if (!undo_on) return;
        op_clear_range(op_pos);               // cabang redo lama dibuang
        if (n_ops >= MAX_OPS) {               // penuh: buang op paling tua
            _ui_free(ops[0].del); _ui_free(ops[0].ins);
            for (int i = 1; i < n_ops; i++) ops[i - 1] = ops[i];
            n_ops--; op_pos--;
            if (op_pos < 0) op_pos = 0;
        }
        EditOp& o = ops[n_ops];
        o.pos = pos; o.del_len = del_len; o.ins_len = ins_len;
        o.del = del_len > 0 ? dup_n(del, del_len) : 0;
        o.ins = ins_len > 0 ? dup_n(ins, ins_len) : 0;
        n_ops++;
        op_pos = n_ops;
    }
    // Terapkan perubahan operasi (dir = -1 undo, +1 redo).
    bool op_apply(int dir) {
        if (dir < 0) {
            if (op_pos <= 0) return false;
            op_pos--;
        } else {
            if (op_pos >= n_ops) return false;
        }
        EditOp& o = ops[op_pos];
        // undo: buang yang tadinya disisipkan, lalu kembalikan yang terhapus.
        // redo: buang yang tadinya terhapus, lalu sisipkan lagi.
        int d_len = (dir < 0) ? o.ins_len : o.del_len;
        int i_len = (dir < 0) ? o.del_len : o.ins_len;
        const char* i_src = (dir < 0) ? o.del : o.ins;
        apply_replace(o.pos, d_len, i_src, i_len, false);
        if (dir > 0) op_pos++;
        else cur = o.pos;                    // undo: kursor ke titik perubahan
        ensure_cursor_visible();
        mark_dirty();
        return true;
    }
    void set_enter(ui_click_cb cb, void* u) { enter_cb = cb; enter_data = u; }
    void set_prompt_style(const char* prefix, color_t color) {
        ps1_len = 0;
        if (!prefix) { mark_dirty(); return; }
        for (; ps1_len < (int)sizeof(ps1) - 1 && prefix[ps1_len]; ps1_len++)
            ps1[ps1_len] = prefix[ps1_len];
        ps1[ps1_len] = '\0';
        ps1_color = color_opaque(color);
        mark_dirty();
    }
    // Baris idx (offset mulai baris) diawali prefix prompt berwarna?
    bool line_has_ps1(int idx) const {
        if (ps1_len == 0 || len - idx < ps1_len) return false;
        for (int i = 0; i < ps1_len; i++) if (text[idx + i] != ps1[i]) return false;
        return true;
    }
    // Readonly (output terminal) tak boleh mencuri fokus dari input.
    virtual bool focusable() override { return !readonly; }

    // --- utilitas baris ---
    int line_at(int idx) const {
        int ln = 0;
        for (int i = 0; i < idx && i < len; i++) if (text[i] == '\n') ln++;
        return ln;
    }
    int line_start(int idx) const {
        int i = idx;
        while (i > 0 && text[i - 1] != '\n') i--;
        return i;
    }
    int total_lines() const {
        int ln = 1;
        for (int i = 0; i < len; i++) if (text[i] == '\n') ln++;
        return ln;
    }
    int vis_lines() const { int v = h / LINE_H; return v < 1 ? 1 : v; }

    // --- edit buffer ---
    // insert_at/delete_at lama digantikan apply_replace() (satu titik ubah +
    // perekaman undo). Tidak ada pemanggil lain — dibuang supaya tidak ada
    // jalur edit yang lolos dari undo.
    void append(const char* s) {
        mark_dirty();
        for (int i = 0; s[i] && len < MAX_TEXT - 1; i++) text[len++] = s[i];
        text[len] = '\0';
        cur = len;
        scroll_top = total_lines() - vis_lines();   // ikut ujung (terminal)
        clamp_scroll();
        mark_dirty();
    }
    void clear() {
        mark_dirty();
        apply_replace(0, len, 0, 0, true);   // sekaligus tercatat di undo
    }

    // ------------------------------------------------------------
    // SATU-SATUNYA titik perubahan teks: hapus `del_n` di `pos`, lalu sisipkan
    // `ins_n` karakter. Semua aksi (ketik, backspace, delete, paste, cut,
    // replace-all, undo/redo) lewat sini supaya undo selalu konsisten.
    // record=false dipakai saat undo/redo menerapkan op (jangan rekam ulang).
    // ------------------------------------------------------------
    void apply_replace(int pos, int del_n, const char* ins, int ins_n, bool record) {
        if (pos < 0) pos = 0;
        if (pos > len) pos = len;
        if (del_n < 0) del_n = 0;
        if (del_n > len - pos) del_n = len - pos;
        if (ins_n < 0) ins_n = 0;
        if (len - del_n + ins_n > MAX_TEXT - 1)         // clamp ke kapasitas buffer
            ins_n = MAX_TEXT - 1 - (len - del_n);
        if (ins_n < 0) ins_n = 0;
        if (record) op_record(pos, text + pos, del_n, ins, ins_n);
        if (del_n > 0)
            for (int i = pos; i + del_n <= len; i++) text[i] = text[i + del_n];
        if (ins_n > 0) {
            for (int i = len + ins_n; i >= pos + ins_n; i--) text[i] = text[i - ins_n];
            for (int i = 0; i < ins_n; i++) text[pos + i] = ins[i];
        }
        len += ins_n - del_n;
        text[len] = '\0';
        cur = pos + ins_n;
        sel_anchor = -1;
        ensure_cursor_visible();
        mark_dirty();
        if (change_cb) change_cb(change_data);
    }
    // Sisipkan teks di kursor (dipakai menu Edit → Waktu/Tanggal, replace-all).
    void insert_str(const char* s) {
        int n = 0; while (s[n]) n++;
        if (n == 0) return;
        int lo = sel_lo(), hi = sel_hi();
        apply_replace(lo, hi - lo, s, n, true);
    }

    // --- seleksi ---
    bool has_sel() const { return sel_anchor >= 0 && sel_anchor != cur; }
    int  sel_lo() const {
        if (sel_anchor < 0) return cur;
        return sel_anchor < cur ? sel_anchor : cur;
    }
    int  sel_hi() const {
        if (sel_anchor < 0) return cur;
        return sel_anchor < cur ? cur : sel_anchor;
    }
    void sel_all() { sel_anchor = 0; cur = len; ensure_cursor_visible(); mark_dirty(); }
    void sel_set(int a, int b) {
        if (a < 0) a = 0; if (a > len) a = len;
        if (b < 0) b = 0; if (b > len) b = len;
        sel_anchor = a; cur = b;
        ensure_cursor_visible(); mark_dirty();
    }
    // Hapus seleksi (tanpa mencatat op baru kalau kosong).
    void sel_delete() {
        if (!has_sel()) { sel_anchor = -1; return; }
        int lo = sel_lo(), hi = sel_hi();
        apply_replace(lo, hi - lo, 0, 0, true);
    }
    void copy_sel() {
        if (!has_sel()) return;
        int lo = sel_lo(), hi = sel_hi();
        char* buf = (char*)_ui_alloc((unsigned)(hi - lo) + 1u);
        if (!buf) return;
        for (int i = lo; i < hi; i++) buf[i - lo] = text[i];
        buf[hi - lo] = '\0';
        ui_clipboard_set_text(buf);
        _ui_free(buf);
    }
    void cut_sel() {
        if (!has_sel()) return;
        copy_sel();
        sel_delete();
    }
    void paste_clip() {
        const char* s = ui_clipboard_get_text();
        if (!s || !s[0]) return;
        int n = 0; while (s[n]) n++;
        int lo = sel_lo(), hi = sel_hi();
        apply_replace(lo, hi - lo, s, n, true);
    }

    // --- kursor bergerak (menghormati Shift = perluas seleksi) ---
    void move_to(int idx, bool extend) {
        if (idx < 0) idx = 0;
        if (idx > len) idx = len;
        if (extend) { if (sel_anchor < 0) sel_anchor = cur; }
        else sel_anchor = -1;
        cur = idx;
        ensure_cursor_visible();
        mark_dirty();
    }
    // Lompat satu kata (Ctrl+Left/Right), gaya editor biasa.
    int word_left(int idx) const {
        while (idx > 0 && (text[idx - 1] == ' ' || text[idx - 1] == '\n')) idx--;
        while (idx > 0 && text[idx - 1] != ' ' && text[idx - 1] != '\n') idx--;
        return idx;
    }
    int word_right(int idx) const {
        while (idx < len && text[idx] != ' ' && text[idx] != '\n') idx++;
        while (idx < len && (text[idx] == ' ' || text[idx] == '\n')) idx++;
        return idx;
    }

    // ------------------------------------------------------------
    // Pemetaan baris LAYAR (word wrap). Saat wrap mati, satu baris layar = satu
    // baris dokumen, jadi jalur terminal/readonly tetap identik dengan dulu.
    // ------------------------------------------------------------
    int wrap_cols() const { int c = (w - 8) / CHAR_W; return c < 1 ? 1 : c; }
    // Indeks akhir baris layar yang mulai di `start`.
    int row_limit(int start) const {
        if (!wrap) { int i = start; while (i < len && text[i] != '\n') i++; return i; }
        int cols = wrap_cols(), i = start, c = 0, last_space = -1;
        while (i < len && text[i] != '\n' && c < cols) {
            if (text[i] == ' ') last_space = i;
            i++; c++;
        }
        // Potong di spasi terakhir supaya kata tidak terbelah (gaya Notepad).
        if (i < len && text[i] != '\n' && last_space > start) i = last_space + 1;
        return i;
    }
    int disp_rows() const {
        int n = 0, i = 0;
        for (;;) {
            n++;
            int lim = row_limit(i);
            if (lim >= len) break;
            i = (text[lim] == '\n') ? lim + 1 : lim;
            if (i >= len) { n++; break; }        // '\n' di akhir → baris kosong
            if (n > MAX_TEXT) break;             // jaga-jaga
        }
        return n;
    }
    void disp_pos(int idx, int* row, int* col) const {
        int n = 0, i = 0;
        for (;;) {
            int lim = row_limit(i);
            if (idx <= lim || lim >= len) {
                *row = n;
                *col = idx - i;
                if (*col < 0) *col = 0;
                return;
            }
            i = (text[lim] == '\n') ? lim + 1 : lim;
            n++;
        }
    }
    int disp_to_idx(int row, int col) const {
        int n = 0, i = 0;
        for (;;) {
            int lim = row_limit(i);
            if (n == row) {
                int idx = i + (col > 0 ? col : 0);
                return idx > lim ? lim : idx;
            }
            if (lim >= len) return len;
            i = (text[lim] == '\n') ? lim + 1 : lim;
            n++;
        }
    }

    // ------------------------------------------------------------
    // Sisa kelas: scroll, mouse, keyboard, gambar.
    // ------------------------------------------------------------

    // --- scroll (dalam BARIS LAYAR, jadi ikut word wrap) ---
    void clamp_scroll() {
        int mt = disp_rows() - vis_lines();
        if (mt < 0) mt = 0;
        if (scroll_top > mt) scroll_top = mt;
        if (scroll_top < 0) scroll_top = 0;
    }
    void ensure_cursor_visible() {
        int row = 0, col = 0;
        disp_pos(cur, &row, &col);
        if (row < scroll_top) scroll_top = row;
        else if (row >= scroll_top + vis_lines())
            scroll_top = row - vis_lines() + 1;
        clamp_scroll();
    }
    // Indeks dokumen dari koordinat window-local konten.
    int idx_at(int mx, int my) const {
        int col = (mx - x - 4) / CHAR_W; if (col < 0) col = 0;
        int row = (my - y) / LINE_H + scroll_top; if (row < 0) row = 0;
        return disp_to_idx(row, col);
    }

    virtual void on_click(int mx, int my) override {
        if (enter_cb) {              // terminal: kursor terkunci di baris perintah
            cur = len;
            ensure_cursor_visible();
            mark_dirty();
            return;
        }
        // Klik = letakkan kursor + pasang jangkar; geser mouse (on_drag)
        // sesudahnya memperluas seleksi.
        int idx = idx_at(mx, my);
        cur = idx;
        sel_anchor = readonly ? -1 : idx;
        ensure_cursor_visible();
        mark_dirty();
    }
    // Seret mouse = blok seleksi (Notepad: seleksi teks dengan drag).
    virtual bool on_drag(int mx, int my) override {
        if (enter_cb || sel_anchor < 0) return false;
        int idx = idx_at(mx, my);
        if (idx == cur) return false;
        cur = idx;
        mark_dirty();
        return true;
    }
    virtual void on_release() override { sel_anchor = has_sel() ? sel_anchor : -1; }
    virtual bool on_scroll(int delta) override {
        int old = scroll_top;
        scroll_top += delta;        // +1 roda bawah = lihat output lebih bawah
        clamp_scroll();
        if (scroll_top != old) mark_dirty();
        return scroll_top != old;
    }
    virtual void on_key(uint8_t ascii, uint32_t scancode, uint32_t mods) override {
        mark_dirty();
        uint32_t sc = scancode & 0xFF;
        bool shift = (mods & KEY_MOD_SHIFT) != 0;
        bool ctrl  = (mods & KEY_MOD_CTRL) != 0;
        if (ctrl) {
            // Ctrl+A/Ctrl+E (Home/End baris) = gaya Emacs, dipakai terminal.
            // Notepad mendaftarkan Ctrl+A sebagai Select All lewat shortcut, jadi
            // jalur ini tidak pernah menyentuh editor teks.
            if (ascii == 'a' || ascii == 'A') { move_to(line_start(cur), shift); return; }
            if (ascii == 'e' || ascii == 'E') {
                int i = cur;
                while (i < len && text[i] != '\n') i++;
                move_to(i, shift); return;
            }
            // Ctrl+Panah = lompat per kata; Ctrl+Home/End = awal/akhir dokumen.
            if (sc == 0x4B) { move_to(word_left(cur), shift); return; }
            if (sc == 0x4D) { move_to(word_right(cur), shift); return; }
            if (sc == 0x47) { move_to(0, shift); return; }
            if (sc == 0x4F) { move_to(len, shift); return; }
            return;
        }
        // Terminal (enter_cb): Enter = submit ke app, bukan sisip '\n'.
        if (enter_cb && (ascii == '\n' || ascii == '\r' || sc == 0x1C)) {
            enter_cb(enter_data);
            return;
        }
        int lower = enter_cb ? line_start(len) : 0;   // edit hanya baris perintah
        // Semua mutasi lewat apply_replace() supaya seleksi ikut terhapus dan
        // langkah undo tercatat satu per aksi (bukan satu per karakter buffer).
        if (ascii >= 32) {                            // printable → sisip/timpa seleksi
            if (!readonly) {
                char c = (char)ascii;
                apply_replace(sel_lo(), sel_hi() - sel_lo(), &c, 1, true);
            }
            return;
        }
        if (sc == 0x0E) {                             // Backspace
            if (readonly) return;
            if (has_sel()) { sel_delete(); return; }
            if (cur > lower) apply_replace(cur - 1, 1, 0, 0, true);
            return;
        }
        if (sc == 0x53) {                             // Delete
            if (readonly) return;
            if (has_sel()) { sel_delete(); return; }
            if (cur < len) apply_replace(cur, 1, 0, 0, true);
            return;
        }
        if (ascii == '\n' || ascii == '\r' || sc == 0x1C) {  // Enter (auto-indent)
            if (!readonly) {
                apply_replace(sel_lo(), sel_hi() - sel_lo(), "\n", 1, true);
            }
            return;
        }
        if (sc == 0x0F) {                             // Tab → 4 spasi (font 8x16)
            if (!readonly) apply_replace(sel_lo(), sel_hi() - sel_lo(), "    ", 4, true);
            return;
        }
        // --- navigasi (Shift = perluas seleksi) ---
        if (sc == 0x4B) { if (cur > lower) move_to(cur - 1, shift); return; }   // Left
        if (sc == 0x4D) { if (cur < len) move_to(cur + 1, shift); return; }     // Right
        if (sc == 0x48 && !enter_cb) {                                          // Up
            int col = cur - line_start(cur);
            int ls = line_start(cur);
            if (ls > 0) {
                int ps = line_start(ls - 1), pe = ps;
                while (pe < len && text[pe] != '\n') pe++;
                int t = ps + col; if (t > pe) t = pe;
                move_to(t, shift);
            } else if (shift) move_to(0, true);
            return;
        }
        if (sc == 0x50 && !enter_cb) {                                          // Down
            int col = cur - line_start(cur);
            int le = cur;
            while (le < len && text[le] != '\n') le++;
            if (le < len) {
                int ns = le + 1, ne = ns;
                while (ne < len && text[ne] != '\n') ne++;
                int t = ns + col; if (t > ne) t = ne;
                move_to(t, shift);
            } else if (shift) move_to(len, true);
            return;
        }
        if (sc == 0x47) { move_to(enter_cb ? lower : line_start(cur), shift); return; }  // Home
        if (sc == 0x4F) {                                                       // End
            int i = cur; while (i < len && text[i] != '\n') i++;
            move_to(i, shift);
            return;
        }
        if (sc == 0x49) {                                                       // PgUp
            // Di editor: pindahkan kursor satu layar penuh (kursor ikut), di
            // terminal cukup viewport-nya.
            if (enter_cb) { scroll_top -= vis_lines(); clamp_scroll(); return; }
            int row = 0, col = 0;
            disp_pos(cur, &row, &col);
            int t = row - vis_lines(); if (t < 0) t = 0;
            move_to(disp_to_idx(t, col), shift);
            return;
        }
        if (sc == 0x51) {                                                       // PgDn
            if (enter_cb) { scroll_top += vis_lines(); clamp_scroll(); return; }
            int row = 0, col = 0;
            disp_pos(cur, &row, &col);
            move_to(disp_to_idx(row + vis_lines(), col), shift);
            return;
        }
        // tombol lain: tidak ada yang berubah (mark_dirty di atas tak apa).
    }
    virtual void draw(Painter& p) override {
        clamp_scroll();
        // Area teks = permukaan "editor" (charcoal #1E1E1E, bukan hitam murni)
        // + garis tepi 1px sebagai batas dari menubar/status bar.
        p.rect(x, y, w, h, p.theme.editor);
        p.rect(x, y, w, 1, p.theme.divider);
        p.rect(x, y + h - 1, w, 1, p.theme.divider);
        p.set_clip(x, y, w, h);
        int colw = (w - 8) / CHAR_W;    // kolom yang muat (margin 4px)
        // Lewati scroll_top baris LAYAR (bukan baris dokumen — wrap mengubahnya).
        int idx = 0, row = 0;
        while (row < scroll_top) {
            int lim = row_limit(idx);
            if (lim >= len) { idx = len; break; }
            idx = (text[lim] == '\n') ? lim + 1 : lim;
            row++;
        }
        bool sel_on = has_sel();
        int hlo = sel_on ? sel_lo() : -1;
        int hhi = sel_on ? sel_hi() : -1;
        int vy = y;
        while (vy < y + h && idx <= len) {
            int lim = row_limit(idx);
            if (lim > idx + colw) lim = idx + colw;   // jaga-jaga (wrap mati)
            int cx = x + 4;
            bool ps1_here = line_has_ps1(idx);        // prompt → prefix berwarna
            for (int c = 0; idx + c < lim; c++) {
                int ci = idx + c;
                // Blok seleksi digambar sebagai latar sebelum karakternya.
                if (ci >= hlo && ci < hhi)
                    p.rect(cx, vy, CHAR_W, LINE_H, p.theme.button_hover);
                char t[2] = { text[ci], '\0' };
                color_t col = (ps1_here && c < ps1_len) ? ps1_color : p.theme.fg;
                p.text(t, cx, vy + 1, col);
                cx += CHAR_W;
            }
            // "kursor" seleksi tepat setelah karakter terakhir tidak punya sel
            // sendiri — tapi caret tetap tergambar di bawah, jadi aman.
            vy += LINE_H;
            if (lim >= len) break;
            idx = (text[lim] == '\n') ? lim + 1 : lim;
        }
        // caret — posisi kursor dipetakan ke baris/kolom LAYAR, digambar sekali
        if (has_focus) {
            int crow = 0, ccol = 0;
            disp_pos(cur, &crow, &ccol);
            crow -= scroll_top;
            if (crow >= 0 && crow * LINE_H < h) {
                int cx = x + 4 + ccol * CHAR_W;
                if (cx >= x + w) cx = x + w - 1;
                p.rect(cx, y + crow * LINE_H, 2, LINE_H, p.theme.caret);
            }
        }
        p.clear_clip();
    }
};

} // namespace ui

#endif // KWIDGET_EDITOR_TEXTEDIT_HPP
