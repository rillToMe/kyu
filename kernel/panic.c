// kernel/panic.c — BSOD KyuzenOS (layar panic + auto-reboot).
//
// PRINSIP
//   * Tanpa heap, tanpa lock. Semua menggambar langsung ke framebuffer lewat
//     panic_draw_*; alamat yang dibaca (stack/RBP chain) selalu dicek dulu
//     dengan paging_is_mapped() supaya tidak double-fault.
//   * LOCKDOWN. panic_lockdown() dipanggil paling awal. Semua penulis scanout
//     lain (compositor_flush, HUD/kursor timer callback, mouse_handler)
//     early-return selama panic_is_locked(). Tanpa ini, di sistem SMP CPU lain
//     tetap merender desktop di atas layar panic — gejalanya "BSOD glitch lalu
//     hilang saat mouse digerakkan" (panic hanya `cli` di CPU yang fault).
//   * HIERARKI INFORMASI (dari atas ke bawah = dari yang paling berguna):
//        PENYEBAB  verdict + petunjuk tindakan
//        DI MANA   task/CPU + RIP (di mana fault terjadi)
//        DETAIL    semua angka mentah (warna redup) — arsip, bukan headline
//   * ANTI-FLICKER. Framebuffer di sini adalah scanout (tanpa double buffer),
//     jadi baris statis digambar SEKALI dan hanya 2 sel digit countdown yang
//     ditimpa saat angkanya berubah. Jangan kembali ke pola "clear + redraw
//     baris penuh tiap iterasi loop" — itu terlihat sebagai hilang-timbul.
//   * AUTO-REBOOT. Hitung mundur PANIC_REBOOT_DELAY_MS, lalu kfs_sync_all()
//     (data user selamat) dan reset hardware: 8042 0xFE, fallback triple-fault
//     via IDT kosong. Kalau timer mati (IRQ tidak lagi dilayani), ada spin
//     counter cadangan supaya tetap reboot, tidak menggantung selamanya.
//
// HOST TEST
//   Compile dengan -DPANIC_HOST_TEST (lihat test/panic_test.c): instruksi
//   privileged (cli/sti/hlt/lidt/CR2/CR3/outb) di-stub, jam diganti
//   g_panic_test_ms. `./test/panic_test --dump` men-decode framebuffer kembali
//   menjadi teks memakai font8x16 asli, jadi tata letak BSOD bisa diperiksa
//   tanpa boot QEMU.
//
// CATATAN STRING: font layar ASCII-only (panic_draw_char memetakan >127 ke
// '?'), jadi JANGAN pakai em dash / karakter box-drawing di string BSOD.

#include <stdint.h>

#include "panic.h"
#include "task.h"      // registers_t, task_t, tasks[], smp_current_task_id()
#include "serial.h"    // serial_print (mirror COM1)
#include "display.h"   // display_get_mode()

// --- Dependensi eksternal -------------------------------------------------
extern uint32_t* fb_ptr;
extern unsigned char font8x16[][16];   // definisi: kernel/gfx/fb.c

extern int      paging_is_mapped(uint64_t vaddr);
extern uint32_t smp_current_cpu_index(void);
extern uint32_t smp_online_cpu_count(void);
extern uint64_t pmm_get_used_pages(void);
extern uint64_t pmm_get_total_pages(void);
extern uint64_t timer_get_ms(void);
extern void     kfs_sync_all(void);

// Konteks syscall terakhir (kernel/syscall.c) — best-effort, tanpa lock.
extern volatile uint64_t g_last_syscall_num;
extern volatile int32_t  g_last_syscall_task;

// =====================================================================
// Konstanta tata letak & warna
// =====================================================================
#define PANIC_X          50u    // margin kiri semua blok teks
#define PANIC_TOP_Y      30u    // baris pertama banner
#define PANIC_ROW_H      18u    // jarak antar baris (font 8x16 + 2)
#define PANIC_CHAR_W     8u
#define PANIC_LABEL_W    11u    // lebar kolom label ("PENYEBAB   ")
#define PANIC_INDENT     (PANIC_X + PANIC_LABEL_W * PANIC_CHAR_W)  // 138
#define PANIC_BOTTOM_GAP 64u    // area bawah yang disisakan untuk countdown rows

#define C_BG     0x001144u      // latar biru tua
#define C_FG     0xFFFFFFu      // teks utama
#define C_TITLE  0xFF4444u      // judul / verdict
#define C_KEY    0xFFCC00u      // angka & nilai penting
#define C_DIM    0x88AAFFu      // label
#define C_FAINT  0x6E82A8u      // bagian DETAIL (sengaja redup)
#define C_RULE   0x3A4E78u      // garis tipis 1px
#define C_OK     0x44FF44u      // penanda "normal / di dalam kernel text"

#define PANIC_REBOOT_DELAY_MS  10000u   // 10 detik sebelum reset otomatis
#define PANIC_MUTE_SPIN_LIMIT  30000000u // timer mati: spin ~30 juta lalu reboot
#define KERNEL_VMA             0xFFFFFFFF80000000ull   // lihat linker.ld

// =====================================================================
// 0. LOCKDOWN (didefinisikan paling awal: seam host test mereset flag ini)
// =====================================================================
static volatile uint32_t g_panic_lockdown = 0;

