#include "syscall.h"
#include <stdint.h>
#include "task.h"          // smp_current_task_id, cred_current_is_root, cred_task_uid
#include "cred.h"          // cred_transition_allowed, CRED_ROOT_UID
#include "usercopy.h"
#include "paging.h"        // vmm_read_cr3, paging_is_mapped_into
#include "fs.h"            // fs_node_t, write_fs, read_fs
#include "tty.h"           // tty_clear
#include "spinlock.h"      // scheduler_lock (sys_set_uid)
#include "crash_notice.h"  // SYS_CRASH_NOTICE
#include "crash_archive.h" // crash_archive_notice

// tty_node: deklarasi kanonis di tty.h (di-include di atas).
// scheduler_lock (kernel/sched/core.c) tidak punya owner header dan dipakai
// lintas TU (proc.c, kernel_userlib.c juga extern manual) — tetap lokal di
// sini; membuat sched.h adalah out-of-scope phase ini.
// Guards task cred transitions (defined in kernel/sched/core.c).
extern spinlock_t scheduler_lock;

int sys_misc_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 1) { // sys_print
        // Tahap 2: copy-in bounded — strlen tak berbatas pada pointer user hilang.
        char kstr[UC_MAX_STR];
        int64_t n = strncpy_from_user(uc, kstr, r->rbx, sizeof(kstr));
        if (n > 0) write_fs(&tty_node, 0, (uint32_t)n, (uint8_t*)kstr);
    }
    else if (syscall_num == 2) { // sys_clear_screen
        tty_clear();
    }
    else if (syscall_num == 3) { // sys_read_keyboard
        // Phase 5B: flush kbd_buffer/event queue DIHAPUS dari titik baca ini.
        // Routing keyboard kini eksklusif (window fokus → event queue; tidak
        // ada fokus → TTY), jadi tidak ada lagi "sisa input app lama" yang
        // harus dibersihkan di sini. Flush-per-baca juga race: karakter yang
        // tiba di antara dua panggilan read_keyboard (mis. saat shell sibuk
        // mengeksekusi perintah) terbuang percuma. Transisi lifecycle
        // (exec/spawn/exit) tetap flush di jalurnya masing-masing.
        // Tahap 2: baca ke buffer kernel dulu — tulisan ke pointer user tidak
        // lagi terjadi di dalam kbd_lock (IRQ off). Validasi SEBELUM read
        // yang blocking, supaya input tidak terlanjur dikonsumsi lalu gagal.
        uint32_t want = (uint32_t)r->rcx;
        if (want > UC_MAX_KBD) want = UC_MAX_KBD;
        if (want > 0 && user_range_ok(uc, r->rbx, want)) {
            uint8_t kbuf[UC_MAX_KBD];
            uint32_t n = read_fs(&tty_node, 0, want, kbuf);
            if (n > 0) copy_to_user(uc, r->rbx, kbuf, n);
            *ret = n;
        }
    }
    // --- SYSCALL: PER-TASK IDENTITY (P0 Phase 1) ---
    // sys_set_uid: root-only transition on the CALLER's own cred.
    // Non-root is denied ((uint64_t)-1), never elevated. uid+gid move
    // together; fine-grained gid/setuid semantics are a later phase.
    // Shell sudo stays a UX gate — this check is the kernel boundary.
    else if (syscall_num == 27) { // sys_set_uid
        task_t* self = syscall_current_task();
        uint32_t want = (uint32_t)r->rbx;
        if (!self || !cred_transition_allowed(cred_task_uid(self))) {
            *ret = (uint64_t)-1;
        } else {
            uint64_t f = spinlock_lock_irqsave(&scheduler_lock);
            self->cred.uid = want;
            self->cred.gid = want;
            spinlock_unlock_irqrestore(&scheduler_lock, f);
            *ret = 0;
        }
    }
    else if (syscall_num == 28) { // sys_get_uid
        task_t* self = syscall_current_task();
        *ret = self ? cred_task_uid(self) : CRED_ROOT_UID;
    }
    else if (syscall_num == 42) { // sys_get_cr3 — return current CR3 physical address
        // Diagnostic syscall for process isolation testing.
        // Returns the physical address of the current PML4 (CR3 value).
        *ret = (uint64_t)vmm_read_cr3();
    }
    else if (syscall_num == 43) { // sys_get_task_id — return current task ID
        *ret = (uint64_t)(int64_t)smp_current_task_id();
    }
    else if (syscall_num == 44) { // sys_is_mapped — check if vaddr is mapped
        // Returns 1 if the page containing vaddr is present in the address
        // space of the CALLING TASK (app AS untuk user page; kernel PML4
        // untuk task kernel). Safe: does NOT dereference, only walks tables.
        task_t *self = syscall_current_task();
        phys_addr_t as = (self) ? self->pml4_phys : PHYS_NULL;
        *ret = (uint64_t)paging_is_mapped_into((uint64_t)r->rbx, as);
    }
    else if (syscall_num == 45) { // sys_get_pid — return per-AS cookie
        // FIX_003: cookie dibaca dari TASK pemanggil, bukan global —
        // global current_as_cookie menampung cookie task yang TERAKHIR
        // exec di CPU mana pun (tertukar di SMP).
        task_t *self = syscall_current_task();
        *ret = (uint64_t)(self ? self->cookie : 0);
    }
    else if (syscall_num == SYS_CRASH_NOTICE) { // sys_crash_notice(crash_notice_t* out)
        // Pemberitahuan crash untuk aplikasi (desktop memakainya untuk
        // menampilkan notifikasi saat boot setelah panic). Struct kecil & tetap
        // -> bounce buffer di stack, tanpa kmalloc.
        crash_notice_t kn;
        int have = crash_archive_notice(&kn);
        if (user_range_ok(uc, r->rbx, sizeof(kn))) {
            if (copy_to_user(uc, r->rbx, &kn, sizeof(kn)) != 0) have = 0;
        } else {
            have = 0;                       // pointer user tidak valid
        }
        *ret = (uint64_t)have;
    }

    return 0;
}
