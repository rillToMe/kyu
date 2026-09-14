#include "shell.h"
#include "userlib.h"
#include "zen.h"
#include "timer.h"   // timer_get_refresh_rate / timer_set_refresh_rate / sleep
#include "task.h"

#include <stddef.h>
#include <stdint.h>

// Phase 2C §9.6 — statistik GPU (graphics/ghal.c)
extern void ghal_stats_dump(void);

// RSP shell disimpan sebelum sys_exec — dipakai syscall.c (sys_exit app) untuk
// longjmp kembali ke user_shell. 0 = tidak ada sesi exec aktif.
uint64_t g_shell_return_rsp = 0;

// ============================================================
// Console shell frontend.
//
// SEMUA perintah generik hidup di apps/shell_core.c (engine bersama), termasuk
// adduser/format/install_app/nettest. File ini hanya:
//   - output → TTY (print/clear_screen)
//   - loop keyboard + prompt + input tertunda (password)
//   - mendaftarkan perintah yang MUTLAK perlu Ring 0 / TUI kernel
//     (logout, zen, refresh, gpu, sleep)
// ============================================================

// TCP socket wrappers (syscall 52-56). Weak: kernel shell links these; ELF apps
// use the strong versions in apps/userlib.c — dipakai engine `nettest`.
__attribute__((weak))
int sys_socket(void) {
    int64_t ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(52ULL));
    return (int)ret;
}
__attribute__((weak))
int sys_connect(int s, uint32_t ip_be, uint16_t port) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(53ULL), "b"((uint64_t)s), "c"((uint64_t)ip_be), "d"((uint64_t)port));
    return (int)ret;
}
__attribute__((weak))
int sys_send(int s, const void *buf, uint32_t len) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(54ULL), "b"((uint64_t)s), "c"((uint64_t)buf), "d"((uint64_t)len));
    return (int)ret;
}
__attribute__((weak))
int sys_recv(int s, void *buf, uint32_t len) {
    int64_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(55ULL), "b"((uint64_t)s), "c"((uint64_t)buf), "d"((uint64_t)len));
    return (int)ret;
}
__attribute__((weak))
int sys_sock_close(int s) {
    int64_t ret; __asm__ volatile("int $0x80" : "=a"(ret) : "a"(56ULL), "b"((uint64_t)s));
    return (int)ret;
}

// ---------- output adapter: shell → TTY ----------
static void tty_out(void* ctx, const char* text) { (void)ctx; print((char*)text); }
static void tty_clear(void* ctx) { (void)ctx; clear_screen(); }

static int parse_uint(const char* str, uint32_t* out) {
    if (str == NULL || out == NULL) return 0;
    while (*str == ' ') str++;
    if (*str == '\0') return 0;
    uint32_t value = 0;
    while (*str != '\0') {
        if (*str < '0' || *str > '9') return 0;
        value = value * 10 + (uint32_t)(*str - '0');
        str++;
    }
    *out = value;
    return 1;
}

// ---------- perintah eksklusif console (Ring 0 / TUI kernel) ----------
static int cmd_zen(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: zen [nama_file]"); return SHELL_ERR; }
    zen_main(argv[1]);
    clear_screen();
    return SHELL_OK;
}

static int cmd_refresh(shell_t* sh, int argc, char** argv) {
    if (argc < 2) {
        shell_write(sh, "Refresh rate saat ini: ");
        shell_writenum(sh, timer_get_refresh_rate());
        shell_writeln(sh, "Hz");
        shell_writeln(sh, "Pilihan: 60, 100, 144");
        return SHELL_OK;
    }
    uint32_t hz = 0;
    if (!parse_uint(argv[1], &hz)) { shell_writeln(sh, "Penggunaan: refresh [60|100|144]"); return SHELL_ERR; }
    if (timer_set_refresh_rate(hz) == 0) {
        shell_write(sh, "Refresh rate diubah ke "); shell_writenum(sh, hz); shell_writeln(sh, "Hz");
    } else {
        shell_writeln(sh, "Refresh rate tidak didukung. Pilihan: 60, 100, 144");
    }
    return SHELL_OK;
}

static int cmd_gpu(shell_t* sh, int argc, char** argv) {
    (void)sh; (void)argc; (void)argv;
    ghal_stats_dump();
    return SHELL_OK;
}

