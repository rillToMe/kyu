#include "syscall.h"
#include <stdint.h>
#include "task.h"      // yield, task_sleep_ms, cred_current_is_root
#include "usercopy.h"
#include "timer.h"     // timer_get_ms, get_cpu_usage
#include "pmm.h"       // pmm_get_total/used_ram
#include "kyuzenfs.h"  // kfs_get_total/used_space
#include "pci.h"       // acpi_poweroff, system_reboot (deklarasi existing)

// Tanpa header publik (dipindahkan verbatim dari syscall.c lama).
extern void get_cpu_string(char* buffer);
extern void rtc_read_time(uint32_t*);

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

    return 0;
}