// =====================================================================
// 1. PRIMITIF PRIVILEGED (di-stub kalau PANIC_HOST_TEST)
// =====================================================================
#ifdef PANIC_HOST_TEST
// Jam palsu: tiap pembacaan di panic_clock_ms() TIDAK mengubah waktu, tapi
// panic_countdown_tick() maju 1 detik sekali supaya countdown deterministik.
volatile uint64_t g_panic_test_ms      = 0;
volatile uint64_t g_panic_test_cr2     = 0;
volatile uint32_t g_panic_test_redraws = 0;
volatile uint32_t g_panic_test_reboots = 0;

static void (*g_reboot_hook)(void) = 0;

void panic_host_test_reset(void) {
    g_panic_lockdown = 0;            // dideklarasikan di bagian lockdown di bawah
    g_panic_test_ms = 0;
    g_panic_test_cr2 = 0;
    g_panic_test_redraws = 0;
    g_panic_test_reboots = 0;
    g_reboot_hook = 0;
}
#else
static void (*g_reboot_hook)(void) = 0;
#endif

static void panic_cli(void) {
#ifndef PANIC_HOST_TEST
    __asm__ volatile("cli");
#endif
}

static void panic_sti(void) {
#ifndef PANIC_HOST_TEST
    __asm__ volatile("sti");
#endif
}

// Hanya dipakai jalur kernel (panic_reset_hw/panic_freeze) — di host test
// tidak ada instruksi privileged yang dijalankan.
#ifndef PANIC_HOST_TEST
static void panic_halt(void) {
    __asm__ volatile("hlt");
}
#endif

static void panic_pause(void) {
#ifndef PANIC_HOST_TEST
    __asm__ volatile("pause");
#endif
}

static uint64_t panic_read_cr2(void) {
#ifdef PANIC_HOST_TEST
    return g_panic_test_cr2;
#else
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
#endif
}

static uint64_t panic_read_cr3(void) {
#ifdef PANIC_HOST_TEST
    return 0x0000000012345000ull;
#else
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
#endif
}

// Waktu sekarang TANPA efek samping (dipakai untuk uptime).
static uint64_t panic_clock_ms(void) {
#ifdef PANIC_HOST_TEST
    return g_panic_test_ms;
#else
    return timer_get_ms();
#endif
}

// Waktu sekarang untuk loop countdown.
static uint64_t panic_countdown_tick(void) {
#ifdef PANIC_HOST_TEST
    g_panic_test_ms += 1000;         // 1 pembacaan = 1 detik
    return g_panic_test_ms;
#else
    return timer_get_ms();
#endif
}

