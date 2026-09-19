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
//   * TANPA IRQ. Seluruh jalur panic berjalan dengan `cli`, jadi TIDAK boleh
//     ada ketergantungan pada IRQ timer/keyboard:
//       - waktu diukur dengan mem-polling PIT channel 2 (diprogram jadi rate
//         generator ~10 ms) — lihat bagian 1b;
//       - tombol dibaca dengan mem-polling controller PS/2 (0x64/0x60) — 1c.
//     Countdown TIDAK memakai timer_get_ms() dan tidak pernah `sti` lagi.
//   * INTERAKTIF. [R] reboot, [S] shutdown (ACPI S5 -> port emulator), [D]
//     freeze supaya layar BSOD bisa dibaca. Petunjuk tombol = baris PALING
//     BAWAH. Tanpa input, PANIC_REBOOT_DELAY_MS tetap jalan sebagai jaring
//     pengaman (mesin tanpa operator tetap pulih).
//   * PERSISTENSI (dua jalur, saling melengkapi):
//       - RAM : log ringkas di halaman PMM pertama (deterministik lintas
//               warm-reboot) -> dilaporkan panic_check_previous_log() saat boot;
//       - DISK: crashdump raw 4KB ke ekor disk (kernel/crashdump.c) supaya
//               selamat dari power cycle.
//   * GUARD PANIC BERSARANG. Kalau panic terjadi lagi saat handler masih
//     berjalan (mis. I/O crashdump memicu fault), crashdump dibatalkan dan
//     sistem langsung dibekukan — tidak menggambar/menulis apa pun lagi.
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
#include "crashdump.h" // crashdump_* — snapshot ke sektor disk
#include "task.h"      // registers_t, task_t, tasks[], smp_current_task_id()
#include "serial.h"    // serial_print (mirror COM1)
#include "display.h"   // display_get_mode()
#include "io.h"        // inb/outb/outw (polling PIT + PS/2 + port power-off)
#include "acpi.h"      // acpi_poweroff_raw() — ACPI S5 untuk aksi [S]

// --- Dependensi eksternal -------------------------------------------------
extern uint32_t* fb_ptr;
extern unsigned char font8x16[][16];   // definisi: kernel/gfx/fb.c

// CATATAN PENTING: seluruh jalan panic WAJIB memakai varian _nolock/_try di
// bawah. Versi ber-lock (paging_is_mapped, pmm_get_*_pages, kfs_sync_all) bisa
// self-deadlock kalau CPU yang fault ternyata sedang memegang lock tsb —
// gejalanya: sistem membeku, BSOD tidak pernah muncul, tidak ada reboot.
extern int      paging_is_mapped_nolock(uint64_t vaddr);
extern uint32_t smp_current_cpu_index(void);
extern uint32_t smp_online_cpu_count(void);
extern uint64_t pmm_get_used_pages_nolock(void);
extern uint64_t pmm_get_total_pages_nolock(void);
extern uint64_t timer_get_ms(void);
extern int      kfs_sync_all_try(void);

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

#define PANIC_UI_MIN_HEIGHT    140u      // di bawah ini: tanpa baris info/petunjuk tombol
// Host test: loop tombol di kernel bersifat TERMINAL (tidak pernah kembali),
// jadi seam uji butuh jalan keluar setara supaya bisa diperiksa bahwa tanpa
// input memang TIDAK ada reboot. Tidak dipakai di build kernel.
#define PANIC_HOST_IDLE_LOOPS  3u
#define KERNEL_VMA             0xFFFFFFFF80000000ull   // lihat linker.ld

// =====================================================================
// 0. LOCKDOWN & GUARD (dideklarasikan paling awal: seam host test meresetnya)
// =====================================================================
static volatile uint32_t g_panic_lockdown = 0;
// Guard panic bersarang: 1 = handler panic sedang berjalan pada CPU ini.
static volatile uint32_t g_panic_active = 0;

// =====================================================================
// 1. PRIMITIF PRIVILEGED (di-stub kalau PANIC_HOST_TEST)
// =====================================================================
#ifdef PANIC_HOST_TEST
// Jam palsu: panic_clock_ms() membaca apa adanya (untuk uptime/label waktu),
// sedangkan panic_idle_slice() memajukannya 1 detik per slice supaya countdown
// 10 detik selesai deterministik tanpa menunggu 10 detik nyata.
volatile uint64_t g_panic_test_ms        = 0;
volatile uint64_t g_panic_test_cr2       = 0;
volatile int32_t  g_panic_test_key       = 0;
volatile int32_t  g_panic_test_aux       = 0;   // byte mouse mengantre di depan

volatile uint32_t g_panic_test_reboots   = 0;
volatile uint32_t g_panic_test_shutdowns = 0;
volatile uint32_t g_panic_test_freezes   = 0;
volatile uint32_t g_panic_test_waits     = 0;

static void (*g_reboot_hook)(void) = 0;

