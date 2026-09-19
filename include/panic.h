#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

// =====================================================================
// BSOD / kernel panic — API publik (implementasi: kernel/panic.c)
// =====================================================================

// Tandai bahwa sistem sedang menampilkan BSOD. Dipanggil paling awal pada
// setiap jalur panic. Semua penulis scanout lain (compositor_flush, HUD timer,
// mouse_handler) memeriksa panic_is_locked() dan berhenti menggambar — tanpa
// ini CPU lain tetap merender desktop di atas layar panic (panic hanya `cli`
// di CPU yang fault), sehingga BSOD tampak "glitch lalu hilang".
void panic_lockdown(void);
int  panic_is_locked(void);

// Ganti rutin reboot. NULL = reset hardware normal (8042 0xFE + triple fault).
// Dipakai host test supaya urutan flush FS -> reboot bisa diverifikasi tanpa
// benar-benar mereset mesin (lihat test/panic_test.c).
void panic_set_reboot_hook(void (*fn)(void));

// --- Seam host test (-DPANIC_HOST_TEST) ------------------------------------
// panic.c dikompilasi langsung oleh test/panic_test.c. Instruksi privileged
// (cli/sti/hlt/lidt/CR2/CR3) di-stub dan jam diganti g_panic_test_ms supaya
// countdown 10 detik selesai deterministik (tiap pembacaan maju 1 detik).
#ifdef PANIC_HOST_TEST
extern volatile uint64_t g_panic_test_ms;       // jam palsu (ms)
extern volatile uint64_t g_panic_test_cr2;      // CR2 palsu untuk fault
extern volatile uint32_t g_panic_test_redraws;  // sel digit countdown digambar ulang
extern volatile uint32_t g_panic_test_reboots;  // berapa kali rutin reboot dipicu
void panic_host_test_reset(void);               // bersihkan lockdown + counter
#endif

// Cadangan crash-log ke disk (belum diimplementasi di panic.c).
#define PANIC_LOG_SIGNATURE 0xDEADC0DEL

typedef struct panic_log {
    uint32_t signature;
    uint64_t timestamp;
    uint64_t exception_vector;
    uint64_t rip;
    uint64_t cr2;
    char     task_name[32];
} panic_log_t;

void panic_log_init(uint64_t reserved_phys, uint64_t reserved_len);
int  panic_check_previous_log(void);

void crashdump_init(uint64_t crash_start_lba, uint32_t crash_sectors);
int  crashdump_write_snapshot(const void* crash_buf, uint32_t len);

#endif
