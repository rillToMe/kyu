#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

// =====================================================================
// BSOD / kernel panic — API publik
//   panic.c     : layar BSOD, diagnosa, aksi interaktif (tanpa auto-reboot)
//   panic_log.c : crash log persisten di RAM reserved (tahan warm-reboot)
//   crashdump.c : snapshot raw ke sektor disk (lihat include/crashdump.h)
// Semua jalur panic TANPA alokasi dinamis (zero malloc) dan TANPA lock.
// =====================================================================

// --- Lockdown ---------------------------------------------------------
// Tandai bahwa sistem sedang menampilkan BSOD. Dipanggil paling awal pada
// setiap jalur panic. Semua penulis scanout lain (compositor_flush, HUD timer,
// mouse_handler) memeriksa panic_is_locked() dan berhenti menggambar — tanpa
// ini CPU lain tetap merender desktop di atas layar panic (panic hanya `cli`
// di CPU yang fault), sehingga BSOD tampak "glitch lalu hilang".
void panic_lockdown(void);
int  panic_is_locked(void);

// --- Aksi yang bisa dipilih pengguna di layar panic -------------------
// [R] reboot  : flush FS lalu reset hardware (8042 0xFE + chipset 0xCF9).
// [S] shutdown: flush FS lalu power-off (ACPI S5 → port QEMU/Bochs/VBox).
// Tidak ada tombol freeze: loop BSOD memang menunggu tanpa batas (tidak ada
// auto-reboot), jadi diam saja sudah setara freeze.
// Di kernel keduanya BERAKHIR (tidak kembali); di host test keduanya kembali
// supaya bisa diverifikasi.
void panic_action_reboot(void);
void panic_action_shutdown(void);

// Ganti rutin reboot. NULL = reset hardware normal.
// Dipakai host test supaya urutan flush FS -> reboot bisa diverifikasi tanpa
// benar-benar mereset mesin (lihat test/panic_test.c).
void panic_set_reboot_hook(void (*fn)(void));

// --- Polling input & waktu tanpa IRQ (dipakai loop panic) -------------
// Konteks panic berjalan dengan `cli`: tidak ada IRQ timer dan tidak ada IRQ
// keyboard, jadi waktu diukur dengan mem-polling PIT dan tombol dibaca dengan
// mem-polling controller PS/2. Keduanya non-blocking dan tanpa kalibrasi.
void     panic_monotonic_reset(void); // program PIT ch2 (sekali, saat masuk panic)
uint64_t panic_monotonic_ms(void);    // ms sejak reset (host: jam g_panic_test_ms)
int      panic_read_key(void);        // 'r' / 's' / 'd'; 0 = tidak ada input

// --- Crash log persisten (kernel/debug/panic_log.c) -------------------------
// Disimpan di RAM reserved (halaman pertama PMM — alamatnya deterministik
// lintas warm-reboot) dan bertahan karena warm reset tidak menghapus DRAM.
#define PANIC_LOG_SIGNATURE 0xDEADC0DEL
#define PANIC_LOG_CONSUMED  0xDEADC0D1L
#define PANIC_LOG_VERSION   1u

// exception_vector 0xFFFF = kernel_panic() manual; kind 0 = exception,
// 1 = kernel_panic manual.
typedef struct panic_log {
    uint32_t version;
    uint32_t signature;        // PANIC_LOG_SIGNATURE / PANIC_LOG_CONSUMED
    uint32_t checksum;         // sum32 sisa struct — tolak isi RAM yang korup
    uint32_t crash_count;      // berapa kali panic sejak area ini direservasi
    uint64_t timestamp;        // timer_get_ms() saat panic
    uint64_t exception_vector; // int_num; 0xFFFF = kernel_panic() manual
    uint64_t error_code;
    uint64_t rip;
    uint64_t cr2;
    uint64_t task_id;
    uint32_t kind;
    char     task_name[24];
} panic_log_t;

// reserved_phys/len: area RAM yang tidak dipakai siapa pun selama boot
// (kernel.c: halaman pertama pmm_alloc_page()). Di host test argumennya
// ditafsirkan sebagai pointer biasa (lihat seam di bawah).
void panic_log_init(uint64_t reserved_phys, uint64_t reserved_len);

// Tulis log TANPA alokasi/lock (dipanggil dari jalur panic).
void panic_log_write(uint64_t exception_vector, uint64_t error_code, uint64_t rip,
                     uint64_t cr2, uint64_t timestamp_ms, uint64_t task_id,
                     const char* task_name, int kind);

// Dipanggil saat boot-up awal: kalau ada log crash dari boot sebelumnya,
// laporkan ke serial + console lalu bersihkan flag-nya. Return 1 kalau ada.
int panic_check_previous_log(void);

// =====================================================================
// Seam host test (-DPANIC_HOST_TEST)
// panic.c/panic_log.c/crashdump.c di-include langsung oleh test/panic_test.c.
// Instruksi privileged (cli/sti/hlt/lidt/CR2/CR3/in-out) di-stub, jam diganti
// g_panic_test_ms, dan input tombol dikendalikan g_panic_test_key.
// =====================================================================
#ifdef PANIC_HOST_TEST
extern volatile uint64_t g_panic_test_ms;        // jam palsu (ms)
extern volatile uint64_t g_panic_test_cr2;       // CR2 palsu untuk fault
extern volatile int32_t  g_panic_test_key;       // key berikutnya ('r'/'s'/'d')

extern volatile uint32_t g_panic_test_reboots;   // berapa kali rutin reboot dipicu
extern volatile uint32_t g_panic_test_shutdowns; // berapa kali power-off dipicu
extern volatile uint32_t g_panic_test_freezes;   // berapa kali mode beku dipilih
extern volatile uint32_t g_panic_test_waits;     // iterasi loop interaktif
void panic_host_test_reset(void);                // bersihkan lockdown + counter
#endif

#endif