void panic_host_test_reset(void) {
    g_panic_lockdown = 0;            // dideklarasikan di bagian 0
    g_panic_active = 0;
    g_panic_test_ms = 0;
    g_panic_test_cr2 = 0;
    g_panic_test_key = 0;
    g_panic_test_aux = 0;

    g_panic_test_reboots = 0;
    g_panic_test_shutdowns = 0;
    g_panic_test_freezes = 0;
    g_panic_test_waits = 0;
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

// TIDAK ada panic_sti(): jalur panic tidak pernah menghidupkan IRQ lagi.
// Pewaktu dan input di-polling (bagian 1b & 1c).

// Hanya dipakai jalur kernel (panic_reset_hw/panic_freeze) — di host test
// tidak ada instruksi privileged yang dijalankan.
#ifndef PANIC_HOST_TEST
static void panic_halt(void) {
    __asm__ volatile("hlt");
}
#endif

// (unused di host test: jalur polling PIT/PS-2 di-stub — lihat bagian 1b/1c.)
static void __attribute__((unused)) panic_pause(void) {
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

// =====================================================================
// 1b. PEWAKTU TANPA IRQ: polling PIT channel 2
// =====================================================================
// Kenapa channel 2 dan bukan channel 0? Channel 0 adalah sumber IRQ0 — kita
// sudah meng-`cli` dan tidak boleh lagi mengandalkan IRQ. Channel 2 hanya
// tersambung ke speaker (TIDAK menghasilkan IRQ) dan status pulsenya bisa
// dibaca lewat port 0x61, jadi aman di-polling dari jalur panic.
// Diprogram mode 3 (square wave) reload 11932 -> 100 Hz: tiap transisi level
// = 5 ms. Resolusi 5 ms cukup untuk hitung mundur.
#define PIT_FREQ_HZ        1193182u
#define PIT2_HZ            100u
#define PIT2_RELOAD        ((uint16_t)(PIT_FREQ_HZ / PIT2_HZ))  // 11931
#define PIT2_MS_PER_EDGE   5u                                  // setengah periode = 5 ms
#define PIT2_POLL_LIMIT    4000000u   // PIT mati: batas iterasi polling satu slice
#define PIT2_SPIN_FALLBACK 800000u    // ~10 ms kasar di CPU modern

static uint64_t g_pit2_edges = 0;     // jumlah transisi level PIT2

static void pit2_start(void) {
#ifndef PANIC_HOST_TEST
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & 0xFCu) | 0x01u));  // gate2 on, speaker off
    outb(0x43, 0xB6);                              // ch2 | lobyte/hibyte | mode3
    outb(0x42, (uint8_t)(PIT2_RELOAD & 0xFFu));
    outb(0x42, (uint8_t)((PIT2_RELOAD >> 8) & 0xFFu));
#endif
}

// (unused di host test — panic_idle_slice di sana memakai jam palsu.)
static uint8_t __attribute__((unused)) pit2_level(void) {
#ifndef PANIC_HOST_TEST
    return (uint8_t)((inb(0x61) >> 5) & 0x01u);
#else
    return 0;
#endif
}

// Milidetik monotonic sejak panic_monotonic_reset(). Bebas IRQ: dihitung dari
// transisi level PIT2 yang kita amati sendiri. (Non-static: bagian dari API
// panic.h supaya loop interaktif bisa diuji dari luar.)
uint64_t panic_monotonic_ms(void) {
#ifdef PANIC_HOST_TEST
    return g_panic_test_ms;
#else
    return g_pit2_edges * (uint64_t)PIT2_MS_PER_EDGE;
#endif
}

void panic_monotonic_reset(void) {
    g_pit2_edges = 0;
    pit2_start();
}

// Tunggu satu slice (~5-10 ms) sambil mem-polling PIT2. Kalau PIT2 tidak
// bergerak (tidak ada hardware legacy), pakai spin kalibrasi supaya countdown
// tetap maju — bukan menggantung selamanya.
static void panic_idle_slice(void) {
#ifdef PANIC_HOST_TEST
    g_panic_test_ms += 1000;      // 1 slice = 1 detik (deterministik)
    g_panic_test_waits++;
    return;
#else
    uint8_t lvl = pit2_level();
    uint32_t guard = 0;
    while (pit2_level() == lvl) {
        panic_pause();
        if (++guard > PIT2_POLL_LIMIT) break;
    }
    if (guard > PIT2_POLL_LIMIT) {
        for (volatile uint32_t s = 0; s < PIT2_SPIN_FALLBACK; s++) { }
        g_pit2_edges += 2u;       // ~10 ms dari spin
    } else {
        g_pit2_edges += 1u;       // satu transisi = 5 ms
    }
#endif
}

// =====================================================================
// 1c. INPUT TANPA IRQ: polling controller PS/2 (0x64/0x60)
// =====================================================================
// Protokol status: bit0 (OBF) = ada byte untuk dibaca, bit5 (AUXB) = byte itu
// milik mouse. Scancode keyboard = set 1; prefix 0xE0/0xE1 dan break code
// (bit7) diabaikan, jadi tombol hanya terpicu SEKALI per penekanan.
#define PS2_STATUS_OBF 0x01u
#define PS2_STATUS_AUX 0x20u

static uint8_t g_key_ext = 0;         // sedang di dalam urutan extended?

// Hasil satu pembacaan controller PS/2.
enum { PS2_NONE = 0, PS2_KEY = 1, PS2_MOUSE = 2 };

