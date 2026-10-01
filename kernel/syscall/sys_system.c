#include "syscall.h"
#include <stdint.h>
#include "task.h"      // yield, task_sleep_ms, cred_current_is_root
#include "usercopy.h"
#include "timer.h"     // timer_get_ms, get_cpu_usage
#include "pmm.h"       // pmm_get_total/used_ram
#include "kyuzenfs.h"  // kfs_get_total/used_space
#include "pci.h"       // acpi_poweroff, system_reboot (deklarasi existing)
#include "rtc.h"       // rtc_read_time
#include "entropy.h"   // SYS_ENTROPY

// get_cpu_string (kernel/cpu.c) tidak punya owner header kernel (deklarasi
// user-ABI ada di userlib.h) dan hanya dipakai satu TU di sini — tetap lokal.
extern void get_cpu_string(char* buffer);

// ============================================================
// GUARD: tabrakan nomor syscall.
//
// Nomor syscall di file ini adalah literal di rantai if/else, dan cabang yang
// dievaluasi lebih dulu menang tanpa peringatan. Bug nyata: refresh rate pernah
// memakai 87, yang sudah dipakai SYS_ENTROPY — akibatnya sys_entropy() selalu
// mengembalikan 60 (refresh rate) dan TLS memakai angka itu sebagai seed
// HMAC-DRBG. _Static_assert ini membuat tabrakan jadi error kompilasi.
//
// Cara pakai: tambahkan satu baris di bawah setiap kali nomor baru ditambahkan
// ke file ini, atau (lebih baik) ganti literalnya dengan #define bernama.
// ============================================================
#define SYS_GET_REFRESH_RATE 89
#define SYS_SET_REFRESH_RATE 90

_Static_assert(SYS_GET_REFRESH_RATE != SYS_ENTROPY,
               "syscall number collision: refresh-rate getter vs SYS_ENTROPY");
_Static_assert(SYS_SET_REFRESH_RATE != SYS_ENTROPY,
               "syscall number collision: refresh-rate setter vs SYS_ENTROPY");
_Static_assert(SYS_GET_REFRESH_RATE != SYS_SET_REFRESH_RATE,
               "syscall number collision: refresh-rate getter vs setter");

// Counter: setiap kali sys_yield dipanggil, tambah counter ini.
// Timer membaca dan mereset setiap tick untuk menentukan apakah CPU idle.
volatile uint32_t yield_counter = 0;

int sys_system_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 4) { // sys_yield
        yield_counter++; // Tandai CPU idle untuk CPU usage tracker
        // FIX_005 Tahap 1: hlt dilakukan di sisi kernel — hlt privileged
        // (CPL=0), app ring-3 yang mengeksekusinya sendiri kena #GP.
        // PENTING: gate int 0x80 (0xEE) adalah INTERRUPT gate — IF dimatikan
        // saat entry. sti WAJIB sebelum hlt atau CPU tidur selamanya.
        __asm__ volatile("sti; hlt");
    }
    else if (syscall_num == 14) { // sys_uptime → returns ms sejak boot (hardware-agnostic)
        *ret = timer_get_ms();
    }

    else if (syscall_num == 15) { // sys_total_ram
        *ret = pmm_get_total_ram();
    }
    else if (syscall_num == 16) { // sys_used_ram
        *ret = pmm_get_used_ram();
    }
    else if (syscall_num == 17) { // get_cpu_string
        // get_cpu_string menulis tepat 49 byte (48 brand CPUID + NUL).
        char kcpu[64];
        get_cpu_string(kcpu);
        copy_to_user(uc, r->rbx, kcpu, 49);
    }
    else if (syscall_num == 20) { // sys_get_time
        uint32_t ktime[6];   // [year, month, day, hour, min, sec]
        rtc_read_time(ktime);
        copy_to_user(uc, r->rbx, ktime, sizeof(ktime));
    }
    else if (syscall_num == 35) { // sys_get_total_disk
        *ret = kfs_get_total_space();
    }
    else if (syscall_num == 36) { // sys_get_used_disk
        *ret = kfs_get_used_space();
    }
    else if (syscall_num == 37) { // sys_get_cpu_usage
        *ret = get_cpu_usage();
    }
    else if (syscall_num == 38) { // sys_shutdown — root only
        if (!cred_current_is_root()) {
            *ret = (uint64_t)-1;
        } else {
            acpi_poweroff();
        }
    }
    else if (syscall_num == 39) { // sys_reboot — root only
        if (!cred_current_is_root()) {
            *ret = (uint64_t)-1;
        } else {
            system_reboot();
        }
    }
    else if (syscall_num == 46) { // sys_sleep — non-busy sleep RBX ms
        // Task masuk sleep queue (TASK_SLEEPING); CPU bebas jalankan task lain.
        task_sleep_ms((uint32_t)r->rbx);
    }
    else if (syscall_num == SYS_GET_REFRESH_RATE) { // sys_get_refresh_rate
        *ret = timer_get_refresh_rate();
    }
    else if (syscall_num == SYS_SET_REFRESH_RATE) { // sys_set_refresh_rate(hz) — RBX = hz
        // Root-only: mengubah frekuensi PIT global memengaruhi semua task.
        // Return 0 sukses, -1 ditolak/tidak didukung (60/100/144 saja).
        //
        // Nomor 89/90 (BUKAN 87/88): 87 sudah dipakai SYS_ENTROPY
        // (include/entropy.h). Kalau keduanya memakai 87, cabang refresh rate
        // yang dievaluasi lebih dulu akan menelan sys_entropy() dan TLS
        // (apps/browser/tls/tls_kyuzen.c:124) menerima refresh rate sebagai
        // "byte acak" untuk seed HMAC-DRBG.
        if (!cred_current_is_root()) {
            *ret = (uint64_t)-1;
        } else {
            *ret = (uint64_t)(int64_t)timer_set_refresh_rate((uint32_t)r->rbx);
        }
    }
    else if (syscall_num == SYS_ENTROPY) { // sys_entropy(out*, len)
        // RBX = buffer user, RCX = len. Isi via bounce kernel (RDRAND tak
        // boleh menulis langsung ke user — dan range dicek dulu).
        uint32_t len = (uint32_t)r->rcx;
        int n = ENTROPY_ERR;
        if (len > 0 && len <= ENTROPY_MAX &&
            user_range_ok(uc, r->rbx, len)) {
            uint8_t kbuf[ENTROPY_MAX];
            n = entropy_fill(kbuf, len);
            if (n > 0) copy_to_user(uc, r->rbx, kbuf, (uint32_t)n);
        }
        *ret = (uint64_t)(int64_t)n;
    }

    return 0;
}
