// ============================================================
// KERNEL USERLIB SHIM — apps/kernel_userlib.c
//
// Implementasi kernel-side dari fungsi-fungsi yang dipanggil
// oleh apps/shell.c, apps/zen.c, dan apps/login.c.
//
// Berjalan di Ring 0. Tidak pakai int $0x80.
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include "fs.h"
#include "heap.h"  // size_t-aware kmalloc/krealloc
#include "pmm.h"   // phys_addr_t, PHYS_NULL

// --- Impor API Kernel ---
extern fs_node_t  tty_node;
extern void       tty_clear(void);
extern void       yield(void);
extern void       compositor_flush(void);  // Flush setiap print agar langsung tampil
extern uint32_t   write_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern uint32_t   read_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);

extern void       kfs_format(void);
extern void       kfs_list_files(void);
extern void       kfs_read_file(char* filename);
extern void       kfs_delete_file(char* filename);
extern int        kfs_exists(char* filename);
extern uint32_t   kfs_get_file_size(char* filename);
extern int        kfs_read_to_buffer(char* filename, char* out_buffer, uint32_t buffer_capacity);
extern int        kfs_create_file(char* filename, char* data, uint32_t size);
// Shell engine (apps/shell_core.c) memakai subset API generik ini; sediakan
// padanannya di Ring 0. Signature memakai void*/char* agar tidak perlu
// meng-include userlib.h (yang mendeklarasikan sys_alloc(uint32_t) bentrok
// dengan sys_alloc(size_t) di sini).
extern int        kfs_get_file_list(char* path, void* buffer, int max_entries);
extern int        kfs_create_folder(char* path);
extern uint32_t   get_cpu_usage(void);
extern int        kernel_ping(const char* host);
extern uint32_t   kfs_get_total_space(void);
extern uint32_t   kfs_get_used_space(void);

#include "timer.h"    // Unified timer API: timer_get_ms(), timer_sleep_ms(), timer_get_ticks()


extern uint64_t   pmm_get_total_ram(void);
extern uint64_t   pmm_get_used_ram(void);

#include "task.h"
#include "smp.h"
#include "spinlock.h"
#include "cred.h"
#include "proc.h"
#include "vfs.h"   // P0 Phase 5: Ring-0 fd shims call vfs_* directly
extern spinlock_t scheduler_lock;

// ============================================================

static uint32_t _kul_strlen(const char* str) {
    uint32_t len = 0;
    while (str[len]) len++;
    return len;
}

// print: tulis ke TTY dan langsung flush ke layar
void print(char* text) {
    if (!text || !tty_node.write) return;
    write_fs(&tty_node, 0, _kul_strlen(text), (uint8_t*)text);
    compositor_flush();  // Tampilkan seketika, tidak tunggu timer tick
}

// clear_screen: bersihkan TTY + flush
void clear_screen(void) {
    tty_clear();
    compositor_flush();
}

// read_keyboard: blocking read 1 karakter (yield CPU sambil nunggu)
uint32_t read_keyboard(char* buffer, uint32_t size) {
    uint32_t n = 0;
    while (n == 0) {
        n = read_fs(&tty_node, 0, size, (uint8_t*)buffer);
        if (n == 0) {
            // Tidurkan CPU sampai interrupt berikutnya (timer/keyboard).
            // AMAN karena ini bukan di dalam interrupt handler.
            __asm__ volatile("sti; hlt");
        }
    }
    return n;
}

// yield_counter didefinisikan di kernel/syscall.c, dipakai timer.c untuk CPU idle tracking
extern volatile uint32_t yield_counter;

void sys_yield(void) {
    yield_counter++; // Hint ke CPU idle tracker bahwa kita sedang menunggu
    // Tidurkan CPU sampai interrupt berikutnya (timer akan preempt otomatis)
    // AMAN: dipanggil dari kernel code, bukan dari dalam ISR.
    __asm__ volatile("sti; hlt");
}



// --- Filesystem (format = root only, mirror syscall 5) ---
int      fs_format(void) {
    if (!cred_current_is_root()) return -1;
    kfs_format();
    return 0;
}
void     fs_list(void)                  { kfs_list_files(); compositor_flush(); }
void     fs_read(char* filename)        { kfs_read_file(filename); compositor_flush(); }
void     fs_delete(char* filename)      { kfs_delete_file(filename); }