// Baca SATU byte dari controller 8042. Byte MOUSE pun WAJIB dibaca (isinya
// dibuang): kalau byte mouse dibiarkan di output buffer, OBF tetap penuh dan
// scancode keyboard berikutnya tidak pernah sampai — tombol [R]/[S] jadi
// terasa "mati" begitu mouse digerakkan di layar BSOD. Dulu fungsi ini
// `return 0` untuk byte mouse TANPA membaca port 0x60, dan itulah bugnya.
static int panic_ps2_read(uint8_t* sc) {
#ifdef PANIC_HOST_TEST
    // Model controller untuk host test: g_panic_test_aux = jumlah byte mouse
    // yang mengantre di depan, lalu g_panic_test_key = satu scancode.
    if (g_panic_test_aux > 0) { g_panic_test_aux--; return PS2_MOUSE; }
    if (g_panic_test_key) { *sc = (uint8_t)g_panic_test_key; g_panic_test_key = 0; return PS2_KEY; }
    return PS2_NONE;
#else
    uint8_t st = inb(0x64);
    if (!(st & PS2_STATUS_OBF)) return PS2_NONE;
    uint8_t b = inb(0x60);          // SELALU dibaca: inilah yang mengosongkan OBF
    if (st & PS2_STATUS_AUX) return PS2_MOUSE;   // byte mouse → buang
    *sc = b;
    return PS2_KEY;
#endif
}

// Buang byte sisa ketikan & gerakan mouse sebelum panic supaya tidak salah tafsir.
static void panic_keys_drain(void) {
#ifdef PANIC_HOST_TEST
    // Di host, g_panic_test_key = tombol yang ditekan SETELAH panic (jadi tidak
    // boleh ikut dibuang); yang di-flush hanya byte mouse yang mengantre.
    g_panic_test_aux = 0;
#else
    for (uint32_t i = 0; i < 64u; i++) {
        uint8_t sc;
        if (panic_ps2_read(&sc) == PS2_NONE) break;
    }
#endif
}

// Scancode set-1 -> 'r'/'s' (0 kalau bukan tombol kita).
static char panic_key_translate(uint8_t sc) {
    if (sc == 0xE0u || sc == 0xE1u) { g_key_ext = 1u; return 0; }  // prefix
    if (sc & 0x80u) { g_key_ext = 0u; return 0; }                  // key release
    if (g_key_ext) { g_key_ext = 0u; return 0; }                   // extended make
    if (sc == 0x13u) return 'r';                                   // 0x13 = R
    if (sc == 0x1Fu) return 's';                                   // 0x1F = S
    return 0;                                                      // tombol lain: abaikan
}

// Baca sampai beberapa byte; return 'r'/'s' atau 0 kalau tidak ada aksi.
int panic_read_key(void) {
    for (uint32_t i = 0; i < 32u; i++) {
        uint8_t sc;
        int kind = panic_ps2_read(&sc);
        if (kind == PS2_NONE) break;
        if (kind == PS2_MOUSE) continue;     // byte mouse: dibuang, cari keyboard lagi
        char c = panic_key_translate(sc);
        if (c) return c;
    }
    return 0;
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
    // 1) Chipset Reset Control Register (0xCF9) — paling andal di QEMU
    //    (i440fx/q35) MAUPUN hardware nyata. 0x06 = SYS_RST | RST_CPU.
    //    Urutan ini penting: di QEMU, reset lewat keyboard controller (0x64,
    //    0xFE) DIABAIKAN — sistem tampak "freeze" setelah countdown padahal
    //    seluruh jalur panic sudah selesai. Karena itu 0xCF9 dicoba lebih dulu.
    serial_print("[PANIC] reset via chipset 0xCF9\n");
    __asm__ volatile("outb %0, %1" ::"a"((uint8_t)0x06), "Nd"((uint16_t)0x0CF9));

    // 2) Keyboard controller (hardware lama yang hanya punya jalur ini).
    serial_print("[PANIC] reset via 8042 0xFE\n");
    __asm__ volatile("outb %0, %1" ::"a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));

    // 3) Cadangan terakhir: IDT kosong + int3 = triple fault = reset CPU.
    serial_print("[PANIC] reset via triple fault\n");
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } null_idt = { 0, 0 };
    __asm__ volatile("cli");
    __asm__ volatile("lidt %0" ::"m"(null_idt));
    __asm__ volatile("int3");
    for (;;) panic_halt();
}
#endif

// Simpan data user, lalu reboot. Di host test cukup memanggil hook + kembali.
static void panic_flush_and_reboot(void) {
    // BEST-EFFORT, tanpa menunggu lock: kalau CPU yang fault memang memegang
    // lock FS/bcache, sync biasa akan self-deadlock (BSOD muncul tapi reboot
    // tidak pernah jalan). Data yang sudah ter-flush tetap aman.
    if (!kfs_sync_all_try())
        serial_print("[PANIC] sync FS dilewati (lock sedang dipakai) - tetap reboot\n");
#ifdef PANIC_HOST_TEST
    g_panic_test_reboots++;
    if (g_reboot_hook) g_reboot_hook();
    return;                         // kontrol kembali ke test
#else
    if (g_reboot_hook) g_reboot_hook();
    panic_reset_hw();               // tidak kembali
#endif
}

