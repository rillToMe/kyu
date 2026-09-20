// kernel/panic/panic_draw.c — mesin gambar darurat BSOD (tanpa malloc).
//
// TANGGUNG JAWAB
//   * Memilih TARGET GAMBAR sekali per panic: memori scanout device (virtio)
//     bila backend menyediakannya, kalau tidak framebuffer hardware (Limine).
//     Lihat bagian TARGET di bawah — di sinilah bug "panic jalan tapi layar
//     tetap menampilkan desktop" diperbaiki.
//   * Semua primitive gambar: pixel, char (font8x16), string, hex, dec.
//   * Tata letak tingkat tinggi: kursor baris, label/indent, banner judul,
//     garis pemisah, baris status, pita panic bersarang.
//
// KONTRAK (lihat panic_internal.h)
//   Tanpa heap, tanpa lock, tanpa IRQ. Pemanggil menjalankan seluruh jalur
//   panic dengan `cli`, jadi tidak boleh ada paging baru/IRQ-wait di sini.
//
// ANTI-FLICKER
//   Target gambar adalah scanout (tanpa double buffer), jadi menggambar ulang
//   seluruh layar tiap iterasi loop terlihat sebagai hilang-timbul. Panggilan
//   gambar dipakai SEKALI untuk layar penuh; perubahan berikutnya (baris
//   status, pita panic bersarang) hanya menimpa bagian yang berubah, dan di
//   jalur device hanya KOTAK ITU yang dikirim (lihat p_flush).

#include <stdint.h>

#include "panic_internal.h"
#include "serial.h"    // serial_print (mirror COM1)
#include "display.h"   // display_get_mode() — geometri framebuffer fallback

// Scanout darurat (graphics/ghal.h). Dideklarasikan langsung, bukan lewat
// header: file modul panic sengaja minim dependensi — beberapa header kernel
// membawa lock/alokasi yang tidak boleh dipakai jalur ini. Kedua fungsi ini
// WAJIB bebas lock & alokasi (lihat komentar di ghal.h), karena CPU yang fault
// bisa jadi pemegang lock device itu sendiri.
extern int ghal_scanout_map(uint32_t** pixels, uint32_t* width, uint32_t* height,
                            uint32_t* pitch_px);
extern int ghal_scanout_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// =====================================================================
// 1. TARGET GAMBAR: scanout device dulu, framebuffer sebagai fallback
// =====================================================================
// Layar panic digambar langsung ke memori yang SEDANG tampil. Pada backend
// virtio-gpu itu resource scanout milik compositor, bukan framebuffer Limine:
// device mengganti outputnya ke resource begitu SET_SCANOUT jalan, jadi tulisan
// ke fb_ptr tidak pernah terlihat (gejalanya: sistem panik, layar tetap
// menampilkan desktop). Backend menyerahkan backing scanout lewat
// ghal_scanout_map(), dan tiap blok gambar dikirim ke device dengan
// ghal_scanout_flush().
//
// Fallback: kalau scanout device tidak ada (backend software, device belum
// siap, atau panic sebelum compositor init), target jatuh ke framebuffer
// hardware — perilaku lama, jadi tidak ada kondisi di mana BSOD kehilangan
// tempat menggambar.
static uint32_t* p_target = NULL;        // buffer gambar aktif (NULL = serial saja)
static uint32_t  p_target_w = 0;
static uint32_t  p_target_h = 0;
static uint32_t  p_target_pitch = 0;     // pitch dalam PIKSEl
static int       p_target_scanout = 0;   // 1 = perubahan harus di-flush ke device
static int       p_target_done = 0;      // sudah dipilih (idempotent per panic)

// Kotak kotor: batas kanan/bawah EKSKLUSIF. Hanya berarti untuk target scanout
// (framebuffer langsung sudah terlihat begitu ditulis). Mengirim hanya kotak
// ini yang menjaga sifat anti-flicker: layar penuh digambar+terkirim SEKALI,
// perubahan berikutnya (baris status, pita panic bersarang) hanya beberapa
// baris, bukan 1920x1080 lagi.
static uint32_t p_dirty_x0, p_dirty_y0, p_dirty_x1, p_dirty_y1;

static void p_dirty_reset(void) {
    p_dirty_x0 = 0xFFFFFFFFu; p_dirty_y0 = 0xFFFFFFFFu;
    p_dirty_x1 = 0;           p_dirty_y1 = 0;
}

