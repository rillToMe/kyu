#ifndef USERLIB_H
#define USERLIB_H

#include <stdint.h>
#include <stddef.h>   // size_t
#include "proc.h"     // P0 Phase 2: PROC_* bounds, proc_info_t, wait/exit ABI
#include "crash_notice.h"  // sys_crash_notice: notifikasi crash (boot setelah panic)
#include "kwm_abi.h"  // kwm_window_info_t kanonis (shared kernel<->user, syscall 61)

// --- STRUKTUR PESAN EVENT (GUI) ---
#define EVENT_NONE          0
#define EVENT_KEY_PRESS     1   // Key down. P1 = ASCII (shift/caps diterapkan; 0 = non-printable)
                                // P2 = modifier bitmask (KEY_MOD_*), P3 = scancode
#define EVENT_MOUSE_MOVE    2
#define EVENT_MOUSE_CLICK   3
#define EVENT_SCROLL        4   // P1 = delta wheel (+1 bawah / -1 atas)
#define EVENT_KEY_RELEASE   5   // Key up. P1 = ASCII dasar (identitas tombol, tanpa shift/caps)
                                // P2 = modifier bitmask (setelah release diproses), P3 = scancode
#define EVENT_WIN_CLOSE     6   // (Phase 5C — dicadangkan) WM meminta app menutup window.
                                // win_id = window yang diminta; P1..P3 = 0.
#define EVENT_WALLPAPER_RELOAD 7 // Permintaan reload wallpaper (syscall 84):
                                // antre ke task pemilik window desktop; P1..P3
                                // = 0, win_id = 0. Desktop memuat ulang dari
                                // konfigurasi persisten di loop normalnya.

// Bitmask modifier keyboard (P2 pada EVENT_KEY_PRESS / EVENT_KEY_RELEASE)
#define KEY_MOD_SHIFT       0x01   // Shift kiri/kanan
#define KEY_MOD_CTRL        0x02   // Ctrl kiri/kanan
#define KEY_MOD_ALT         0x04   // Alt kiri/kanan
#define KEY_MOD_CAPS        0x08   // CapsLock sedang aktif

// P3 = scancode set-1; bit 0x100 menyala = tombol extended (prefix E0,
// mis. Ctrl/Alt kanan, arrow keys). Pairing press↔release via P3, bukan P1.

typedef struct {
    uint32_t type;    // Jenis Event (Key, Mouse, Click)
    int32_t param1;   // Data 1 (ASCII huruf, atau X Mouse, atau Tombol Kiri/Kanan)
    int32_t param2;   // Data 2 (Y Mouse, atau Status Ditekan/Dilepas)
    int32_t param3;   // Tambahan
    int32_t win_id;   // Phase 5B: window tujuan event, diisi KWM saat routing.
                      // Nilai = id slot KWM + 1; 0 = tidak relevan.
} kyuzen_event_t;
// ----------------------------------

void print(char* text);
void clear_screen(void);
uint32_t read_keyboard(char* buffer, uint32_t size);
void sys_yield(void);
void sys_sleep(uint32_t ms);   // Non-busy sleep (Syscall 46)

int fs_format(void);   // 0 sukses, -1 ditolak (butuh root). Mirror syscall 5.
void fs_list(void);
void fs_read(char* filename);
// Hapus file ATAU folder kosong (rmdir-style). Return 0 sukses, -1 gagal
// (path salah / folder tidak kosong). Dulu void — caller lama tetap boleh
// mengabaikan hasilnya.
int  fs_delete(char* filename);

void* sys_alloc(uint32_t size);
void sys_free(void* ptr);
void* sys_realloc(void* ptr, uint32_t old_size, uint32_t new_size);
int sys_file_exists(char* filename);
uint32_t sys_file_size(char* filename);
int sys_read_file_to_buffer(char* filename, char* buffer, uint32_t buffer_capacity);
int sys_create_file(char* filename, char* data, uint32_t size);

uint64_t sys_uptime(void);

uint64_t sys_total_ram(void);
uint64_t sys_used_ram(void);

void print_num(uint32_t num);
extern int strcmp(const char *s1, const char *s2);

void get_cpu_string(char* buffer);


void sys_get_time(uint32_t* time_array);

void sys_draw_pixel(int x, int y, uint32_t color);

void sys_draw_image(int x, int y, int width, int height, uint32_t* buffer);

typedef struct {
    char filename[24];
    uint32_t size;
    uint8_t is_folder;
} file_info_t;