// Bekukan sistem (mode debug / panic bersarang). Di kernel TIDAK kembali;
// di host test kembali supaya bisa diverifikasi.
static void panic_freeze(void) {
#ifdef PANIC_HOST_TEST
    g_panic_test_freezes++;         // dihitung di sini agar SEMUA jalur beku tercatat
    return;
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

// Pita peringatan di atas BSOD yang SUDAH tergambar. Dipakai jalur "panic
// bersarang": layar panic pertama tetap berlaku, tapi pengguna harus tahu
// kenapa crashdump/reboot tidak jalan. Best-effort: kalau framebuffer tidak
// siap, fungsi ini tidak melakukan apa-apa (serial tetap dapat laporannya).
static void panic_overlay_banner(const char* msg) {
    const display_mode_t* m = display_get_mode();
    if (!fb_ptr || !m || m->height < PANIC_UI_MIN_HEIGHT) return;

    // Baris STATUS (di atas baris countdown) — bukan di tepi atas layar: di
    // atas sana pita bertabrakan dengan banner judul BSOD dan tampak
    // "terlalu tinggi". Di baris status ia terbaca sebagai catatan kaki, dan
    // saat panic bersarang baris itu memang kosong (loop countdown tidak jalan).
    const uint32_t x = PANIC_X;
    const uint32_t y = m->height - 68u;
    uint32_t avail   = panic_strlen(msg);
    uint32_t max_ch  = (m->width > x + 16u) ? (m->width - x - 16u) / PANIC_CHAR_W : 0u;
    if (avail > max_ch) avail = max_ch;          // potong, jangan lewat margin

    // Latar = C_BG (bukan warna khusus): teks tetap terbaca di layar DAN tetap
    // bisa di-decode `panic_test --dump` (decoder mencocokkan tiap sel 8x16).
    p_fill_rect(x, y, avail * PANIC_CHAR_W, PANIC_ROW_H, C_BG);
    for (uint32_t i = 0; i < avail; i++)
        panic_draw_char(msg[i], x + i * PANIC_CHAR_W, y, C_TITLE, C_BG);
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

// =====================================================================
// 5a. ATURAN ALAMAT: jalur panic hanya boleh MEMBACA higher-half
// =====================================================================
// Pelajaran dari QEMU: fault aplikasi user (ring 3) meninggalkan RSP/RBP di
// alamat USER (mis. RSP=0x0BFBFFB8). Membaca alamat itu dari ring 0 saat SMAP
// aktif menghasilkan #PF KEDUA -> guard panic bersarang -> layar BSOD
// terpotong di baris BACKTRACE lalu sistem freeze. Jadi: jangan pernah
// dereference alamat di bawah higher-half dari jalur panic, sekalipun
// paging_is_mapped_nolock() menilainya "mapped" (PML4 proses memang memetakan
// alamat user, dan itu penilaian yang benar — tapi tetap tidak boleh dibaca).
#define PANIC_HIGHER_HALF 0xFFFF800000000000ull

static int panic_ptr_readable(uint64_t a) {
    return panic_is_canonical(a) && a >= PANIC_HIGHER_HALF;
}

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

static void p_backtrace(uint64_t rbp, uint64_t rsp, uint64_t cs) {
    p_label("BACKTRACE");
    p_trace_count = 0;

    // Fault dari ring 3: stack/tumpukan yang aktif milik aplikasi user. Menelusuri
    // alamat user dari kernel tidak menghasilkan frame yang berguna DAN
    // melanggar SMAP, jadi lewati secara eksplisit (bukan coba-coba baca).
    if ((cs & 3u) == 3u) {
        p_str("(konteks user - backtrace kernel dilewati, stack di alamat user)", C_FAINT);
        p_newline();
        return;
    }

    // 1) RBP chain (kalau frame pointer tersimpan; -O2 sering menghilangkannya).
    uint64_t frame = rbp;
    for (uint32_t depth = 0; depth < 6u; depth++) {
        if (!frame || !panic_ptr_readable(frame)) break;
        if (!paging_is_mapped_nolock(frame) || !paging_is_mapped_nolock(frame + 8u)) break;
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
            if (!panic_ptr_readable(a) || !paging_is_mapped_nolock(a)) break;
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
// 6. LOOP INTERAKTIF (tanpa IRQ) — TANPA AUTO-REBOOT
// =====================================================================
// SENGAJA tidak ada hitung mundur / reboot otomatis: sistem berhenti di layar
// BSOD dan menunggu operator memilih tindakan. Reboot otomatis membuat bukti
// crash hilang sebelum dibaca (dan di QEMU menghasilkan boot-loop yang
// menyembunyikan penyebab pertama — persis yang bikin diagnosis lambat).
//
// Baris info (di atas baris tombol): dua varian sesuai hasil sync FS.
static const char INFO_HOLD[]   = "TIDAK ada reboot otomatis - sistem menunggu pilihan tombol";
static const char INFO_NOSYNC[] = "TIDAK ada reboot otomatis - sync FS dilewati (lock dipakai)";
// Petunjuk tombol = baris PALING BAWAH, supaya mata langsung menemukan
// pilihan tindakan.
static const char HINT_KEYS[] = "[R] reboot   [S] shutdown";

// Baris status di atas countdown: umpan balik sesaat sebelum sistem mati/beku.
static void panic_status_line(const char* msg) {
    const display_mode_t* m = display_get_mode();
    if (!m || m->height < PANIC_UI_MIN_HEIGHT || !fb_ptr) return;
    uint32_t y = m->height - 68u;
    uint32_t w = (m->width > 2u * PANIC_X) ? (m->width - 2u * PANIC_X) : 0u;
    p_fill_rect(PANIC_X, y, w, PANIC_ROW_H, C_BG);
    panic_draw_string(msg, PANIC_X, y, C_FG, C_BG);
}

// =====================================================================
// 6b. AKSI INTERAKTIF: [R] reboot, [S] shutdown
// (Tidak ada tombol freeze: tanpa auto-reboot, tidak menekan apa pun SUDAH
// berarti layar BSOD bertahan selamanya — tombol itu tidak menambah apa pun.)
// =====================================================================
void panic_action_reboot(void) {
    serial_print("\n[PANIC] [R] reboot diminta pengguna\n");
    panic_status_line("REBOOT diminta - menyimpan data lalu reset...");
    panic_flush_and_reboot();          // sync best-effort lalu reset hardware
}

void panic_action_shutdown(void) {
    serial_print("\n[PANIC] [S] shutdown diminta pengguna\n");
    panic_status_line("SHUTDOWN diminta - menyimpan data lalu mematikan daya...");
    if (!kfs_sync_all_try())           // best-effort: jangan menggantung di lock
        serial_print("[PANIC] sync FS dilewati (lock sedang dipakai) - tetap shutdown\n");
#ifndef PANIC_HOST_TEST
    // Rantai power-off: ACPI S5 dari FADT (hardware nyata), lalu port emulator.
    // APM (int 15h) tidak tersedia dari long mode — karena itu rantainya
    // berhenti di ACPI + port emulator.
    (void)acpi_poweroff_raw();
    outw(0xB004, 0x2000);              // Bochs / QEMU (lama)
    outw(0x0604, 0x2000);              // QEMU (modern)
    outw(0x4004, 0x3400);              // VirtualBox
    panic_status_line("Power-off tidak didukung - sistem dibekukan (tekan RESET).");
#else
    g_panic_test_shutdowns++;
#endif
    panic_freeze();                    // kernel: halt; host test: kembali
}


// =====================================================================
// 6c. PERSISTENSI: log RAM + crashdump disk (keduanya zero-allocation)
// =====================================================================
#define PANIC_DUMP_TEXT_MAX 3072u      // payload crashdump (4KB - header)
static char g_dump_text[PANIC_DUMP_TEXT_MAX];

// Penulis teks minimal ke buffer tetap (tanpa libc, tanpa alokasi).
static uint32_t dt_put(char* b, uint32_t cap, uint32_t at, const char* s) {
    while (*s && at + 1u < cap) b[at++] = *s++;
    b[at] = 0;
    return at;
}

static uint32_t dt_dec(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) { return dt_put(b, cap, at, "0"); }
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && at + 1u < cap) { b[at++] = tmp[--n]; }
    b[at] = 0;
    return at;
}

static uint32_t dt_hex(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    static const char H[] = "0123456789ABCDEF";
    if (at + 19u >= cap) return at;
    b[at++] = '0'; b[at++] = 'x';
    for (int i = 15; i >= 0; i--) b[at++] = H[(v >> (i * 4)) & 0xFull];
    b[at] = 0;
    return at;
}

// Nama task (kalau tabel task belum siap / tid tidak valid -> "").
static void panic_task_name(int tid, char* out, uint32_t cap) {
    out[0] = 0;
    if (cap < 2u || tid < 0) return;
    if ((uint32_t)tid >= MAX_TASKS) return;
    const char* n = tasks[tid].name;
    uint32_t i = 0;
    while (i + 1u < cap && n[i]) { out[i] = n[i]; i++; }
    out[i] = 0;
}

// Kumpulkan alamat backtrace (jalur teks; layar punya rutin gambar sendiri).
static uint32_t panic_bt_collect(uint64_t rbp, uint64_t rsp, uint64_t cs,
                                 uint64_t* out, uint32_t max) {
    uint32_t n = 0;

    // Batas lama (0x1000..0x7FFF_FFFF_FFFF) menerima alamat USER, dan buffer
    // crashdump dulu benar-benar membacanya — dengan SMAP itu = #PF kedua di
    // tengah penulisan dump. Aturan sekarang sama dengan p_backtrace(): hanya
    // higher-half boleh dibaca, dan konteks user dilewati.
    if ((cs & 3u) == 3u) return 0;

    if (panic_ptr_readable(rbp) && rbp < 0xFFFFFFFFFFFFF000ull) {
        const uint64_t* bp = (const uint64_t*)rbp;
        for (uint32_t depth = 0; depth < max && n < max; depth++) {
            uint64_t next = bp[0];
            uint64_t ret  = bp[1];
            if (ret == 0) break;
            out[n++] = ret;
            if (next <= rbp) break;
            bp = (const uint64_t*)next;
        }
    }
    if (n == 0 && panic_ptr_readable(rsp)) {
        // Fallback: -O2 sering menghilangkan frame pointer -> scan stack.
        const uint64_t* sp = (const uint64_t*)rsp;
        for (uint32_t i = 0; i < 768u && n < max; i++) {
            uint64_t q = sp[i];
            if (q >= KERNEL_VMA && q < KERNEL_VMA + 0x2000000ull) out[n++] = q;
        }
    }
    return n;
}

// Simpan snapshot ke RAM log + crashdump disk. Zero-allocation: seluruh buffer
// statis, tanpa heap.
static void panic_persist(uint64_t vector, uint64_t error_code, const registers_t* r,
                          uint64_t cr2, uint64_t cr3, int tid, uint32_t uptime_s,
                          const panic_cause_t* cause, const char* title,
                          const char* desc, int kind) {
    char tname[24];
    panic_task_name(tid, tname, sizeof(tname));

    // 1) Log RAM — deterministik lintas warm-reboot (halaman PMM pertama).
    panic_log_write(vector, error_code, r ? r->rip : 0, cr2, panic_clock_ms(),
                    (uint64_t)(tid < 0 ? 0xFFFFFFFFu : (uint32_t)tid),
                    tname, (uint32_t)kind);

    // 2) Crashdump disk — polling ATA, tanpa IRQ/scheduler.
    if (!crashdump_ready()) return;

    uint32_t at = 0;
    g_dump_text[0] = 0;
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "KyuzenOS panic snapshot\n");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "kind  : ");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at,
                kind ? "kernel_panic() manual" : "exception");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nvector: ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, vector);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  err ");
    at = dt_hex(g_dump_text, sizeof(g_dump_text), at, error_code);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
    if (title) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "cause : ");
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, title);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
    }
    if (cause) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "verdict: ");
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, cause->verdict);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        if (cause->hint1 && cause->hint1[0]) {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "hint1 : ");
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, cause->hint1);
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        }
        if (cause->hint2 && cause->hint2[0]) {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "hint2 : ");
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, cause->hint2);
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        }
    }
    if (desc) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "desc  : ");
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, desc);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
    }
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "task  : #");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, (uint64_t)(tid < 0 ? 0 : (uint32_t)tid));
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, " \"");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, tname[0] ? tname : "(tanpa nama)");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\"\nuptime: ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, uptime_s);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, " s\n");

    if (r) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "rip   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rip);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  cs ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->cs);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  cr3 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, cr3);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  cr2 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, cr2);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nrsp   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rsp);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rbp ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rbp);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rflags ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rflags);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nrax   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rax);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rbx ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rbx);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rcx ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rcx);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rdx ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rdx);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nrsi   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rsi);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rdi ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rdi);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r8  ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r8);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r9  ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r9);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nr10   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r10);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r11 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r11);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r12 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r12);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r13 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r13);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nr14   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r14);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r15 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r15);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  int ");
        at = dt_dec(g_dump_text, sizeof(g_dump_text), at, r->int_num);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");

        uint64_t bt[6];
        uint32_t n = panic_bt_collect(r->rbp, r->rsp, r->cs, bt, 6u);
        if (n == 0) {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at,
                        "bt    : (tidak ada frame valid)\n");
        } else {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "bt    :");
            for (uint32_t i = 0; i < n; i++) {
                at = dt_put(g_dump_text, sizeof(g_dump_text), at, " ");
                if (bt[i] >= KERNEL_VMA) {
                    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "KERNEL+");
                    at = dt_hex(g_dump_text, sizeof(g_dump_text), at, bt[i] - KERNEL_VMA);
                } else {
                    at = dt_hex(g_dump_text, sizeof(g_dump_text), at, bt[i]);
                }
            }
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        }
    }

    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "lastsc: ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, g_last_syscall_num);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, " (task ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at,
                (uint64_t)(g_last_syscall_task < 0 ? 0 : (uint32_t)g_last_syscall_task));
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, ")\n");

    // Header crashdump: semua field diisi eksplisit (tanpa memset).
    crashdump_hdr_t h;
    h.magic            = CRASHDUMP_MAGIC;
    h.version          = CRASHDUMP_VERSION;
    h.flags            = 0;
    h.length           = 0;
    h.checksum         = 0;
    h.sectors          = 0;
    h.dump_count       = 0;
    h.timestamp_ms     = panic_clock_ms();
    h.exception_vector = vector;
    h.error_code       = error_code;
    h.rip              = r ? r->rip : 0;
    h.cr2              = cr2;
    h.rsp              = r ? r->rsp : 0;
    h.cs               = r ? r->cs : 0;
    h.cr3              = cr3;
    h.task_id          = (uint64_t)(tid < 0 ? 0xFFFFFFFFu : (uint32_t)tid);
    for (uint32_t i = 0; i < sizeof(h.task_name); i++) h.task_name[i] = 0;
    for (uint32_t i = 0; i + 1u < sizeof(h.task_name) && tname[i]; i++) h.task_name[i] = tname[i];

    int rc = crashdump_write_snapshot(&h, g_dump_text, at);
    serial_print("\n[PANIC] crashdump: ");
    switch (rc) {
        case  0: serial_print("tersimpan di ekor disk\n"); break;
        case -2: serial_print("DIBATALKAN (dump lain masih aktif)\n"); break;
        case -3: serial_print("DIBATALKAN (panic bersarang)\n"); break;
        case -4: serial_print("GAGAL verifikasi baca-balik\n"); break;
        default: serial_print("tidak tersedia\n"); break;
    }
}