// Reset hardware terakhir: keyboard controller + fallback triple fault.
// Tidak pernah kembali. (Tidak dibangun di host test — di sana reboot
// diwakili panic_set_reboot_hook.)
#ifndef PANIC_HOST_TEST
static void panic_reset_hw(void) {
    // Kuras buffer keyboard controller dulu: kalau 0x60/0x64 masih penuh,
    // perintah reset bisa diabaikan.
    for (uint32_t guard = 0; guard < 100000u; guard++) {
        uint8_t st = 0;
        __asm__ volatile("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
        if (st & 0x01) {
            uint8_t junk = 0;
            __asm__ volatile("inb %1, %0" : "=a"(junk) : "Nd"((uint16_t)0x60));
        }
        if (!(st & 0x02)) break;
    }
    __asm__ volatile("outb %0, %1" ::"a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    // Cadangan: IDT kosong + int3 = triple fault = reset CPU.
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } null_idt = { 0, 0 };
    __asm__ volatile("cli");
    __asm__ volatile("lidt %0" ::"m"(null_idt));
    __asm__ volatile("int3");
    for (;;) panic_halt();
}
#endif

// Simpan data user, lalu reboot. Di host test cukup memanggil hook + kembali.
static void panic_flush_and_reboot(void) {
    kfs_sync_all();                 // flush FS sebelum reset (write-back cache)
#ifdef PANIC_HOST_TEST
    g_panic_test_reboots++;
    if (g_reboot_hook) g_reboot_hook();
    return;                         // kontrol kembali ke test
#else
    if (g_reboot_hook) g_reboot_hook();
    panic_reset_hw();               // tidak kembali
#endif
}

static void panic_freeze(void) {
#ifdef PANIC_HOST_TEST
    return;                         // test lanjut ke skenario berikutnya
#else
    for (;;) panic_halt();
#endif
}

// =====================================================================
// 2. LOCKDOWN
// =====================================================================
void panic_lockdown(void) { g_panic_lockdown = 1; }
int  panic_is_locked(void) { return (int)g_panic_lockdown; }
void panic_set_reboot_hook(void (*fn)(void)) { g_reboot_hook = fn; }

// =====================================================================
// 3. MESIN GAMBAR DARURAT (tanpa malloc)
// =====================================================================
static uint32_t panic_cursor_x = PANIC_X;
static uint32_t panic_cursor_y = PANIC_TOP_Y;

static uint32_t panic_screen_w(void) {
    const display_mode_t* m = display_get_mode();
    return m ? m->width : 0u;
}

static uint32_t panic_screen_h(void) {
    const display_mode_t* m = display_get_mode();
    return m ? m->height : 0u;
}

// Baris DETAIL tidak boleh menabrak dua baris countdown di bawah layar.
static uint32_t panic_text_limit_y(void) {
    uint32_t h = panic_screen_h();
    return h > PANIC_BOTTOM_GAP ? h - PANIC_BOTTOM_GAP : h;
}

static uint32_t panic_strlen(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

void panic_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    const display_mode_t* m = display_get_mode();
    if (!fb_ptr || !m || x >= m->width || y >= m->height) return;
    fb_ptr[(y * (m->pitch_bytes / 4)) + x] = color;
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

static void fill_screen(uint32_t color) {
    const display_mode_t* m = display_get_mode();
    if (!m) return;
    for (uint32_t y = 0; y < m->height; y++)
        for (uint32_t x = 0; x < m->width; x++)
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

// --- kursor ---------------------------------------------------------------
static void p_home(void) {
    panic_cursor_x = PANIC_X;
    panic_cursor_y = PANIC_TOP_Y;
}

static void p_newline(void) {
    panic_cursor_x = PANIC_X;
    uint32_t limit = panic_text_limit_y();
    if (limit && panic_cursor_y + PANIC_ROW_H + 16u > limit) return;  // penuh
    panic_cursor_y += PANIC_ROW_H;
}

// Tulis teks pada kursor, majukan kursor. Tidak menggambar kalau baris sudah
// melewati batas (layar pendek) — supaya tidak menimpa baris countdown.
static void p_str(const char* s, uint32_t fg) {
    uint32_t limit = panic_text_limit_y();
    if (limit && panic_cursor_y >= limit) return;
    panic_draw_string(s, panic_cursor_x, panic_cursor_y, fg, C_BG);
    panic_cursor_x += panic_strlen(s) * PANIC_CHAR_W;
}

static void p_hex(uint64_t v, uint32_t fg) {
    panic_draw_hex(v, panic_cursor_x, panic_cursor_y, fg, C_BG);
    panic_cursor_x += 18u * PANIC_CHAR_W;
}

static void p_dec(uint64_t v, uint32_t fg) {
    char buf[24];
    uint32_t n = panic_u64_dec(v, buf);
    p_str(buf, fg);
    (void)n;
}

// Baris berlabel: label redup di kolom kiri, nilai mulai di kolom PANIC_INDENT.
static void p_label(const char* label) {
    panic_cursor_x = PANIC_X;
    p_str(label, C_FAINT);
    panic_cursor_x = PANIC_INDENT;
}

// Baris lanjutan (tanpa label).
static void p_cont_begin(void) { panic_cursor_x = PANIC_INDENT; }

// Gambar satu baris utuh di y tertentu (dipakai baris countdown di bawah layar).
static void p_line(uint32_t y, const char* s, uint32_t fg) {
    panic_cursor_x = PANIC_X;
    panic_cursor_y = y;
    panic_draw_string(s, PANIC_X, y, fg, C_BG);
    panic_cursor_x += panic_strlen(s) * PANIC_CHAR_W;
}

// Label + garis tipis 1px sampai margin kanan (pengganti deretan '----').
static void p_rule(const char* label) {
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
static void p_banner(int repeat) {
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

// --- mirror serial (COM1) -------------------------------------------------
static void ser_hex(uint64_t v) {
    const char* d = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    serial_print(buf);
}

static void ser_dec(uint64_t v) {
    char buf[24];
    panic_u64_dec(v, buf);
    serial_print(buf);
}

// =====================================================================
// 4. NAMA EXCEPTION & VERDICT
// =====================================================================
static const char* const exception_names[15] = {
    "Divide by Zero",         // 0
    "Debug",                  // 1
    "NMI",                    // 2
    "Breakpoint",             // 3
    "Overflow",               // 4
    "Bound Range Exceeded",   // 5
    "INVALID OPCODE",         // 6
    "Device Not Available",   // 7
    "DOUBLE FAULT",           // 8
    "Coprocessor Overrun",    // 9
    "Invalid TSS",            // 10
    "Segment Not Present",    // 11
    "STACK-SEGMENT FAULT",    // 12
    "GENERAL PROTECTION",     // 13
    "PAGE FAULT",             // 14
};

static int panic_is_canonical(uint64_t a) {
    return a < 0x0000800000000000ull || a >= 0xFFFF800000000000ull;
}

static int panic_in_kernel(uint64_t rip) { return rip >= KERNEL_VMA; }

typedef struct {
    const char* verdict;   // baris PENYEBAB
    const char* hint1;     // petunjuk 1 (boleh NULL)
    const char* hint2;     // petunjuk 2 (boleh NULL)
} panic_cause_t;

// Terjemahkan fault menjadi sebab + tindakan. Ini bagian yang paling berguna
// saat membaca BSOD, jadi selalu diisi — bukan sekadar dump angka.
static panic_cause_t panic_explain(uint64_t intno, uint64_t err, uint64_t cr2, uint64_t rip) {
    panic_cause_t c = { "Tidak dapat disimpulkan otomatis",
                        "lihat BACKTRACE + register untuk konteks pemanggil",
                        "(rincian teknis ada di bagian DETAIL di bawah)" };

    if (intno == 14) {                                    // PAGE FAULT
        if (!panic_is_canonical(cr2)) {
            c.verdict = "ALAMAT NON-KANONIKAL - pointer korup / cast salah";
            c.hint1 = "(nilai 32-bit tanpa sign-extend, atau stack rusak)";
            c.hint2 = "return address / function pointer menunjuk ke data";
        } else if (err & 0x10) {                          // instruction fetch
            c.verdict = "FETCH INSTRUKSI dari halaman tanpa izin exec / belum termap";
            c.hint1 = "return address / function pointer menunjuk ke data";
            c.hint2 = "lihat BACKTRACE + register untuk konteks pemanggil";
        } else if (err & 0x08) {                          // reserved bit PTE
            c.verdict = "ENTRY PAGE-TABLE RESERVED RUSAK (bit RSVD)";
            c.hint1 = "korupsi struktur page table";
            c.hint2 = 0;
        } else if (!(err & 0x01)) {                        // halaman belum termap
            if (err & 0x04) {
                c.verdict = "APLIKASI user mengakses alamat BELUM TERMAP";
                c.hint1 = "kemungkinan stack overflow (periksa kedalaman rekursi)";
                c.hint2 = "heap/stack overflow, atau pointer ilegal dari app";
            } else {
                c.verdict = "KERNEL mengakses alamat yang BELUM TERMAP";
                c.hint1 = "kemungkinan dereference pointer NULL / liar";
                c.hint2 = 0;
            }
        } else if (err & 0x02) {                           // present + write
            if (err & 0x04) {
                c.verdict = "PROTECTION VIOLATION pada akses user";
                c.hint1 = "app menulis area read-only (mis. .rodata / kode)";
                c.hint2 = 0;
            } else {
                c.verdict = "KERNEL MENULIS ke halaman READ-ONLY";
                c.hint1 = "umumnya tulis ke .text/.rodata ELF (CoW belum di-copy?)";
                c.hint2 = 0;
            }
        } else {                                           // present + read
            c.verdict = (err & 0x04) ? "PROTECTION VIOLATION pada akses user"
                                     : "PROTECTION VIOLATION (halaman ada, akses ditolak)";
            c.hint1 = "lihat BACKTRACE + register untuk konteks pemanggil";
            c.hint2 = 0;
        }
    } else if (intno == 13) {                              // #GP
        if (err == 0) {
            c.verdict = "GPF dengan selector 0 - eksekusi instruksi privileged";
            c.hint1 = "null selector atau alamat non-kanonik";
            c.hint2 = "periksa GDT/IDT atau segment register yang korup";
        } else {
            c.verdict = "GPF pada selector/segmen tidak sah";
            c.hint1 = "periksa GDT/IDT atau segment register yang korup";
            c.hint2 = "atau alamat non-kanonik di segmen/selector tak sah";
        }
    } else if (intno == 0) {
        c.verdict = "DIVIDE BY ZERO di kernel";
        c.hint1 = "periksa pembagi yang berasal dari input luar";
        c.hint2 = 0;
    } else if (intno == 6) {
        c.verdict = "INVALID OPCODE - kode korup atau instruksi tak didukung";
        c.hint1 = "kernel dibangun -mno-sse: instruksi SSE = kode/mismatch";
        c.hint2 = 0;
    } else if (intno == 8) {
        c.verdict = "DOUBLE FAULT - handler fault sebelumnya gagal";
        c.hint1 = "korupsi struktur page table";
        c.hint2 = "biasanya stack (RSP) rusak / tidak valid saat handler jalan";
    } else if (intno == 11 || intno == 12) {
        c.verdict = "STACK/SEGMENT tidak present - RSP atau SS rusak";
        c.hint1 = "biasanya stack (RSP) rusak / tidak valid saat handler jalan";
        c.hint2 = 0;
    } else {
        c.verdict = "Exception tidak tertangani di kernel";
        c.hint1 = "lihat BACKTRACE + register untuk konteks pemanggil";
        c.hint2 = "dump stack trace lengkap ada di serial COM1 (-serial stdio)";
    }

    // RIP di luar kernel text = lompatan ke pointer tak sah; ini keterangan
    // paling berguna, jadi ditampilkan walau sudah ada hint lain.
    if (!panic_in_kernel(rip)) {
        c.hint2 = "RIP di LUAR kernel text saat CPL=0: kernel melompat ke pointer tak sah";
    }
    return c;
}

// Klasifikasi RIP terhadap kernel text & CPL pemanggil.
static const char* panic_rip_class(uint64_t rip, uint64_t cs) {
    if ((cs & 3u) == 3u) return "   RING-3 (aplikasi user)";
    if (panic_in_kernel(rip)) return "   KERNEL TEXT";
    return "   DI LUAR KERNEL TEXT - lompat ke pointer tak sah";
}

static uint32_t panic_rip_class_color(uint64_t rip, uint64_t cs) {
    if ((cs & 3u) == 3u) return C_KEY;
    return panic_in_kernel(rip) ? C_OK : C_TITLE;
}

// =====================================================================
// 5. BACKTRACE
// =====================================================================
static uint32_t p_trace_count = 0;

static void p_trace_addr(uint64_t addr) {
    p_cont_begin();
    p_str("  #", C_FAINT);
    p_dec(p_trace_count, C_FAINT);
    if (addr >= KERNEL_VMA) {
        p_str(" KERNEL+", C_DIM);
        p_hex(addr - KERNEL_VMA, C_FG);
    } else {
        p_str(" NONKERNEL ", C_DIM);
        p_hex(addr, C_FG);
    }
    p_trace_count++;
}

static void p_backtrace(uint64_t rbp, uint64_t rsp) {
    p_label("BACKTRACE");
    p_trace_count = 0;

    // 1) RBP chain (kalau frame pointer tersimpan; -O2 sering menghilangkannya).
    uint64_t frame = rbp;
    for (uint32_t depth = 0; depth < 6u; depth++) {
        if (!frame || !panic_is_canonical(frame)) break;
        if (!paging_is_mapped(frame) || !paging_is_mapped(frame + 8u)) break;
        uint64_t* f = (uint64_t*)frame;
        uint64_t ret = f[1];
        if (!ret) break;
        p_trace_addr(ret);
        p_newline();
        uint64_t next = f[0];
        if (next <= frame) break;
        frame = next;
    }

    // 2) Fallback: scan stack mencari alamat yang menunjuk ke kernel text.
    if (p_trace_count == 0 && rsp) {
        for (uint32_t i = 0; i < 256u && p_trace_count < 6u; i++) {
            uint64_t a = rsp + (uint64_t)i * 8u;
            if (!panic_is_canonical(a) || !paging_is_mapped(a)) break;
            uint64_t v = *(uint64_t*)a;
            if (v >= KERNEL_VMA && v < rbp + 0x100000ull) {   // dekat = masuk akal
                p_trace_addr(v);
                p_newline();
            }
        }
    }

    if (p_trace_count == 0) {
        p_str("(tidak ada frame valid - RSP/RBP korup?)", C_FAINT);
        p_newline();
    }
}

// =====================================================================
// 6. COUNTDOWN + AUTO-REBOOT
// =====================================================================
// Baris countdown. Digit ada di indeks 18-19 (2 sel = 16x18 px) supaya
// penggambaran ulang per detik tidak menyentuh sisa baris (anti-flicker).
#define CNT_DIGIT_IDX 18u
static const char CNT_LABEL[] = "AUTO-REBOOT dalam ";
static const char CNT_TAIL[]  = " s  (FS sudah diflush)";

static void panic_countdown(void) {
    const display_mode_t* m = display_get_mode();

    if (!m || m->height < 120u) {
        // Layar belum/tidak layak: tidak ada tempat untuk baris countdown.
        // Tunggu kasar (bukan timer, karena IRQ bisa saja sudah mati) lalu reboot.
#ifndef PANIC_HOST_TEST
        for (volatile uint32_t spin = 0; spin < 200000000u; spin++) { }
#endif
        panic_flush_and_reboot();
        return;
    }

    uint32_t y_cnt = m->height - 48u;
    uint32_t y_msg = m->height - 28u;

    // Baris statis digambar SEKALI.
    p_line(y_msg, "Sistem akan restart sendiri. Jangan matikan paksa.", C_FG);

    char cnt[48];
    for (uint32_t i = 0; i < CNT_DIGIT_IDX; i++) cnt[i] = CNT_LABEL[i];   // 18 char
    cnt[CNT_DIGIT_IDX] = ' ';
    cnt[CNT_DIGIT_IDX + 1u] = ' ';
    for (uint32_t i = 0; i < (uint32_t)sizeof(CNT_TAIL); i++)
        cnt[CNT_DIGIT_IDX + 2u + i] = CNT_TAIL[i];                        // + NUL
    p_line(y_cnt, cnt, C_KEY);

    uint64_t start = panic_countdown_tick();
    uint64_t prev  = start;
    uint32_t last_secs = 0xFFFFFFFFu;
    uint32_t mute_spin = 0;
    uint32_t digit_x = PANIC_X + CNT_DIGIT_IDX * PANIC_CHAR_W;

    for (;;) {
        uint64_t now = panic_countdown_tick();

        if (now == prev) {
            // Timer tidak maju (IRQ mati) — jangan menggantung selamanya.
            if (++mute_spin > PANIC_MUTE_SPIN_LIMIT) break;
            panic_sti();          // coba hidupkan lagi IRQ supaya waktu berjalan
            panic_pause();
            continue;
        }
        prev = now;
        mute_spin = 0;

        uint64_t elapsed = now - start;
        if (elapsed >= PANIC_REBOOT_DELAY_MS) break;

        uint32_t secs = (uint32_t)((PANIC_REBOOT_DELAY_MS - elapsed) / 1000u);
        if (secs > 99u) secs = 99u;
        if (secs == last_secs) { panic_sti(); panic_pause(); continue; }
        last_secs = secs;

        // Timpa HANYA 2 sel digit — bukan seluruh baris. Satuan puluhan
        // kosong jadi spasi supaya "9 s" tidak tampil sebagai "09 s".
        char d0 = secs >= 10u ? (char)('0' + (secs / 10u) % 10u) : ' ';
        char d1 = (char)('0' + (secs % 10u));
        p_fill_rect(digit_x, y_cnt, 2u * PANIC_CHAR_W, PANIC_ROW_H, C_BG);
        panic_draw_char(d0, digit_x, y_cnt, C_KEY, C_BG);
        panic_draw_char(d1, digit_x + PANIC_CHAR_W, y_cnt, C_KEY, C_BG);
#ifdef PANIC_HOST_TEST
        g_panic_test_redraws++;
#else
        panic_sti();
        panic_pause();
#endif
    }

    panic_flush_and_reboot();
}

// =====================================================================
// 7. SERIAL MIRROR
// =====================================================================
static void ser_task_context(void) {
    int tid = smp_current_task_id();
    serial_print("\nTASK: ");
    if (tid >= 0 && tid < MAX_TASKS) {
        ser_dec((uint64_t)tid);
        serial_print(" (");
        serial_print(tasks[tid].name[0] ? tasks[tid].name : "(tanpa nama)");
        serial_print(") ");
        serial_print(tasks[tid].kind == TASK_KIND_SPAWNED ? "user" : "kernel");
    } else {
        serial_print("idle/ISR");
    }
    serial_print("  UPTIME(s): ");
    ser_dec(panic_clock_ms() / 1000u);
    serial_print("  LAST SYSCALL: ");
    ser_dec(g_last_syscall_num);
    serial_print(" (task ");
    ser_dec((uint64_t)(g_last_syscall_task < 0 ? 0 : g_last_syscall_task));
    serial_print(")");
}

static void ser_regs(const registers_t* r) {
    serial_print("\nRIP: "); ser_hex(r->rip);
    serial_print("  RSP: "); ser_hex(r->rsp);
    serial_print("  CS: ");  ser_hex(r->cs);
    serial_print("  SS: ");  ser_hex(r->ss);
    serial_print("\nRAX: "); ser_hex(r->rax);
    serial_print("  RBX: "); ser_hex(r->rbx);
    serial_print("  RCX: "); ser_hex(r->rcx);
    serial_print("  RDX: "); ser_hex(r->rdx);
    serial_print("\nRSI: "); ser_hex(r->rsi);
    serial_print("  RDI: "); ser_hex(r->rdi);
    serial_print("  RBP: "); ser_hex(r->rbp);
    serial_print("  R8:  "); ser_hex(r->r8);
    serial_print("\nR9:  "); ser_hex(r->r9);
    serial_print("  R10: "); ser_hex(r->r10);
    serial_print("  R11: "); ser_hex(r->r11);
    serial_print("  R12: "); ser_hex(r->r12);
    serial_print("\nR13: "); ser_hex(r->r13);
    serial_print("  R14: "); ser_hex(r->r14);
    serial_print("  R15: "); ser_hex(r->r15);
    serial_print("\nCR3: "); ser_hex(panic_read_cr3());
    serial_print("\n");
}

// =====================================================================
// 8. LAYAR BSOD — EXCEPTION
// =====================================================================
void exception_handler(registers_t* r) {
    if (!r) return;

    panic_cli();

#ifdef HEAP_WATCH_DEBUG
    // Vector 1 (#DB): hardware watchpoint dari heap_watch — log ke serial dan
    // lanjutkan eksekusi (stub akan iretq), bukan BSOD.
    if (r->int_num == 1) {
        extern void heap_watch_db_handler(registers_t* r);
        heap_watch_db_handler(r);
        return;
    }
#endif

    int repeat = panic_is_locked();
    panic_lockdown();                     // hentikan semua penulis scanout lain

    uint64_t int_num    = r->int_num;
    uint64_t error_code = r->error_code;
    uint64_t cr2        = (int_num == 14) ? panic_read_cr2() : 0;
    uint64_t cr3        = panic_read_cr3();
    uint64_t uptime_s   = panic_clock_ms() / 1000u;

    // --- mirror serial (selalu verbose; ini tempat dump lengkap) ---
    serial_print("\n\n==== KERNEL PANIC (serial dump) ====\nEXCEPTION: ");
    serial_print(int_num < 15 ? exception_names[int_num] : "Unknown");
    serial_print("\nINT: ");
    ser_dec(int_num);
    serial_print("  ERR: ");
    ser_hex(error_code);
    if (int_num == 14) {
        serial_print("\nCR2: ");
        ser_hex(cr2);
        serial_print("  PF: ");
        serial_print(paging_is_mapped(cr2) ? "PROT" : "NONP");
        serial_print(error_code & 2u ? " WRITE" : " READ");
        serial_print(error_code & 4u ? " USER" : " KERNEL");
    }
    ser_regs(r);
    ser_task_context();
    serial_print("\n");

    if (!fb_ptr || !display_get_mode()) {           // tidak bisa menggambar
        serial_print("HINT: framebuffer tidak siap - lihat dump di atas\n");
        panic_countdown();
        panic_freeze();
        return;
    }

    panic_cause_t cause = panic_explain(int_num, error_code, cr2, r->rip);

    fill_screen(C_BG);
    p_home();
    p_banner(repeat);
    p_newline();

    // --- PENYEBAB (headline) -----------------------------------------
    p_label("PENYEBAB");
    p_str(cause.verdict, C_TITLE);
    p_newline();
    if (cause.hint1) { p_cont_begin(); p_str(cause.hint1, C_KEY); p_newline(); }
    if (cause.hint2) { p_cont_begin(); p_str(cause.hint2, C_DIM); p_newline(); }
    p_newline();

    // --- DI MANA ------------------------------------------------------
    p_label("DI MANA");
    int tid = smp_current_task_id();
    if (tid >= 0 && tid < MAX_TASKS) {
        p_str("task #", C_DIM);
        p_dec((uint64_t)tid, C_KEY);
        if (tasks[tid].name[0]) {
            p_str(" \"", C_DIM);
            p_str(tasks[tid].name, C_FG);
            p_str("\" (", C_DIM);
            p_str(tasks[tid].kind == TASK_KIND_SPAWNED ? "user" : "kernel", C_DIM);
            p_str(")", C_DIM);
        } else {
            p_str(" (tanpa nama)", C_DIM);
        }
    } else {
        p_str("(tidak ada task aktif - konteks idle/ISR)", C_DIM);
    }
    p_str("   CPU ", C_DIM);
    p_dec(smp_current_cpu_index(), C_KEY);
    p_str("/", C_DIM);
    p_dec(smp_online_cpu_count(), C_FG);
    p_newline();

    p_cont_begin();
    p_str("RIP ", C_DIM);
    p_hex(r->rip, C_KEY);
    p_str(panic_rip_class(r->rip, r->cs), panic_rip_class_color(r->rip, r->cs));
    p_str("   uptime ", C_DIM);
    p_dec(uptime_s, C_FG);
    p_str("s", C_DIM);
    p_newline();
    p_newline();

    // --- DETAIL (arsip; sengaja redup) --------------------------------
    p_rule("DETAIL (buka hanya bila perlu)");

    p_label("FAULT");
    p_str(int_num < 15 ? exception_names[int_num] : "Unknown", C_FG);
    p_str(" (INT ", C_DIM);
    p_dec(int_num, C_FG);
    p_str(")  ERR ", C_DIM);
    p_hex(error_code, C_FG);
    p_str("  [P=", C_DIM);
    p_str((error_code & 1u) ? "1" : "0", C_FG);
    p_str(" W=", C_DIM);
    p_str((error_code & 2u) ? "1" : "0", C_FG);
    p_str(" U=", C_DIM);
    p_str((error_code & 4u) ? "1" : "0", C_FG);
    p_str(" RSVD=", C_DIM);
    p_str((error_code & 8u) ? "1" : "0", C_FG);
    p_str(" ID=", C_DIM);
    p_str((error_code & 16u) ? "1" : "0", C_FG);
    p_str("]", C_DIM);
    p_newline();

    if (int_num == 14) {
        p_label("TARGET");
        p_str("CR2 ", C_DIM);
        p_hex(cr2, C_KEY);
        p_str("   ", C_DIM);
        if (!panic_is_canonical(cr2))
            p_str("NON-KANONIKAL (alamat mustahil)", C_TITLE);
        else if (error_code & 1u)
            p_str("PROTECTION VIOLATION (halaman ada, akses ditolak)", C_FG);
        else
            p_str("NOT PRESENT (alamat belum termap)", C_FG);
        p_str("   ", C_DIM);
        p_str((error_code & 4u) ? "USER" : "KERNEL/HIGHER-HALF", C_DIM);
        if (error_code & 16u)      p_str("  INSTRUCTION FETCH", C_DIM);
        else if (error_code & 2u)  p_str("  OP: WRITE", C_DIM);
        else                       p_str("  OP: READ", C_DIM);
        p_newline();

        // Bit P dari CPU itu otoritatif; beda dengan paging_is_mapped() bisa
        // berarti TLB/CR3 tidak sinkron (atau alamat milik AS task lain).
        if (paging_is_mapped(cr2) != (int)(error_code & 1u)) {
            p_cont_begin();
            p_str("catatan: paging_is_mapped() menilai berbeda dari bit P (TLB/CR3?)", C_FAINT);
            p_newline();
        }
    } else if (int_num == 13) {
        p_label("GPF");
        if (error_code == 0) {
            p_str("null selector atau alamat non-kanonik", C_TITLE);
            p_newline();
        } else {
            uint64_t tbl = (error_code >> 1) & 3u;
            p_str("selector index ", C_DIM);
            p_dec((error_code >> 3) & 0x1FFFu, C_KEY);
            p_str(tbl == 0 ? " (GDT)" : (tbl == 2 ? " (LDT)" : " (IDT)"), C_DIM);
            p_newline();
        }
    } else {
        p_label("TARGET");
        p_str("CS ", C_DIM);
        p_hex(r->cs, C_FG);
        p_str("   RFLAGS ", C_DIM);
        p_hex(r->rflags, C_FG);
        p_newline();
    }

    p_backtrace(r->rbp, r->rsp);

    // --- REGISTERS (4 baris x 4 register) -----------------------------
    p_label("REGISTERS");
    p_str("RAX ", C_DIM); p_hex(r->rax, C_FG);
    p_str(" RBX ", C_DIM); p_hex(r->rbx, C_FG);
    p_str(" RCX ", C_DIM); p_hex(r->rcx, C_FG);
    p_str(" RDX ", C_DIM); p_hex(r->rdx, C_FG);
    p_newline();

    p_cont_begin();
    p_str("RSI ", C_DIM); p_hex(r->rsi, C_FG);
    p_str(" RDI ", C_DIM); p_hex(r->rdi, C_FG);
    p_str(" RBP ", C_DIM); p_hex(r->rbp, C_FG);
    p_str(" R8  ", C_DIM); p_hex(r->r8, C_FG);
    p_newline();

    p_cont_begin();
    p_str("R9 ", C_DIM);  p_hex(r->r9, C_FG);
    p_str(" R10 ", C_DIM); p_hex(r->r10, C_FG);
    p_str(" R11 ", C_DIM); p_hex(r->r11, C_FG);
    p_str(" R12 ", C_DIM); p_hex(r->r12, C_FG);
    p_newline();

    p_cont_begin();
    p_str("R13 ", C_DIM); p_hex(r->r13, C_FG);
    p_str(" R14 ", C_DIM); p_hex(r->r14, C_FG);
    p_str(" R15 ", C_DIM); p_hex(r->r15, C_FG);
    p_str(" RSP ", C_DIM); p_hex(r->rsp, C_FG);
    p_newline();

    // --- STATE & MEM ---------------------------------------------------
    p_label("STATE");
    p_str("CR3 ", C_DIM);  p_hex(cr3, C_FG);
    p_str(" CS ", C_DIM);  p_hex(r->cs, C_FG);
    p_str("   tasks ", C_DIM); p_dec((uint64_t)task_count, C_FG);
    if (tid >= 0 && tid < MAX_TASKS && tasks[tid].parent_id >= 0) {
        p_str("   parent #", C_DIM);
        p_dec((uint64_t)tasks[tid].parent_id, C_FG);
    }
    p_str("   last syscall ", C_DIM);
    p_dec(g_last_syscall_num, C_KEY);
    p_str(" (task ", C_DIM);
    p_dec((uint64_t)(g_last_syscall_task < 0 ? 0 : g_last_syscall_task), C_FG);
    p_str(")", C_DIM);
    p_newline();

    p_label("MEM");
    p_dec(pmm_get_used_pages(), C_KEY);
    p_str(" / ", C_DIM);
    p_dec(pmm_get_total_pages(), C_FG);
    p_str(" pages (", C_DIM);
    p_dec((pmm_get_used_pages() * 4096u) / 1024u, C_KEY);
    p_str(" / ", C_DIM);
    p_dec((pmm_get_total_pages() * 4096u) / 1024u, C_FG);
    p_str(" KB)", C_DIM);
    p_newline();

    panic_countdown();
    panic_freeze();
}

// =====================================================================
// 9. GENERIC KERNEL PANIC (dipanggil manual dari kode kernel)
// =====================================================================
__attribute__((weak))
void kernel_panic(const char* title, const char* desc, uint64_t code) {
    panic_cli();
    int repeat = panic_is_locked();
    panic_lockdown();

    // --- mirror serial ---
    serial_print("\n\n==== KERNEL PANIC (serial dump) ====\nREASON: ");
    serial_print(title ? title : "(null)");
    serial_print("\nDETAILS: ");
    serial_print(desc ? desc : "(null)");
    serial_print("\nCODE: ");
    ser_hex(code);
    serial_print("\n");
    ser_task_context();
    serial_print("\n");

    if (!fb_ptr || !display_get_mode()) {
        panic_countdown();
        panic_freeze();
        return;
    }

    fill_screen(C_BG);
    p_home();
    p_banner(repeat);
    p_newline();

    p_label("PENYEBAB");
    p_str(title ? title : "(tanpa judul)", C_TITLE);
    p_newline();
    p_cont_begin();
    p_str(desc ? desc : "(tanpa keterangan)", C_KEY);
    p_newline();
    p_newline();

    p_label("DI MANA");
    int tid = smp_current_task_id();
    if (tid >= 0 && tid < MAX_TASKS && tasks[tid].name[0]) {
        p_str("task #", C_DIM);
        p_dec((uint64_t)tid, C_KEY);
        p_str(" \"", C_DIM);
        p_str(tasks[tid].name, C_FG);
        p_str("\"", C_DIM);
    } else {
        p_str("(tidak ada task aktif - konteks idle/ISR)", C_DIM);
    }
    p_str("   CPU ", C_DIM);
    p_dec(smp_current_cpu_index(), C_KEY);
    p_str("/", C_DIM);
    p_dec(smp_online_cpu_count(), C_FG);
    p_newline();
    p_newline();

    p_rule("DETAIL (buka hanya bila perlu)");

    p_label("CODE");
    p_hex(code, C_KEY);
    p_newline();

    p_label("PETUNJUK");
    p_str("dump stack trace lengkap ada di serial COM1 (-serial stdio)", C_DIM);
    p_newline();

    p_label("BACKTRACE");
    p_trace_count = 0;
    p_str("(tidak ada frame valid - RSP/RBP korup?)", C_FAINT);
    p_newline();

    p_label("STATE");
    p_str("CR3 ", C_DIM);
    p_hex(panic_read_cr3(), C_FG);
    p_str("   tasks ", C_DIM);
    p_dec((uint64_t)task_count, C_FG);
    p_str("   last syscall ", C_DIM);
    p_dec(g_last_syscall_num, C_KEY);
    p_newline();

    p_label("MEM");
    p_dec(pmm_get_used_pages(), C_KEY);
    p_str(" / ", C_DIM);
    p_dec(pmm_get_total_pages(), C_FG);
    p_str(" pages terpakai", C_DIM);
    p_newline();

    panic_countdown();
    panic_freeze();
}

// =====================================================================
// 10. SHORTCUT / KOMPATIBILITAS
// =====================================================================
void page_fault_handler(registers_t* r, uint64_t fault_addr) {
    (void)fault_addr;   // CR2 sudah dibaca langsung di exception_handler
    exception_handler(r);
}
