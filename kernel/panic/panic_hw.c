// kernel/panic/panic_hw.c — akses hardware darurat TANPA IRQ + aksi operator.
//
// Seluruh jalur panic berjalan dengan `cli`: IRQ timer dan IRQ keyboard sudah
// mati dan TIDAK boleh dihidupkan lagi (CPU lain masih memakai device itu).
// Karena itu semua yang butuh waktu/input di file ini berjalan dengan POLLING:
//   * waktu  : PIT channel 2 diprogram jadi rate generator (~100 Hz) dan level
//              pulsenya dibaca lewat port 0x61 — channel 2 tidak menghasilkan
//              IRQ (hanya tersambung ke speaker), jadi aman di-polling;
//   * tombol : controller PS/2 (0x64/0x60) dibaca langsung, byte mouse dibuang
//              supaya output buffer tidak menyumbat scancode keyboard.
//
// File ini juga memegang aksi operator dari layar BSOD ([R] reboot, [S]
// shutdown) beserta freeze — semuanya berakhir di hardware, bukan di scheduler.
//
// KONTRAK: tanpa heap, tanpa lock (baca `_try` di panic_internal.h), dan tidak
// pernah menunggu lock yang mungkin dipegang CPU yang fault.

#include <stdint.h>

#include "panic_internal.h"
#include "serial.h"   // serial_print (jejak perintah reset)
#include "io.h"       // inb/outb/outw (polling PIT + PS/2 + port power-off)
#include "acpi.h"     // acpi_reset_raw()/acpi_poweroff_raw() — ACPI RESET_REG & S5

// =====================================================================
// 0. SEAM HOST TEST (jam palsu, tombol, counter aksi, reboot hook)
// =====================================================================
// Di host test instruksi privileged (cli/hlt/CR2/in-out) di-stub dan waktu
// diganti jam palsu yang dimajukan panic_idle_slice() — lihat test/panic_test.c.
#ifdef PANIC_HOST_TEST
// Jam palsu: panic_clock_ms() membaca apa adanya (untuk uptime/label waktu),
// sedangkan panic_idle_slice() memajukannya 1 detik per slice supaya loop
// tombol selesai deterministik tanpa menunggu waktu nyata.
volatile uint64_t g_panic_test_ms        = 0;
volatile uint64_t g_panic_test_cr2       = 0;
volatile int32_t  g_panic_test_key       = 0;
volatile int32_t  g_panic_test_aux       = 0;   // byte mouse mengantre di depan

volatile uint32_t g_panic_test_reboots   = 0;
volatile uint32_t g_panic_test_shutdowns = 0;
volatile uint32_t g_panic_test_freezes   = 0;
volatile uint32_t g_panic_test_waits     = 0;
#endif

// Hook reboot (panic_set_reboot_hook, include/panic.h). Di host test dipakai
// untuk memverifikasi urutan flush FS → reboot tanpa mereset mesin.
static void (*g_reboot_hook)(void) = 0;

void panic_set_reboot_hook(void (*fn)(void)) { g_reboot_hook = fn; }

#ifdef PANIC_HOST_TEST
void panic_hw_reset(void) {
    g_reboot_hook = 0;
    g_panic_test_ms = 0;
    g_panic_test_cr2 = 0;
    g_panic_test_key = 0;
    g_panic_test_aux = 0;
    g_panic_test_reboots = 0;
    g_panic_test_shutdowns = 0;
    g_panic_test_freezes = 0;
    g_panic_test_waits = 0;
}
#endif

// =====================================================================
// 1. PRIMITIF PRIVILEGED (di-stub kalau PANIC_HOST_TEST)
// =====================================================================
void panic_cli(void) {
#ifndef PANIC_HOST_TEST
    __asm__ volatile("cli");
#endif
}

// TIDAK ada panic_sti(): jalur panic tidak pernah menghidupkan IRQ lagi.
// Pewaktu dan input di-polling (bagian 2 & 3).

// Hanya dipakai jalur kernel (panic_reset_hw/panic_freeze) — di host test
// tidak ada instruksi privileged yang dijalankan.
#ifndef PANIC_HOST_TEST
static void panic_halt(void) {
    __asm__ volatile("hlt");
}
#endif

// (unused di host test: jalur polling PIT/PS-2 di-stub — lihat bagian 2/3.)
static void __attribute__((unused)) panic_pause(void) {
#ifndef PANIC_HOST_TEST
    __asm__ volatile("pause");
#endif
}

uint64_t panic_read_cr2(void) {
#ifdef PANIC_HOST_TEST
    return g_panic_test_cr2;
#else
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
#endif
}

uint64_t panic_read_cr3(void) {
#ifdef PANIC_HOST_TEST
    return 0x0000000012345000ull;
#else
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
#endif
}

// Waktu sekarang TANPA efek samping (dipakai untuk uptime).
uint64_t panic_clock_ms(void) {
#ifdef PANIC_HOST_TEST
    return g_panic_test_ms;
#else
    return timer_get_ms();
#endif
}