#ifdef PANIC_HOST_TEST
void panic_target_reset(void) {
    p_target = NULL;
    p_target_w = 0; p_target_h = 0; p_target_pitch = 0;
    p_target_scanout = 0;
    p_target_done = 0;
    p_dirty_reset();
}
#endif

// Pilih target gambar. Idempotent: pemanggilan kedua hanya mengembalikan hasil
// pemilihan pertama (layar sudah digambar di sana, tidak boleh pindah buffer).
int panic_target_begin(void) {
    if (p_target_done) return p_target ? 0 : -1;
    p_target_done = 1;
    p_dirty_reset();

    uint32_t* px = NULL;
    uint32_t  w = 0, h = 0, pitch = 0;
    if (ghal_scanout_map(&px, &w, &h, &pitch) == 0 && px && w && h && pitch >= w) {
        p_target = px;
        p_target_w = w; p_target_h = h; p_target_pitch = pitch;
        p_target_scanout = 1;
        serial_print("[PANIC] layar: scanout device ");
        ser_dec(w); serial_print("x"); ser_dec(h);
        serial_print(" (gambar langsung ke resource, tanpa framebuffer)\n");
        return 0;
    }

    const display_mode_t* m = display_get_mode();
    if (!fb_ptr || !m || m->width == 0 || m->height == 0 ||
        m->pitch_bytes < m->width * 4u) {
        serial_print("[PANIC] layar: TIDAK ada target gambar - lapor ke serial saja\n");
        return -1;
    }
    p_target = fb_ptr;
    p_target_w = m->width; p_target_h = m->height;
    p_target_pitch = m->pitch_bytes / 4u;
    p_target_scanout = 0;
    serial_print("[PANIC] layar: FALLBACK framebuffer ");
    ser_dec(m->width); serial_print("x"); ser_dec(m->height);
    serial_print(" (scanout device tidak tersedia)\n");
    return 0;
}

// 1 = ada target gambar yang bisa dipakai (setelah panic_target_begin()).
// Dipakai orchestrator untuk memutuskan apakah baris info/tombol perlu digambar
// (layar pendek atau tanpa target = lewati, jangan tulis ke mana-mana).
int panic_target_ready(void) { return p_target != NULL; }

// Kirim perubahan yang sudah digambar. No-op di jalur framebuffer (menulis =
// sudah terlihat) dan saat tidak ada piksel baru. Kotak kotor direset HANYA
// bila device menerimanya: kalau command dilewati (lock device dipegang CPU
// lain), percobaan berikutnya mengirim ulang kotak yang sama.
void p_flush(void) {
    if (!p_target_scanout) return;
    if (p_dirty_x1 <= p_dirty_x0 || p_dirty_y1 <= p_dirty_y0) return;
    if (ghal_scanout_flush(p_dirty_x0, p_dirty_y0,
                           p_dirty_x1 - p_dirty_x0, p_dirty_y1 - p_dirty_y0) == 0)
        p_dirty_reset();
}

uint32_t panic_screen_w(void) {
    if (p_target) return p_target_w;
    const display_mode_t* m = display_get_mode();
    return m ? m->width : 0u;
}

uint32_t panic_screen_h(void) {
    if (p_target) return p_target_h;
    const display_mode_t* m = display_get_mode();
    return m ? m->height : 0u;
}

// Baris DETAIL tidak boleh menabrak dua baris info/tombol di bawah layar.
static uint32_t panic_text_limit_y(void) {
    uint32_t h = panic_screen_h();
    return h > PANIC_BOTTOM_GAP ? h - PANIC_BOTTOM_GAP : h;
}

static uint32_t panic_strlen(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

// =====================================================================
// 2. PRIMITIVE GAMBAR
// =====================================================================
void panic_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!p_target || x >= p_target_w || y >= p_target_h) return;
    p_target[(uint64_t)y * p_target_pitch + x] = color;
    if (p_target_scanout) {
        // Perluas kotak kotor (batas kanan/bawah eksklusif → x+1/y+1).
        if (x < p_dirty_x0) p_dirty_x0 = x;
        if (y < p_dirty_y0) p_dirty_y0 = y;
        if (x + 1u > p_dirty_x1) p_dirty_x1 = x + 1u;
        if (y + 1u > p_dirty_y1) p_dirty_y1 = y + 1u;
    }
}

void panic_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    if ((unsigned char)c > 127) c = '?';          // font layar ASCII-only
    const unsigned char* bmp = font8x16[(unsigned char)c];
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            panic_draw_pixel(x + (uint32_t)col, y + (uint32_t)row,
                             (bmp[row] & (0x80 >> col)) ? fg : bg);
        }
    }
}

