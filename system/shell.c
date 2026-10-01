// system/shell.c — frontend shell konsol (TTY).
//
// Dibangun sebagai ELF user-space (Ring 3), sumber di system/ karena historis
// dulu bagian kernel image. Sama seperti cat.c/echo.c/zen.c/login.c/init.c:
// seluruh isinya lewat API userlib (include/userlib.h) — nol simbol kernel.
//
// Perubahan dari versi Ring 0:
//   - user_shell() → main() (ENTRY main di apps/app.ld)
//   - extern fs_node_t tty_node + read_fs (simbol kernel) → sys_read_keyboard
//     (syscall 3) yang memang non-blocking: return 0 bila tak ada input
//   - g_shell_return_rsp dihapus: tidak ada lagi longjmp dari sys_exit karena
//     app sekarang task ring-3 sungguhan (lihat sys_proc.c sys_exit)
#include "shell.h"
#include "userlib.h"

#include <stddef.h>
#include <stdint.h>

// ============================================================
// Console shell frontend.
//
// SEMUA perintah generik hidup di system/shell_core.c (engine bersama), termasuk
// adduser/format/install_app/nettest. File ini hanya:
//   - output → TTY (print/clear_screen)
//   - loop keyboard + prompt + input tertunda (password)
//   - mendaftarkan perintah yang MUTLAK perlu TUI konsol (logout, zen, sleep)
//
// Aturan: file ini hanya boleh memakai include/userlib.h. Jangan panggil simbol
// kernel langsung (kfs_*, timer_*, ghal_*) — itu yang dulu mengunci frontend ini
// ke Ring 0.
// ============================================================

// ---------- output adapter: shell → TTY ----------
static void tty_out(void* ctx, const char* text) { (void)ctx; print((char*)text); }
static void tty_clear(void* ctx) { (void)ctx; clear_screen(); }

// P0 Phase 6A poll: non-blocking TTY drain untuk join foreground.
// 0x03 (Ctrl-C masakan driver) dikonsumsi -> 1; ketikan susulan lain
// dibuang (terdokumentasi: tanpa type-ahead selama pipeline jalan).
//
// sys_read_keyboard (syscall 3) SUDAH non-blocking: kernel memanggil
// read_fs(&tty_node, ...) dan mengembalikan 0 bila buffer kosong. Dulu di sini
// dipakai read_fs langsung karena shell hidup di Ring 0; sekarang shell adalah
// ELF ring-3, jadi jalurnya lewat syscall.
static int console_poll_input(void* ctx) {
    (void)ctx;
    for (;;) {
        char c = 0;
        if (read_keyboard(&c, 1) == 0) return 0;
        if (c == 0x03) return 1;
    }
}

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

// ---------- perintah eksklusif console (TUI kernel) ----------
static int cmd_zen(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: zen [nama_file]"); return SHELL_ERR; }
    // zen kini ELF ring-3 (system/zen.c di-`filter-out` dari kernel image).
    // Di-spawn sebagai task baru supaya shell tetap hidup saat editor terbuka —
    // dulu zen_main() dipanggil langsung sehingga editor memblokir shell.
    char app[40];
    build_app_path(app, sizeof(app), "zen.elf");
    if (!sys_file_exists(app)) {
        shell_writeln(sh, "zen: /apps/zen.elf tidak ada (jalankan `make apps`)");
        return SHELL_ERR;
    }
    int tid = sys_spawn_argv(app, argc - 1, &argv[1]);
    if (tid < 0) { shell_writeln(sh, "zen: gagal spawn"); return SHELL_ERR; }
    return SHELL_OK;
}

static int cmd_refresh(shell_t* sh, int argc, char** argv) {
    if (argc < 2) {
        shell_write(sh, "Refresh rate saat ini: ");
        shell_writenum(sh, sys_get_refresh_rate());
        shell_writeln(sh, "Hz");
        shell_writeln(sh, "Pilihan: 60, 100, 144");
        return SHELL_OK;
    }
    uint32_t hz = 0;
    if (!parse_uint(argv[1], &hz)) { shell_writeln(sh, "Penggunaan: refresh [60|100|144]"); return SHELL_ERR; }
    // Portable: Ring 3 = syscall 88 (root-only di kernel), Ring 0 = driver timer.
    if (sys_set_refresh_rate(hz) == 0) {
        shell_write(sh, "Refresh rate diubah ke "); shell_writenum(sh, hz); shell_writeln(sh, "Hz");
    } else {
        shell_writeln(sh, "Refresh rate tidak didukung. Pilihan: 60, 100, 144");
    }
    return SHELL_OK;
}