// =====================================================================
// 6d. LOOP UTAMA: polling tombol, menunggu keputusan operator
// =====================================================================
static void panic_interactive(void) {
    const display_mode_t* m = display_get_mode();
    int have_ui = (m && m->height >= PANIC_UI_MIN_HEIGHT && fb_ptr);

    // Pewaktu bebas-IRQ + buang sisa ketikan sebelum panic.
    panic_monotonic_reset();
    g_key_ext = 0;
    panic_keys_drain();

    // Sync FS SEKALI di sini (best-effort, try-lock). Tanpa auto-reboot,
    // inilah jaring pengaman data: operator bisa mematikan daya kapan saja
    // setelah membaca layar. Kalau CPU yang fault memegang lock FS, sync
    // dilewati — jangan pernah menunggu lock di jalur panic.
    int fs_ok = kfs_sync_all_try();
    if (!fs_ok)
        serial_print("[PANIC] sync FS dilewati (lock sedang dipakai) - menunggu tombol\n");

    if (have_ui) {
        // Dua baris statis digambar SEKALI (framebuffer = scanout, tanpa
        // double buffer: menggambar ulang tiap iterasi tampak berkedip).
        p_line(m->height - 48u, fs_ok ? INFO_HOLD : INFO_NOSYNC, C_KEY);
        p_line(m->height - 28u, HINT_KEYS, C_FG);
    }

#ifdef PANIC_HOST_TEST
    uint32_t host_loops = 0;
#endif

    for (;;) {
        // Input pengguna (polling PS/2, tanpa IRQ). Aksi tidak kembali di
        // kernel; `return` di bawah hanya tercapai di host test.
        int k = panic_read_key();
        if (k == 'r') { panic_action_reboot();   return; }
        if (k == 's') { panic_action_shutdown(); return; }
        panic_idle_slice();     // ~5-10 ms sambil mem-polling PIT2

#ifdef PANIC_HOST_TEST
        // Di kernel loop ini terminal. Host test butuh jalan keluar setara
        // supaya bisa memverifikasi "tanpa tombol = tidak ada reboot".
        if (++host_loops >= PANIC_HOST_IDLE_LOOPS) return;
#endif
    }
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
    serial_enter_panic_mode();   // TX UART dibatasi: tidak boleh menggantung

#ifdef HEAP_WATCH_DEBUG
    // Vector 1 (#DB): hardware watchpoint dari heap_watch — log ke serial dan
    // lanjutkan eksekusi (stub akan iretq), bukan BSOD.
    if (r->int_num == 1) {
        extern void heap_watch_db_handler(registers_t* r);
        heap_watch_db_handler(r);
        return;
    }
#endif

    // Jejak paling awal ke COM1 (dikirim SEBELUM lockdown/persistensi). Kalau
    // sistem membeku tanpa layar BSOD, baris ini di serial.log yang memisahkan
    // "fault yang menggantung di dalam handler" dari "hang biasa — tidak ada
    // fault sama sekali". Satu baris, tanpa lock, aman di konteks panic.
    serial_print("\n[PANIC] handler masuk (cpu ");
    ser_dec((uint64_t)smp_current_cpu_index());
    serial_print(")\n");

    // GUARD PANIC BERSARANG. Handler ini bisa dipanggil lagi selagi yang
    // sebelumnya masih berjalan (contoh: penulisan crashdump memicu fault baru).
    // Dalam kondisi itu jangan menggambar/menulis apa pun lagi: batalkan
    // crashdump lalu bekukan sistem supaya layar BSOD tetap utuh dibaca.
    if (g_panic_active) {
        crashdump_abort();
        serial_print("\n[PANIC] panic BERSARANG - crashdump dibatalkan, freeze.\n");
        // Jangan freeze secara SENYAP: layar panic pertama tetap ada, tapi
        // beri tanda di layar supaya jelas kenapa tidak ada dump/reboot.
        panic_overlay_banner("PANIC BERSARANG - crashdump dibatalkan (lihat serial COM1)");
        panic_freeze();
        return;
    }
    g_panic_active = 1;

    int repeat = panic_is_locked();
    panic_lockdown();                     // hentikan semua penulis scanout lain
    serial_print("\n[P1] lockdown aktif - laporan serial mulai\n");

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
        serial_print(paging_is_mapped_nolock(cr2) ? "PROT" : "NONP");
        serial_print(error_code & 2u ? " WRITE" : " READ");
        serial_print(error_code & 4u ? " USER" : " KERNEL");
    }
    ser_regs(r);
    ser_task_context();
    serial_print("\n");

    panic_cause_t cause = panic_explain(int_num, error_code, cr2, r->rip);

    // --- URUTAN PENTING: GAMBAR DULU, SIMPAN BELAKANGAN -------------------
    // Persistensi menyentuh disk (ATA) dan RAM reserved; keduanya bisa lambat
    // atau macet di hardware bermasalah. Kalau persistensi dijalankan lebih
    // dulu, kegagalan/macet di situ membuat sistem tampak "freeze tanpa
    // panic": layar desktop tetap terpampang, BSOD tidak pernah muncul, dan
    // tidak ada reboot. Dengan gambar-dulu, laporan visual SELALU ada; artefak
    // crash (RAM log + crashdump disk) tetap ditulis sebelum countdown/reboot.
    if (!fb_ptr || !display_get_mode()) {           // tidak bisa menggambar
        serial_print("[P2] framebuffer tidak siap - lapor ke serial saja\n");
        panic_persist(int_num, error_code, r, cr2, cr3, smp_current_task_id(),
                      (uint32_t)uptime_s, &cause, NULL, NULL, 0);
        serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");
        panic_interactive();          // tanpa layar: hanya countdown + tombol
        g_panic_active = 0;           // hanya tercapai di host test
        return;
    }

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
        if (paging_is_mapped_nolock(cr2) != (int)(error_code & 1u)) {
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

    p_backtrace(r->rbp, r->rsp, r->cs);

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
    p_dec(pmm_get_used_pages_nolock(), C_KEY);
    p_str(" / ", C_DIM);
    p_dec(pmm_get_total_pages_nolock(), C_FG);
    p_str(" pages (", C_DIM);
    p_dec((pmm_get_used_pages_nolock() * 4096u) / 1024u, C_KEY);
    p_str(" / ", C_DIM);
    p_dec((pmm_get_total_pages_nolock() * 4096u) / 1024u, C_FG);
    p_str(" KB)", C_DIM);
    p_newline();

    // Layar sudah utuh (BSOD + baris countdown digambar panic_interactive).
    serial_print("[P2] layar BSOD selesai digambar\n");

    // Persistensi setelah layar: RAM log + crashdump disk (zero-allocation).
    panic_persist(int_num, error_code, r, cr2, cr3, smp_current_task_id(),
                  (uint32_t)uptime_s, &cause, NULL, NULL, 0);
    serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");

    // Loop interaktif: [R]/[S]/[D] via polling PS/2 + countdown ke auto-reboot.
    // Di kernel rutin ini TIDAK PERNAH kembali (selalu berakhir reboot/freeze);
    // baris setelahnya hanya tercapai di host test.
    panic_interactive();
    g_panic_active = 0;
}