// App binary (.elf) pindah ke /apps/ (Fase 3 KyuzenFS). Bangun path lengkap
// "/apps/<nama>" dengan bound-check; pakai INI untuk semua launch app, jangan
// concat manual. Static inline → tersedia di semua app tanpa link dependency.
#define KYUZEN_APPS_DIR "/apps/"

static inline void build_app_path(char* out, int out_cap, const char* elf_name) {
    int k = 0;
    const char* d = KYUZEN_APPS_DIR;
    while (d[k] && k < out_cap - 1) { out[k] = d[k]; k++; }
    for (int i = 0; elf_name[i] && k < out_cap - 1; i++) out[k++] = elf_name[i];
    out[k] = '\0';
}

int sys_get_file_list(char* path, file_info_t* buffer, int max_entries);
int sys_mkdir(char* path);

// Phase 2C §9.6 — statistik GPU (mirror ghal_gpu_stats_t, ABI syscall 65).
// Counter kumulatif sejak boot; hitung delta dua sampel untuk per-frame.
// Layout HARUS sama dengan ghal_gpu_stats_t di graphics/ghal.h.
typedef struct {
    uint64_t present_count;
    uint64_t cmd_count;
    uint64_t cmd_bytes;
    uint64_t notify_count;
    uint64_t wait_calls;
    uint64_t wait_ticks;
    uint64_t err_count;
} gpu_stats_t;
int sys_gpu_stats(gpu_stats_t* out);   // -> 0 sukses, -1 backend tanpa stats

uint64_t sys_load_elf(char* filename);

// sys_exec: Load app baru, replace current app, TIDAK PERNAH kembali ke caller.
// OS yang free RAM lama, load app baru, lalu lompat langsung ke entry-nya.
void sys_exec(char* filename);

// sys_spawn (Phase 5A): jalankan ELF sebagai task ring-3 BARU yang konkuren —
// caller TETAP jalan (beda dengan sys_exec yang menggantikan caller).
// Return: task id (>= 0), atau -1 jika gagal (file tak ada, OOM, slot penuh).
// P0 Phase 2: argc=1, argv[0]=basename(path). Full argv via sys_spawn_argv.
int sys_spawn(char* filename);

// sys_spawn_argv (P0 Phase 2): seperti sys_spawn tapi dengan argv.
// argv[0] = nama app, argv[1..] = argumen; argc = 1..PROC_MAX_ARGC, tiap
// string NUL-terminated <= PROC_MAX_ARG_LEN, total <= PROC_ARG_TOTAL_MAX.
// Return: child pid (>= 0) atau -1 (file tak ada / argumen tak valid / OOM).
int sys_spawn_argv(char* filename, int argc, char** argv);

// sys_exit: App selesai, kembali ke shell. TIDAK PERNAH kembali ke caller.
// P0 Phase 2: exit(0). Status eksplisit via sys_exit_code.
void sys_exit(void);
// sys_exit_code: seperti sys_exit tapi merekam status untuk waitpid parent.
void sys_exit_code(int code) __attribute__((noreturn));

// P0 Phase 2 — process identity/wait (syscall 69-72). waitpid memblokir
// (tanpa polling) sampai child keluar; hanya boleh menunggu child sendiri.
int sys_waitpid(int pid, int* status, int options);  // -> child pid / -1
int32_t sys_getpid(void);    // -> task id (-1 jika idle)
int32_t sys_getppid(void);   // -> parent id (PROC_NO_PARENT jika tak ada)
// Snapshot proses hidup untuk Task Manager (read-only). -> jumlah / -1.
int sys_proc_list(proc_info_t* buf, int max);
// P0 Phase 3 — sys_kill (syscall 73): minta terminasi pid. -> 0 sukses,
// -1 ditolak (bukan child / bukan root / PID tak valid / sudah keluar /
// task kernel). Child yang di-kill dilaporkan waitpid dengan
// PROC_KILL_EXIT_CODE / PROC_EXIT_KILLED.
int sys_kill(int pid);
void sys_draw_string(const char* str, int x, int y, uint32_t color);

int sys_set_uid(uint32_t uid);   // 0 sukses, -1 ditolak (butuh root). Tak ada setuid.
uint32_t sys_get_uid();          // UID task pemanggil (per-task cred).

// Identitas bersama (apps/userutil.c): isi `out` dengan username akun yang
// UID-nya == sys_get_uid(), dibaca dari users.sys ("username:password:uid").
// Bukan autentikasi — hanya resolusi nama untuk prompt. Return 1 sukses,
// 0 gagal (out dikosongkan). `cap` termasuk terminator.
int current_username(char* out, uint32_t cap);