static int cmd_sleep(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "Sistem memasuki mode Sleep...");
    shell_writeln(sh, "Mata CPU ditutup. Tekan tombol apapun untuk membangunkan.");
    timer_sleep_ms(2000);
    clear_screen();
    char dummy[2];
    while (read_keyboard(dummy, 1) == 0) sys_yield();
    clear_screen();
    return SHELL_OK;
}

// ---------- frontend console ----------
// Fallback kompatibilitas console: perintah tak terdaftar dicoba dieksekusi
// sebagai /apps/<nama>.elf (perilaku lama "ketik nama app"). Return 1 bila
// sudah ditangani (termasuk pesan gagal-muat), 0 bila bukan app.
static int try_implicit_exec(shell_t* sh, const char* line) {
    char first[32]; int fi = 0;
    while (line[fi] && line[fi] != ' ' && fi < 31) { first[fi] = line[fi]; fi++; }
    first[fi] = '\0';
    if (!first[0] || shell_has_command(sh, first)) return 0;

    char elf[32]; int ei = 0;
    while (first[ei] && ei < 27) { elf[ei] = first[ei]; ei++; }
    elf[ei] = '\0';
    int has_ext = (ei >= 4 && elf[ei-4]=='.' && elf[ei-3]=='e' && elf[ei-2]=='l' && elf[ei-1]=='f');
    if (!has_ext && ei < 28) { elf[ei++]='.'; elf[ei++]='e'; elf[ei++]='l'; elf[ei++]='f'; elf[ei]='\0'; }

    char app[40]; build_app_path(app, sizeof(app), elf);
    if (!sys_file_exists(app)) return 0;

    __asm__ volatile("mov %%rsp, %0" : "=m"(g_shell_return_rsp) :: "memory");
    clear_screen();
    sys_exec(app);
    // sys_exec tidak kembali saat sukses; sampai sini = gagal memuat.
    shell_error(sh, "Gagal memuat: "); shell_writeln(sh, elf);
    return 1;
}

// Baca satu baris dari keyboard. mask=1 → tampilkan '*'.
static void read_line(char* out, int cap, int mask) {
    int n = 0;
    for (;;) {
        char c;
        if (read_keyboard(&c, 1) > 0) {
            if (c == '\n') { out[n] = '\0'; print("\n"); return; }
            else if (c == '\b') { if (n > 0) { print("\b \b"); n--; } }
            else if (n < cap - 1) {
                out[n++] = c;
                if (mask) print("*");
                else { char s[2] = { c, '\0' }; print(s); }
            }
        }
        sys_yield();
    }
}

void user_shell(void) {
    char cmd_buffer[256];
    int cmd_index = 0;
    char key_buffer[2];

    shell_io_t io;
    io.out = tty_out; io.err = tty_out; io.clear = tty_clear; io.ctx = NULL;
    shell_t* sh = shell_init(&io);

    // Perintah yang mutlak butuh Ring 0 / TUI kernel.
    shell_register_command(sh, "zen",     cmd_zen,     "Buka teks editor");
    shell_register_command(sh, "refresh", cmd_refresh, "Atur refresh rate");
    shell_register_command(sh, "gpu",     cmd_gpu,     "Statistik GPU");
    shell_register_command(sh, "sleep",   cmd_sleep,   "Sleep OS");

    char prompt[64];
    shell_build_prompt(prompt, sizeof(prompt), "@kyuzen> ");

    print("Selamat datang di Kyuzen OS.\n");
    print(prompt);

    while (1) {
        uint32_t bytes_read = read_keyboard(key_buffer, 1);
        if (bytes_read > 0) {
            char c = key_buffer[0];
            if (c == '\n') {
                print("\n");
                cmd_buffer[cmd_index] = '\0';
                if (cmd_index > 0) {
                    if (!try_implicit_exec(sh, cmd_buffer)) {
                        int st = shell_execute(sh, cmd_buffer);
                        if (st == SHELL_STATUS_ASK_INPUT) {
                            // sudo/adduser minta input; shell sudah cetak prompt.
                            char in[64];
                            read_line(in, sizeof(in), shell_pending_mask(sh));
                            st = shell_supply_input(sh, in);
                        }
                        if (st == SHELL_STATUS_EXIT) break;
                    }
                }
                cmd_index = 0;
                print(prompt);
            } else if (c == '\b') {
                if (cmd_index > 0) { print("\b"); cmd_index--; }
            } else {
                if (cmd_index < 255) {
                    cmd_buffer[cmd_index++] = c;
                    char char_str[2] = { c, '\0' };
                    print(char_str);
                }
            }
        }
        sys_yield();
    }
}