void panic_draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    if (!str) return;
    for (uint32_t i = 0; str[i]; i++) {
        panic_draw_char(str[i], x + i * PANIC_CHAR_W, y, fg, bg);
    }
}

void panic_draw_hex(uint64_t num, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    const char* digits = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = digits[num & 0xF]; num >>= 4; }
    panic_draw_string(buf, x, y, fg, bg);
}

// Desimal -> teks. Mengembalikan panjang; `out` minimal 21 byte.
static uint32_t panic_u64_dec(uint64_t v, char* out) {
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    for (uint32_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = '\0';
    return n;
}

void panic_draw_dec(uint64_t num, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    char buf[24];
    panic_u64_dec(num, buf);
    panic_draw_string(buf, x, y, fg, bg);
}

void fill_screen(uint32_t color) {
    const uint32_t w = panic_screen_w();
    const uint32_t h = panic_screen_h();
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            panic_draw_pixel(x, y, color);
}

static void p_hline(uint32_t x, uint32_t y, uint32_t len, uint32_t color) {
    for (uint32_t i = 0; i < len; i++) panic_draw_pixel(x + i, y, color);
}

static void p_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            panic_draw_pixel(x + i, y + j, color);
}

// =====================================================================
// 3. TATA LETAK: kursor baris, label, banner, garis
// =====================================================================
static uint32_t panic_cursor_x = PANIC_X;
static uint32_t panic_cursor_y = PANIC_TOP_Y;

void p_home(void) {
    panic_cursor_x = PANIC_X;
    panic_cursor_y = PANIC_TOP_Y;
}

void p_newline(void) {
    panic_cursor_x = PANIC_X;
    uint32_t limit = panic_text_limit_y();
    if (limit && panic_cursor_y + PANIC_ROW_H + 16u > limit) return;  // penuh
    panic_cursor_y += PANIC_ROW_H;
}

// Tulis teks pada kursor, majukan kursor. Tidak menggambar kalau baris sudah
// melewati batas (layar pendek) — supaya tidak menimpa baris info/tombol.
void p_str(const char* s, uint32_t fg) {
    uint32_t limit = panic_text_limit_y();
    if (limit && panic_cursor_y >= limit) return;
    panic_draw_string(s, panic_cursor_x, panic_cursor_y, fg, C_BG);
    panic_cursor_x += panic_strlen(s) * PANIC_CHAR_W;
}

void p_hex(uint64_t v, uint32_t fg) {
    panic_draw_hex(v, panic_cursor_x, panic_cursor_y, fg, C_BG);
    panic_cursor_x += 18u * PANIC_CHAR_W;
}

void p_dec(uint64_t v, uint32_t fg) {
    char buf[24];
    uint32_t n = panic_u64_dec(v, buf);
    p_str(buf, fg);
    (void)n;
}

// Baris berlabel: label redup di kolom kiri, nilai mulai di kolom PANIC_INDENT.
void p_label(const char* label) {
    panic_cursor_x = PANIC_X;
    p_str(label, C_FAINT);
    panic_cursor_x = PANIC_INDENT;
}

// Baris lanjutan (tanpa label).
void p_cont_begin(void) { panic_cursor_x = PANIC_INDENT; }

// Gambar satu baris utuh di y tertentu (dipakai baris info & petunjuk tombol).
void p_line(uint32_t y, const char* s, uint32_t fg) {
    panic_cursor_x = PANIC_X;
    panic_cursor_y = y;
    panic_draw_string(s, PANIC_X, y, fg, C_BG);
    panic_cursor_x += panic_strlen(s) * PANIC_CHAR_W;
}

// Label + garis tipis 1px sampai margin kanan (pengganti deretan '----').
void p_rule(const char* label) {
    panic_cursor_x = PANIC_X;
    p_str(label, C_FAINT);
    uint32_t y = panic_cursor_y + 8u;
    uint32_t x0 = panic_cursor_x + 16u;
    uint32_t w = panic_screen_w();
    uint32_t x1 = w > 24u ? w - 24u : x0;
    if (x1 > x0) p_hline(x0, y, x1 - x0, C_RULE);
    p_newline();
}