// True jika `pw` cocok dengan password akun UID saat ini (users.sys).
// Dipakai `sudo` untuk verifikasi. Bukan autentikasi login (tidak mengubah
// state apa pun). Return 1 cocok, 0 tidak.
int current_password_match(const char* pw);

// 2. Syscall Event Queue (Syscall 29)
int sys_get_event(kyuzen_event_t* event_out);

// 3. Syscall Window Manager (Syscall 30, 31, 32, 40)
int sys_create_window(int x, int y, uint32_t width, uint32_t height);
void sys_update_window(int win_id, uint32_t* buffer);
void sys_destroy_window(int win_id);

void* memcpy(void* dest, const void* src, size_t count);
void* memset(void* dest, int val, size_t count);

int sys_kwm_create_window(int x, int y, uint32_t width, uint32_t height);
void sys_kwm_update_window(int win_id, uint32_t* buffer);
void sys_kwm_destroy_window(int win_id);

// --- Phase 3: partial window update (syscall 66) ---
// Kirim HANYA sebuah rect konten window. buffer tetap canvas penuh app
// (stride = window_width * 4 byte); kernel menyalin baris/kolom rect saja.
// Rect dalam koordinat konten window-local. Return 0 sukses, -1 ditolak.
// Syscall 31 (sys_kwm_update_window) tetap ada untuk full-canvas update.
typedef struct {
    int32_t   win_id;
    int32_t   x;
    int32_t   y;
    uint32_t  width;
    uint32_t  height;
    uint32_t* buffer;
} kwm_rect_update_t;
int sys_kwm_update_window_rect(kwm_rect_update_t* req);

// --- Phase 19: declare a window canvas fully opaque (syscall 67) ---
// Owner-only. The kernel scans the WHOLE canvas and accepts only if every pixel
// has a non-zero alpha byte; otherwise it rejects and the window keeps the
// scalar compositor path. Purely a performance hint for the base-blit elision
// (Phase 16-18) + opaque memcpy fast-path (Phase 14) — never a render semantic.
// Call once, after the canvas is fully painted (e.g. libgui's initial fill).
// Return 0 accepted / -1 rejected.
int sys_kwm_set_window_opaque(int win_id);

// sys_kwm_set_cursor (Phase 9): ganti bentuk kursor global (0 panah / 1 I-beam / 2 tangan).
int sys_kwm_set_cursor(int kind);

// --- Phase 10: Desktop window + taskbar (syscall 59-63) ---
// kwm_window_info_t: definisi kanonis di include/kwm_abi.h (dipakai kernel
// juga — satu layout untuk syscall 61, jangan duplikasi di sini).
int sys_kwm_create_desktop(void);        // -> win_id / -1 (frameless full-screen z=0)
int sys_kwm_set_title(int win_id, const char* title);   // 0 / -1 (hanya pemilik)
int sys_kwm_get_windows(kwm_window_info_t* buf, int max);   // -> jumlah / -1
int sys_kwm_activate_window(int win_id); // bring-to-front + fokus; 0 / -1
int sys_get_screen_size(uint32_t* w, uint32_t* h);   // -> 0 / -1

// sys_wallpaper_reload (syscall 84): minta Desktop yang berjalan memuat ulang
// wallpaper dari konfigurasi persisten (/wallpaper.ui, fallback manifest).
// Tanpa argumen (tanpa path user — tak ada validasi pointer yang diperlukan).
// Return 0 = permintaan diterima (event antre ke task desktop; hasil decode
// dilaporkan terpisah — bukan janji gambar termuat, dan tak menunggu decode),
// -1 = Desktop tak tersedia (tanpa window desktop). Boleh dipanggil task
// mana pun: hanya notifikasi bertipe tetap, tanpa akses compositor/memori.
int sys_wallpaper_reload(void);   // -> 0 / -1

// sys_crash_notice (syscall 80): isi *out dengan ringkasan crash terakhir.
// Return 1 kalau BOOT INI baru menerbitkan laporan crash (yaitu boot tepat
// setelah sistem panic), 0 kalau tidak ada — jadi aplikasi bisa menampilkan
// notifikasi sekali saja, bukan tiap startup. Dipakai desktop.
int sys_crash_notice(crash_notice_t* out);

void sys_shutdown(void);
void sys_reboot(void);

//statistik hardware
uint32_t sys_get_total_disk(void);
uint32_t sys_get_used_disk(void); 
uint32_t sys_get_cpu_usage(void);