static int cmd_gpu(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    // Portable: lewat API userlib (syscall 65). Sebelumnya memanggil
    // ghal_stats_dump() yang hanya ada di sisi kernel, sehingga perintah ini
    // terkunci ke Ring 0.
    gpu_stats_t st;
    if (sys_gpu_stats(&st) != 0) {
        shell_writeln(sh, "[gpu] statistik tidak tersedia (backend tanpa stats)");
        return SHELL_OK;
    }
    shell_write(sh, "[gpu] present=");    shell_writenum(sh, (uint32_t)st.present_count);
    shell_write(sh, "  cmd=");            shell_writenum(sh, (uint32_t)st.cmd_count);
    shell_write(sh, "  bytes=");          shell_writenum(sh, (uint32_t)st.cmd_bytes);
    shell_writeln(sh, "");
    shell_write(sh, "[gpu] notify=");     shell_writenum(sh, (uint32_t)st.notify_count);
    shell_write(sh, "  wait_calls=");     shell_writenum(sh, (uint32_t)st.wait_calls);
    shell_write(sh, "  wait_ticks=");     shell_writenum(sh, (uint32_t)st.wait_ticks);
    shell_write(sh, "  err=");            shell_writenum(sh, (uint32_t)st.err_count);
    shell_writeln(sh, "");
    return SHELL_OK;
}

static int cmd_sleep(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "Sistem memasuki mode Sleep...");
    shell_writeln(sh, "Mata CPU ditutup. Tekan tombol apapun untuk membangunkan.");
    sys_sleep(2000);   // Portable: Ring 3 = syscall 46, Ring 0 = timer_sleep_ms
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
    // P0 Phase 5: baris beroperator milik shell_execute (pipeline/redirection)
    // — jangan exec-replace shell dengan kata pertamanya.
    for (const char* q = line; *q; q++) {
        if (*q == '|' || *q == '>' || *q == '<') return 0;
    }
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

    // Spawn sebagai task ring-3 baru, bukan sys_exec (replace image).
    // sys_exec akan menggantikan shell itu sendiri — kalau app crash, shell
    // ikut hilang dan user kehilangan prompt. Dengan spawn, shell tetap hidup
    // dan `logout` tetap bisa dijalankan setelah app selesai.
    clear_screen();
    int pid = sys_spawn(app);
    if (pid < 0) {
        shell_error(sh, "Gagal menjalankan: "); shell_writeln(sh, elf);
        return 1;
    }
    // Tunggu app selesai (perilaku lama: prompt kembali setelah app keluar).
    int status = 0;
    sys_waitpid(pid, &status, 0);
    clear_screen();
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

void main(int argc, char** argv) {
    // argv[1] = uid target (dari login.elf). Shell di-spawn SEBELUM login
    // menurunkan uid supaya mewarisi root; penurunan dilakukan di sini setelah
    // proses hidup. Kalau login menurunkan uid lebih dulu, shell mewarisi uid
    // non-root dan perintah root-only (format/shutdown/reboot) ditolak
    // selamanya — `sudo` hanya flag UX, batas aslinya di kernel.
    if (argc >= 2 && argv[1] && argv[1][0]) {
        uint32_t want = 0;
        for (int i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
            want = want * 10u + (uint32_t)(argv[1][i] - '0');
        if (want != 0) sys_set_uid(want);   // root-only; gagal = tetap root
    }

    char cmd_buffer[256];
    int cmd_index = 0;
    char key_buffer[2];

    shell_io_t io;
    io.out = tty_out; io.err = tty_out; io.clear = tty_clear; io.ctx = NULL;
    io.poll_input = console_poll_input;
    shell_t* sh = shell_init(&io);

    // Perintah frontend konsol (perintah generik ada di shell_core.c).
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
            if (c == 0x03) {
                // P0 Phase 6A: Ctrl-C tanpa foreground — bukan kill (tak ada
                // yang dibunuh), hanya baris baru + prompt segar. (Saat
                // pipeline jalan, loop ini tak berjalan; 0x03 dimakan poll
                // join sebagai interupsi foreground.)
                print("^C\n");
                cmd_index = 0;
                print(prompt);
            } else if (c == '\n') {
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
                        if (st == SHELL_STATUS_EXIT) {
                            // `logout`/`exit`: keluar dari loop. login.elf
                            // menunggu kami lewat waitpid, lalu menampilkan
                            // layar login lagi.
                            clear_screen();
                            sys_exit();
                        }
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