int      sys_file_exists(char* fn)      { return kfs_exists(fn); }
uint32_t sys_file_size(char* fn)        { return kfs_get_file_size(fn); }
int      sys_read_file_to_buffer(char* fn, char* buf, uint32_t cap) { return kfs_read_to_buffer(fn, buf, cap); }
int      sys_create_file(char* fn, char* data, uint32_t size) { return kfs_create_file(fn, data, size); }

// Shell engine portable API (Ring 0 passthrough).
int      sys_get_file_list(char* path, void* buffer, int max_entries) { return kfs_get_file_list(path, buffer, max_entries); }
int      sys_mkdir(char* path)        { return kfs_create_folder(path); }
uint32_t sys_get_cpu_usage(void)      { return get_cpu_usage(); }
int      sys_ping(const char* host)   { return kernel_ping(host); }
uint32_t sys_get_total_disk(void)     { return kfs_get_total_space(); }
uint32_t sys_get_used_disk(void)      { return kfs_get_used_space(); }
void     sys_sleep(uint32_t ms)       { timer_sleep_ms(ms); }

// --- Memori (size_t agar cocok dengan heap.h) ---
void*    sys_alloc(size_t size)                              { return kmalloc(size); }
void     sys_free(void* ptr)                                 { kfree(ptr); }
void*    sys_realloc(void* ptr, size_t old_sz, size_t new_sz){ return krealloc(ptr, old_sz, new_sz); }

// --- Info Sistem ---
uint64_t sys_uptime(void)    { return timer_get_ms(); }        // ms sejak boot (uint64_t, tidak overflow)
uint64_t sys_total_ram(void) { return pmm_get_total_ram(); }
uint64_t sys_used_ram(void)  { return pmm_get_used_ram(); }


void sys_get_time(uint32_t* time_array) {
    extern void rtc_read_time(uint32_t*);
    rtc_read_time(time_array);
}

// --- Identitas User (P0 Phase 1: per-task cred, root-only transition) ---
// Ring-0 contexts (console shell/login) share the same policy as syscall 27:
// only a root task may change identity. Returns 0 / -1 like the syscall.
int      sys_set_uid(uint32_t uid) {
    int self = smp_current_task_id();
    if (self < 0 || self >= task_count) return -1;
    if (!cred_transition_allowed(cred_task_uid(&tasks[self]))) return -1;
    uint64_t f = spinlock_lock_irqsave(&scheduler_lock);
    tasks[self].cred.uid = uid;
    tasks[self].cred.gid = uid;
    spinlock_unlock_irqrestore(&scheduler_lock, f);
    return 0;
}
uint32_t sys_get_uid(void) { return cred_current_uid(); }

// --- ELF Loader ---
uint64_t sys_load_elf(char* filename) {
    extern uint64_t elf_load_file(char* filename, uint64_t* out_stack_top,
                                  phys_addr_t target_pml4);
    extern void flush_event_queue(int task_id);
    extern void flush_kbd_buffer(void);
    extern int  smp_current_task_id(void);
    flush_event_queue(smp_current_task_id());
    flush_kbd_buffer();
    // Direct-launch dari kernel: tidak ada per-process AS — map ke kernel PML4.
    return elf_load_file(filename, NULL, PHYS_NULL);
}

// FIX_005 Tahap 1: launch app lewat syscall 33 — AS per-proses + iretq ke
// CPL 3. TIDAK kembali saat sukses (app exit → longjmp ke user_shell);
// kembali dengan normal hanya saat file gagal dimuat.
void sys_exec(char* filename) {
    __asm__ volatile("int $0x80" : : "a"(33), "b"((uint64_t)filename));
}

// Phase 5A: spawn app sebagai task ring-3 BARU (syscall 57) — konkuren,
// shell tetap jalan. Return task id (>= 0) atau -1 jika gagal.
int sys_spawn(char* filename) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(57), "b"((uint64_t)filename));
    return ret;
}

// P0 Phase 2: spawn dengan argv (syscall 68). Ring 0 (console shell) lewat
// jalur bypass boundary-copy seperti sys_spawn — pointer kernel sah.
int sys_spawn_argv(char* filename, int argc, char** argv) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(68), "b"((uint64_t)filename), "c"((uint64_t)argc), "d"((uint64_t)argv));
    return ret;
}