// sys_ping: kirim ICMP Echo Request (ping) ke host.
// host = nama domain atau IP string ("google.com" atau "8.8.8.8")
// Return: rata-rata RTT dalam ms jika berhasil, -1 jika gagal/timeout
// Output ping dicetak langsung oleh kernel (kprint) ke TTY.
int sys_ping(const char *host);

// Diagnostic: process isolation testing
uint64_t sys_get_cr3(void);   // Return CR3 physical address (PML4 pointer)
int sys_get_task_id(void);    // Return current task ID (-1 if idle)
int sys_is_mapped(void* addr); // Return 1 if addr is mapped, 0 if not (safe probe)
uint32_t sys_get_pid(void);   // Return per-AS unique cookie (OS-generated)

// fd layer (Fase 5) — file descriptors over KyuzenFS. fd valid per-task.
// open flags
#define O_RDONLY  0x0
#define O_WRONLY  0x1
#define O_RDWR    0x2
#define O_CREAT   0x4
#define O_TRUNC   0x8
#define O_APPEND  0x10
// lseek whence
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

int sys_open(const char* path, uint32_t flags);        // -> fd (>=0) or -1
int sys_read_fd(int fd, void* buf, uint32_t count);     // -> bytes read
int sys_write_fd(int fd, const void* buf, uint32_t count); // -> bytes written
int sys_lseek(int fd, int32_t offset, int whence);      // -> new position
int sys_close(int fd);                                  // -> 0 or -1

// --- Fase 4: filesystem tree (syscall 81-83) ----------------------------
// Resolusi path lengkap ada DI KERNEL dan berlaku untuk semua API di atas:
// "/", "//", "/a//b", ".", ".." ("/a/b/../c"), trailing '/' ("a/b/"),
// dan nama tanpa '/' (relatif root — KyuzenOS tidak punya cwd).

// Buka direktori: sys_open(dir, O_RDONLY) -> fd, lalu enumerasi dengan
// sys_readdir. Isi mentah TERMASUK "." dan ".." (lewati bila tidak perlu).
// Return 0 = ada entri, -1 = habis / fd bukan direktori.
int sys_readdir(int fd, uint32_t index, char* name, uint32_t cap, uint8_t* is_dir);

// Ganti nama / pindah file atau folder. Tidak menimpa target yang sudah ada.
// Return 0 sukses, -1 gagal (path salah / target ada / folder tidak kosong).
int sys_rename(const char* old_path, const char* new_path);

// Ukuran + tipe sebuah path. Kedua out-param boleh NULL.
// Return 0 sukses, -1 bila path tidak ada.
int sys_stat(const char* path, uint32_t* size, uint8_t* is_dir);
// P0 Phase 4: dup shares the open description (one offset, one buffer).
// sys_dup (74): oldfd -> lowest free fd. sys_dup2 (75): oldfd -> newfd
// (no-op if equal, closes newfd first). -> newfd or -1.
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
// P0 Phase 5: pipe (76): fds[0]=read, fds[1]=write, both in caller table.
// -> 0 ok / -1 fail (both-or-neither, no half-created pipe).
int sys_pipe(int fds[2]);
// P0 Phase 5: spawn with explicit stdio inheritance (77): like
// sys_spawn_argv, plus spec naming the caller's fds for the child's
// 0/1/2 (NULL or {-1,-1,-1} = fresh console TTY). -> child pid / -1.
int sys_spawn_redir(char* filename, int argc, char** argv, spawn_stdio_t* spec);
// P0 Phase 6B: fork (78): duplicate the caller. Parent gets child pid,
// child resumes after this call with 0, -1 on failure (no user AS / OOM /
// no slot). Ring-3 only; kernel contexts are rejected by the kernel.
int sys_fork(void);
// P0 Phase 6C: execve (79): atomically replace the CALLER's image with
// path+argv. Same pid/ppid/creds/fds; new stack/entry/argv. Returns only
// on failure (-1, old image untouched). Ring-3 user tasks only.
int sys_execve(char* path, int argc, char** argv);

// TCP client sockets (Fase 6). ip_be = IPv4 in network byte order.
int sys_socket(void);                                   // -> sockfd or -1
int sys_connect(int s, uint32_t ip_be, uint16_t port);  // 0 ok, -1 fail
int sys_send(int s, const void* buf, uint32_t len);     // bytes sent or -1
int sys_recv(int s, void* buf, uint32_t len);           // bytes, 0=closed, -1=err
int sys_sock_close(int s);                              // -> 0 or -1

#endif