// =====================================================================
// 2. PEWAKTU TANPA IRQ: polling PIT channel 2
// =====================================================================
// Kenapa channel 2 dan bukan channel 0? Channel 0 adalah sumber IRQ0 — kita
// sudah meng-`cli` dan tidak boleh lagi mengandalkan IRQ. Channel 2 hanya
// tersambung ke speaker (TIDAK menghasilkan IRQ) dan status pulsenya bisa
// dibaca lewat port 0x61, jadi aman di-polling dari jalur panic.
// Diprogram mode 3 (square wave) reload 11931 -> 100 Hz: tiap transisi level
// = 5 ms. Resolusi 5 ms cukup untuk kebutuhan jalur panic.
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
// bergerak (tidak ada hardware legacy), pakai spin kalibrasi supaya loop tetap
// maju — bukan menggantung selamanya.
void panic_idle_slice(void) {
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
// 3. INPUT TANPA IRQ: polling controller PS/2 (0x64/0x60)
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

// Matikan IRQ keyboard (1) & mouse (12) di 8259 PIC.
//
// Kenapa perlu: jalur panic hanya `cli` di CPU yang FAULT, jadi CPU lain masih
// melayani IRQ1 — dan handler keyboard (drivers/keyboard.c) membaca port 0x60,
// artinya ia MENELAN scancode sebelum loop panic sempat mem-poll-nya. Gejalanya
// persis "tombol [R]/[S] tidak berfungsi": tombol ditekan, byte-nya dibaca
// handler di CPU lain, loop panic tidak pernah melihat apa pun.
//
// Handler-nya sudah di-guard (panic_is_locked) supaya tidak membaca port saat
// BSOD; masking ini lapisan kedua yang membuat byte PS/2 DIPASTIKAN tinggal di
// output buffer 8042 sampai loop panic membacanya — berlaku juga kalau routing
// IRQ berubah (mis. lewat APIC/IOAPIC) atau ada pembaca baru di masa depan.
// Nilai IMR dicatat ke serial supaya kondisi PIC bisa diperiksa dari log.
void panic_input_irq_mask(void) {
#ifndef PANIC_HOST_TEST
    uint8_t imr_master = inb(0x21);   // bit1 = IRQ1 keyboard
    uint8_t imr_slave  = inb(0xA1);   // bit4 = IRQ12 mouse
    outb(0x21, (uint8_t)(imr_master | 0x02u));
    outb(0xA1, (uint8_t)(imr_slave  | 0x10u));
    serial_print("[PANIC] IRQ keyboard/mouse dimatikan - input diambil alih polling PS/2\n");
#else
    (void)0;   // host test: tidak ada PIC yang bisa diprogram
#endif
}

// Buang byte sisa ketikan & gerakan mouse sebelum panic supaya tidak salah tafsir.
void panic_keys_drain(void) {
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
        if (c) {
            // Jejak lapangan: memisahkan "tombol tidak pernah sampai ke loop
            // panic" (masalah input) dari "aksi tidak jalan" (masalah reset/
            // power-off). Tanpa ini, dua kegagalan itu sama-sama terlihat
            // sebagai "tombol tidak berfungsi".
            serial_print("[PANIC] tombol PS/2 terdeteksi (scancode ");
            ser_hex(sc);
            serial_print("): ");
            serial_print(c == 'r' ? "R (reboot)" : "S (shutdown)");
            serial_print("\n");
            return c;
        }
    }
    return 0;
}

// =====================================================================
// 4. AKSI AKHIR: reset hardware, power-off, freeze
// =====================================================================
// Reset hardware terakhir: ACPI RESET_REG, chipset, keyboard controller, lalu
// fallback triple fault. Tidak pernah kembali. (Tidak dibangun di host test —
// di sana reboot diwakili panic_set_reboot_hook.)
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
    // 0) RESET_REG FADT (nilai generik firmware) — cara reset paling benar
    //    per spesifikasi ACPI. Diabaikan rapi bila FADT tak terparse.
    serial_print("[PANIC] reset via ACPI RESET_REG\n");
    (void)acpi_reset_raw();

    // 1) Chipset Reset Control Register (0xCF9) — paling andal di QEMU
    //    (i440fx/q35) MAUPUN hardware nyata. 0x06 = SYS_RST | RST_CPU.
    //    Urutan ini penting: di QEMU, reset lewat keyboard controller (0x64,
    //    0xFE) DIABAIKAN — sistem tampak "freeze" setelah aksi reboot padahal
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
void panic_flush_and_reboot(void) {
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

// Bekukan sistem (panic bersarang / power-off tidak didukung). Di kernel TIDAK
// kembali; di host test kembali supaya bisa diverifikasi.
void panic_freeze(void) {
#ifdef PANIC_HOST_TEST
    g_panic_test_freezes++;         // dihitung di sini agar SEMUA jalur beku tercatat
    return;
#else
    for (;;) panic_halt();
#endif
}

// [R] reboot: baris status singkat → sync best-effort → reset hardware.
void panic_action_reboot(void) {
    serial_print("\n[PANIC] [R] reboot diminta pengguna\n");
    panic_status_line("REBOOT diminta - menyimpan data lalu reset...");
    panic_flush_and_reboot();
}

// [S] shutdown: baris status singkat → sync best-effort → ACPI S5 → port
// emulator → freeze. APM (int 15h) tidak tersedia dari long mode, jadi
// rantainya berhenti di ACPI + port emulator.
void panic_action_shutdown(void) {
    serial_print("\n[PANIC] [S] shutdown diminta pengguna\n");
    panic_status_line("SHUTDOWN diminta - menyimpan data lalu mematikan daya...");
    if (!kfs_sync_all_try())           // best-effort: jangan menggantung di lock
        serial_print("[PANIC] sync FS dilewati (lock sedang dipakai) - tetap shutdown\n");
#ifndef PANIC_HOST_TEST
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