// Banner: dua garis merah 2px + judul + (opsional) penanda panic berulang.
void p_banner(int repeat) {
    uint32_t w = panic_screen_w();
    uint32_t x0 = PANIC_X;
    uint32_t x1 = w > PANIC_X + 40u ? w - PANIC_X : PANIC_X + 40u;

    p_hline(x0, panic_cursor_y, x1 - x0, C_TITLE);
    p_hline(x0, panic_cursor_y + 1u, x1 - x0, C_TITLE);
    panic_cursor_y += 6u;

    panic_cursor_x = PANIC_X;
    p_str("KYUZEN OS", C_TITLE);
    panic_cursor_x += 3u * PANIC_CHAR_W;
    p_str("KERNEL PANIC", C_TITLE);

    if (repeat) {
        const char* tag = "PANIC BERULANG";
        uint32_t tw = panic_strlen(tag) * PANIC_CHAR_W;
        if (x1 > tw + PANIC_CHAR_W) {
            uint32_t tx = x1 - tw;
            // Rata kanan pada grid karakter yang sama dengan sisa layar
            // (PANIC_X bukan kelipatan 8), supaya teks tetap "satu kolom".
            tx -= (tx - PANIC_X) % PANIC_CHAR_W;
            panic_draw_string(tag, tx, panic_cursor_y, C_KEY, C_BG);
        }
    }

    // Baris judul setinggi 16 px: garis penutup HARUS di bawahnya, bukan
    // menembus glyph (dulu 8 px sehingga judul terpotong garis).
    panic_cursor_y += PANIC_ROW_H;
    p_hline(x0, panic_cursor_y, x1 - x0, C_TITLE);
    p_hline(x0, panic_cursor_y + 1u, x1 - x0, C_TITLE);
    panic_cursor_y += 8u;
    panic_cursor_x = PANIC_X;
}

// Pita peringatan di atas BSOD yang SUDAH tergambar. Dipakai jalur "panic
// bersarang": layar panic pertama tetap berlaku, tapi pengguna harus tahu
// kenapa crashdump/reboot tidak jalan. Best-effort: kalau target gambar belum
// siap, fungsi ini tidak melakukan apa-apa (serial tetap dapat laporannya).
void panic_overlay_banner(const char* msg) {
    // Panic bersarang bisa terjadi SEBELUM jalur gambar mana pun berjalan, jadi
    // target dipastikan terpilih dulu (idempotent).
    (void)panic_target_begin();
    const uint32_t mw = panic_screen_w();
    const uint32_t mh = panic_screen_h();
    if (!p_target || mh < PANIC_UI_MIN_HEIGHT) return;

    // Baris STATUS (di atas baris tombol) — bukan di tepi atas layar: di atas
    // sana pita bertabrakan dengan banner judul BSOD dan tampak "terlalu
    // tinggi". Di baris status ia terbaca sebagai catatan kaki, dan saat panic
    // bersarang baris itu memang kosong (loop tombol tidak jalan).
    const uint32_t x = PANIC_X;
    const uint32_t y = mh - 68u;
    uint32_t avail   = panic_strlen(msg);
    uint32_t max_ch  = (mw > x + 16u) ? (mw - x - 16u) / PANIC_CHAR_W : 0u;
    if (avail > max_ch) avail = max_ch;          // potong, jangan lewat margin

    // Latar = C_BG (bukan warna khusus): teks tetap terbaca di layar DAN tetap
    // bisa di-decode `panic_test --dump` (decoder mencocokkan tiap sel 8x16).
    p_fill_rect(x, y, avail * PANIC_CHAR_W, PANIC_ROW_H, C_BG);
    for (uint32_t i = 0; i < avail; i++)
        panic_draw_char(msg[i], x + i * PANIC_CHAR_W, y, C_TITLE, C_BG);
    p_flush();
}

// Baris status di atas baris tombol: umpan balik sesaat sebelum sistem
// mati/beku. Dipanggil aksi operator di panic_hw.c.
void panic_status_line(const char* msg) {
    if (!p_target || panic_screen_h() < PANIC_UI_MIN_HEIGHT) return;
    uint32_t y = panic_screen_h() - 68u;
    uint32_t w = (panic_screen_w() > 2u * PANIC_X) ? (panic_screen_w() - 2u * PANIC_X) : 0u;
    p_fill_rect(PANIC_X, y, w, PANIC_ROW_H, C_BG);
    panic_draw_string(msg, PANIC_X, y, C_FG, C_BG);
    p_flush();   // baris status harus sudah di device sebelum aksi shutdown/reboot
}

// =====================================================================
// 4. MIRROR SERIAL (COM1) — versi lokal tanpa lock/buffer bersama
// =====================================================================
void ser_hex(uint64_t v) {
    const char* d = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    serial_print(buf);
}

void ser_dec(uint64_t v) {
    char buf[24];
    panic_u64_dec(v, buf);
    serial_print(buf);
}