// P0 Phase 2: waitpid/getpid/getppid/proc_list (syscall 69-72). Sama seperti
// sys_spawn: int 0x80 dari Ring 0, handler memakai jalur bypass.
int sys_waitpid(int pid, int* status, int options) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(69), "b"((uint64_t)(int64_t)pid), "c"((uint64_t)status), "d"((uint64_t)(int64_t)options));
    return ret;
}
int32_t sys_getpid(void) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(70));
    return (int32_t)ret;
}
int32_t sys_getppid(void) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(71));
    return (int32_t)ret;
}
// proc_info_t didefinisikan di proc.h (via include di atas); dipakai Task
// Manager / shell. Ring 0 lewat int 0x80 seperti sys_spawn.
int sys_proc_list(proc_info_t* buf, int max) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(72), "b"((uint64_t)buf), "c"((uint64_t)(int64_t)max));
    return ret;
}
// P0 Phase 3: sys_kill (73). Ring 0 lewat int 0x80 seperti sys_spawn.
int sys_kill(int pid) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(73), "b"((uint64_t)(int64_t)pid));
    return ret;
}

// --- fd layer Ring-0 shims (P0 Phase 4/5) ---
// Console shell (Ring 0) memakai tabel fd task-nya sendiri seperti app
// Ring 3 — langsung ke vfs_* (tanpa int 0x80: tak ada pointer user yang
// perlu boundary-copy). Dipakai redirection/pipeline shell.
int sys_open(const char* path, uint32_t flags) {
    extern int vfs_open(const char* path, uint32_t flags);
    return vfs_open(path, flags);
}
int sys_read_fd(int fd, void* buf, uint32_t count) {
    extern int vfs_read(int fd, void* buf, uint32_t count);
    return vfs_read(fd, buf, count);
}
int sys_write_fd(int fd, const void* buf, uint32_t count) {
    extern int vfs_write(int fd, const void* buf, uint32_t count);
    return vfs_write(fd, buf, count);
}
int sys_lseek(int fd, int32_t offset, int whence) {
    extern int vfs_lseek(int fd, int32_t offset, int whence);
    return vfs_lseek(fd, offset, whence);
}
int sys_close(int fd) {
    extern int vfs_close(int fd);
    return vfs_close(fd);
}
int sys_dup(int oldfd) {
    extern int vfs_dup(int oldfd);
    return vfs_dup(oldfd);
}
int sys_dup2(int oldfd, int newfd) {
    extern int vfs_dup2(int oldfd, int newfd);
    return vfs_dup2(oldfd, newfd);
}
int sys_pipe(int fds[2]) {
    extern int vfs_pipe(int out[2]);
    return vfs_pipe(fds);
}
// P0 Phase 5: spawn + stdio inheritance. Lewat int 0x80 seperti
// sys_spawn_argv (spawn_common di syscall.c, bukan API vfs).
int sys_spawn_redir(char* filename, int argc, char** argv, spawn_stdio_t* spec) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(77), "b"((uint64_t)filename), "c"((uint64_t)argc), "d"((uint64_t)argv), "S"((uint64_t)spec));
    return ret;
}
// P0 Phase 6B: fork dari Ring 0 selalu ditolak kernel (-1: CS bukan
// ring 3 / tanpa user AS). Shim ada agar API generik shell lengkap.
int sys_fork(void) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(78));
    return (int)ret;
}
// P0 Phase 6C: execve dari Ring 0 selalu ditolak kernel (-1: bukan task
// user ring 3). Shim ada agar API generik shell lengkap.
int sys_execve(char* path, int argc, char** argv) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(79), "b"((uint64_t)path), "c"((uint64_t)argc), "d"((uint64_t)argv));
    return (int)ret;
}
// P0 Phase 6C: fork/exec child-exit path. Ring-0 tasks never take the
// fork-child branch (fork fails first), but shell_core references it —
// route through the syscall like the other lifecycle shims.
__attribute__((noreturn))
void sys_exit_code(int code) {
    __asm__ volatile("int $0x80" : : "a"(34), "b"((uint64_t)(int64_t)code));
    __builtin_unreachable();
}


// --- Cetak Angka ---
void print_num(uint32_t num) {
    if (num == 0) { print("0"); return; }
    char buf[16]; int i = 14; buf[15] = '\0';
    while (num > 0 && i >= 0) { buf[i--] = (char)((num % 10) + '0'); num /= 10; }
    print(&buf[i + 1]);
}

void sys_shutdown(void) {
    __asm__ volatile("int $0x80" : : "a"(38));
}

void sys_reboot(void) {
    __asm__ volatile("int $0x80" : : "a"(39));
}
// strcmp sudah ada di kernel/string.c