// =====================================================================
// 9. GENERIC KERNEL PANIC (dipanggil manual dari kode kernel)
// =====================================================================
__attribute__((weak))
void kernel_panic(const char* title, const char* desc, uint64_t code) {
    panic_cli();                          // seluruh jalur panic: IRQ mati
    serial_enter_panic_mode();            // TX UART dibatasi (lihat serial.h)

    serial_print("\n[PANIC] handler masuk (kernel_panic, cpu ");
    ser_dec((uint64_t)smp_current_cpu_index());
    serial_print(")\n");

    // GUARD PANIC BERSARANG (sama seperti exception_handler): kalau handler
    // masih berjalan, jangan menggambar/menulis apa pun lagi.
    if (g_panic_active) {
        crashdump_abort();
        serial_print("\n[PANIC] panic BERSARANG - crashdump dibatalkan, freeze.\n");
        panic_overlay_banner("PANIC BERSARANG - crashdump dibatalkan (lihat serial COM1)");
        panic_freeze();
        return;
    }
    g_panic_active = 1;

    int repeat = panic_is_locked();
    panic_lockdown();
    serial_print("\n[P1] lockdown aktif - laporan serial mulai\n");

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

    // (Gambar-dulu, simpan-belakangan: lihat catatan panjang di
    //  exception_handler. Panic manual tidak punya registers_t, jadi artefak
    //  berisi judul + detail + konteks task saja.)
    if (!fb_ptr || !display_get_mode()) {
        serial_print("[P2] framebuffer tidak siap - lapor ke serial saja\n");
        panic_persist(0xFFFFu, code, 0, 0, panic_read_cr3(), smp_current_task_id(),
                      (uint32_t)(panic_clock_ms() / 1000u), 0, title, desc, 1);
        serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");
        panic_interactive();
        g_panic_active = 0;           // hanya tercapai di host test
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
    p_dec(pmm_get_used_pages_nolock(), C_KEY);
    p_str(" / ", C_DIM);
    p_dec(pmm_get_total_pages_nolock(), C_FG);
    p_str(" pages terpakai", C_DIM);
    p_newline();

    serial_print("[P2] layar BSOD selesai digambar\n");
    panic_persist(0xFFFFu, code, 0, 0, panic_read_cr3(), smp_current_task_id(),
                  (uint32_t)(panic_clock_ms() / 1000u), 0, title, desc, 1);
    serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");

    // Loop interaktif: [R]/[S]/[D] via polling PS/2 + countdown ke auto-reboot.
    // Di kernel rutin ini TIDAK PERNAH kembali (selalu berakhir reboot/freeze);
    // baris setelahnya hanya tercapai di host test.
    panic_interactive();
    g_panic_active = 0;
}

// =====================================================================
// 10. SHORTCUT / KOMPATIBILITAS
// =====================================================================
void page_fault_handler(registers_t* r, uint64_t fault_addr) {
    (void)fault_addr;   // CR2 sudah dibaca langsung di exception_handler
    exception_handler(r);
}
