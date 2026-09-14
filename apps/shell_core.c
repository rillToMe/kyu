// ============================================================
// apps/shell_core.c — Kyuzen Shell engine (shared by all frontends).
//
// Parser + command registry + built-in commands + identity + output
// abstraction. Dikompilasi ke kernel (console shell) DAN ke terminal.elf
// (GUI frontend): satu implementasi perintah, banyak terminal.
//
// Hanya memakai API generik yang tersedia di kedua konteks (userlib.h):
// sys_get_uid/sys_get_file_list/sys_read_file_to_buffer/sys_spawn/... —
// tidak ada dependensi libui, KWM, atau kernel-internal.
// ============================================================

#include "shell.h"
#include "userlib.h"

#include <stddef.h>

#define SHELL_LINE_MAX 256
#define SHELL_MAX_ARGV 16

typedef struct {
    const char* name;
    shell_cmd_fn fn;
    const char* desc;
    uint8_t sudo;          // 1 = hanya boleh lewat `sudo <name>`
} shell_cmd_entry_t;

struct shell_s {
    const shell_io_t* io;
    shell_cmd_entry_t cmds[SHELL_MAX_CMDS];
    int n;
    // Alur input tertunda (sudo password / password user baru).
    uint8_t pending;                 // 0 = tidak ada (lihat PEND_*)
    uint8_t pending_mask;            // 1 = input disembunyikan (password)
    uint8_t elevated;                // 1 = sedang berjalan sbg root (sudo)
    char    pending_prompt[48];      // prefix yang sudah dicetak shell
    char    pending_line[SHELL_LINE_MAX];   // perintah dalam (sudo)
    char    pending_arg[32];         // argumen (mis. username adduser)
};

#define PEND_NONE    0
#define PEND_SUDO    1
#define PEND_ADDUSER 2

static shell_t g_shell;

// ---------- util ----------
static int s_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void str_copy(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void u32_str(uint32_t v, char* out) {
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char t[16]; int i = 0;
    while (v > 0 && i < 15) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0; while (i > 0) out[j++] = t[--i]; out[j] = '\0';
}

// ---------- output ----------
void shell_write(shell_t* sh, const char* text) {
    if (sh && sh->io && sh->io->out && text) sh->io->out(sh->io->ctx, text);
}
void shell_error(shell_t* sh, const char* text) {
    if (!sh || !sh->io || !text) return;
    if (sh->io->err) sh->io->err(sh->io->ctx, text);
    else if (sh->io->out) sh->io->out(sh->io->ctx, text);
}
void shell_writeln(shell_t* sh, const char* text) {
    shell_write(sh, text ? text : "");
    shell_write(sh, "\n");
}
void shell_writenum(shell_t* sh, uint32_t n) {
    char b[16]; u32_str(n, b); shell_write(sh, b);
}

// ---------- identity / prompt ----------
int shell_username(char* out, uint32_t cap) { return current_username(out, cap); }

void shell_build_prompt(char* out, uint32_t cap, const char* suffix) {
    if (!out || cap == 0) return;
    uint32_t k = 0;
    out[0] = '\0';
    char user[32];
    if (!shell_username(user, sizeof(user))) {
        const char* fb = "user";
        for (int i = 0; fb[i] && k < cap - 1; i++) out[k++] = fb[i];
    } else {
        for (int i = 0; user[i] && k < cap - 1; i++) out[k++] = user[i];
    }
    if (suffix) for (int i = 0; suffix[i] && k < cap - 1; i++) out[k++] = suffix[i];
    out[k] = '\0';
}

uint32_t shell_uid(shell_t* sh) { (void)sh; return sys_get_uid(); }

// ---------- registry ----------
int shell_register_command(shell_t* sh, const char* name, shell_cmd_fn fn,
                           const char* desc) {
    if (!sh || !name || !fn || sh->n >= SHELL_MAX_CMDS) return -1;
    for (int i = 0; i < sh->n; i++)
        if (strcmp(sh->cmds[i].name, name) == 0) return -1;  // duplikat
    sh->cmds[sh->n].name = name;
    sh->cmds[sh->n].fn   = fn;
    sh->cmds[sh->n].desc = desc;
    sh->cmds[sh->n].sudo = 0;
    sh->n++;
    return 0;
}

int shell_require_sudo(shell_t* sh, const char* name) {
    if (!sh || !name) return -1;
    for (int i = 0; i < sh->n; i++)
        if (strcmp(sh->cmds[i].name, name) == 0) { sh->cmds[i].sudo = 1; return 0; }
    return -1;
}

// ---------- built-in commands ----------
static int cmd_help(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "Perintah User Space:");
    for (int i = 0; i < sh->n; i++) {
        const char* nm = sh->cmds[i].name;
        shell_write(sh, "  ");
        shell_write(sh, nm);
        int pad = 10 - s_len(nm);
        for (int k = 0; k < (pad > 0 ? pad : 1); k++) shell_write(sh, " ");
        shell_write(sh, "- ");
        shell_writeln(sh, sh->cmds[i].desc ? sh->cmds[i].desc : "");
        if (sh->cmds[i].sudo) shell_writeln(sh, "  (butuh: sudo)");
    }
    return SHELL_OK;
}

static int cmd_clear(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    if (sh->io && sh->io->clear) sh->io->clear(sh->io->ctx);
    return SHELL_OK;
}

static int cmd_echo(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: echo [teks]"); return SHELL_ERR; }
    for (int i = 1; i < argc; i++) {
        shell_write(sh, argv[i]);
        if (i + 1 < argc) shell_write(sh, " ");
    }
    shell_write(sh, "\n");
    return SHELL_OK;
}

static int cmd_ls(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    file_info_t fi[32];
    int n = sys_get_file_list("/", fi, 32);
    if (n < 0) { shell_writeln(sh, "ls: gagal"); return SHELL_ERR; }
    if (n == 0) { shell_writeln(sh, "(kosong)"); return SHELL_OK; }
    shell_writeln(sh, "--- Isi Hard Disk ---");
    for (int i = 0; i < n; i++) {
        shell_write(sh, fi[i].is_folder ? "[dir] " : "      ");
        shell_writeln(sh, fi[i].filename);
    }
    return SHELL_OK;
}

static int cmd_baca(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: baca [nama_file]"); return SHELL_ERR; }
    if (!sys_file_exists(argv[1])) { shell_writeln(sh, "baca: file tidak ada"); return SHELL_ERR; }
    static char buf[4096];
    sys_read_file_to_buffer(argv[1], buf, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    shell_writeln(sh, buf);
    return SHELL_OK;
}

static int cmd_hapus(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: hapus [nama_file]"); return SHELL_ERR; }
    fs_delete(argv[1]);
    shell_writeln(sh, "dihapus");
    return SHELL_OK;
}

static int cmd_mkdir(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: mkdir [path]"); return SHELL_ERR; }
    if (sys_mkdir(argv[1]) == 1) shell_writeln(sh, "folder dibuat");
    else shell_writeln(sh, "mkdir: gagal (path tak valid / sudah ada)");
    return SHELL_OK;
}

static int cmd_fetch(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    char cpu[49]; get_cpu_string(cpu);
    uint32_t used = (uint32_t)(sys_used_ram() / 1024 / 1024);
    uint32_t tot  = (uint32_t)(sys_total_ram() / 1024 / 1024);
    shell_writeln(sh, "OS   : KyuzenOS");
    shell_writeln(sh, "Arch : x86_64 (Long Mode)");
    shell_write(sh, "CPU  : "); shell_writeln(sh, cpu);
    shell_write(sh, "RAM  : "); shell_writenum(sh, used);
    shell_write(sh, " MB / ");  shell_writenum(sh, tot); shell_writeln(sh, " MB");
    return SHELL_OK;
}

static int cmd_neofetch(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    char cpu[49]; get_cpu_string(cpu);
    char user[32]; shell_username(user, sizeof(user));
    uint32_t used = (uint32_t)(sys_used_ram() / 1024 / 1024);
    uint32_t tot  = (uint32_t)(sys_total_ram() / 1024 / 1024);
    shell_writeln(sh, "        /\\        KyuzenOS");
    shell_writeln(sh, "       /  \\       --------");
    shell_write(sh, "      /____\\      User : "); shell_writeln(sh, user[0] ? user : "user");
    shell_write(sh, "     /      \\     CPU  : "); shell_writeln(sh, cpu);
    shell_write(sh, "    /________\\    RAM  : ");
    shell_writenum(sh, used); shell_write(sh, " MB / ");
    shell_writenum(sh, tot);  shell_writeln(sh, " MB");
    return SHELL_OK;
}

static int cmd_sched(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_write(sh, "CPU  : "); shell_writenum(sh, sys_get_cpu_usage());
    shell_writeln(sh, "%");
    return SHELL_OK;
}

static int cmd_time(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    uint32_t t[6];   // [year, month, day, hour, min, sec]
    sys_get_time(t);
    char b[16]; int k = 0;
    uint32_t p[3] = { t[3], t[4], t[5] };
    for (int i = 0; i < 3; i++) {
        if (i) b[k++] = ':';
        if (p[i] < 10) b[k++] = '0';
        char n[16]; u32_str(p[i], n);
        for (int j = 0; n[j]; j++) b[k++] = n[j];
    }
    b[k] = '\0';
    shell_writeln(sh, b);
    return SHELL_OK;
}

static int cmd_start(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: start [app]"); return SHELL_ERR; }
    char elf[32]; int i = 0;
    while (argv[1][i] && i < 27) { elf[i] = argv[1][i]; i++; }
    elf[i] = '\0';
    int has_ext = (i >= 4 && elf[i-4]=='.' && elf[i-3]=='e' && elf[i-2]=='l' && elf[i-1]=='f');
    if (!has_ext && i < 28) { elf[i++]='.'; elf[i++]='e'; elf[i++]='l'; elf[i++]='f'; elf[i]='\0'; }
    char app[40]; build_app_path(app, sizeof(app), elf);
    if (!sys_file_exists(app)) { shell_writeln(sh, "start: file tidak ada"); return SHELL_ERR; }
    int tid = sys_spawn(app);
    if (tid < 0) { shell_writeln(sh, "start: gagal (slot task penuh / OOM)"); return SHELL_ERR; }
    shell_write(sh, "start: "); shell_write(sh, elf);
    shell_write(sh, " (task "); shell_writenum(sh, (uint32_t)tid); shell_writeln(sh, ")");
    return SHELL_OK;
}

static int cmd_ping(shell_t* sh, int argc, char** argv) {
    if (argc < 2) {
        shell_writeln(sh, "Penggunaan: ping [host]");
        shell_writeln(sh, "Contoh  : ping 8.8.8.8 / ping google.com");
        return SHELL_ERR;
    }
    int rtt = sys_ping(argv[1]);   // detail dicetak kernel ke TTY (tak di sini)
    if (rtt < 0) shell_writeln(sh, "ping : tidak ada balasan (timeout)");
    else { shell_write(sh, "ping : "); shell_writenum(sh, (uint32_t)rtt); shell_writeln(sh, " ms"); }
    return SHELL_OK;
}

static int cmd_shutdown(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "Mematikan Kyuzen OS...");
    sys_shutdown();
    return SHELL_OK;
}

static int cmd_restart(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "Merestart Kyuzen OS...");
    sys_reboot();
    return SHELL_OK;
}

// Jalankan baris perintah dengan hak root (dipakai sudo).
static int run_elevated(shell_t* sh, const char* line) {
    uint32_t old_uid = sys_get_uid();
    sh->elevated = 1;
    if (old_uid != 0) sys_set_uid(0);
    int st = shell_execute(sh, line);
    if (old_uid != 0) sys_set_uid(old_uid);
    sh->elevated = 0;
    return st;
}

// `sudo <perintah> [arg...]` — minta password akun saat ini; benar → jalankan
// sbg root. Root sendiri tidak dimintai password.
static int cmd_sudo(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: sudo <perintah> [arg...]"); return SHELL_ERR; }

    // Susun baris perintah dalam (argv[1..]) ke pending_line.
    int k = 0;
    for (int i = 1; i < argc; i++) {
        for (const char* p = argv[i]; *p && k < SHELL_LINE_MAX - 1; p++)
            sh->pending_line[k++] = *p;
        if (i + 1 < argc && k < SHELL_LINE_MAX - 1) sh->pending_line[k++] = ' ';
    }
    sh->pending_line[k] = '\0';

    if (sys_get_uid() == 0) return run_elevated(sh, sh->pending_line);   // root: langsung

    sh->pending = PEND_SUDO;
    sh->pending_mask = 1;
    str_copy(sh->pending_prompt, SHELL_PASSWORD_PROMPT, sizeof(sh->pending_prompt));
    shell_write(sh, sh->pending_prompt);
    return SHELL_STATUS_ASK_INPUT;
}

// --- adduser (portable: pakai syscalls file generik) ---
static void num_to_str(uint32_t n, char* out) {
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char t[16]; int i = 0;
    while (n > 0 && i < 15) { t[i++] = (char)('0' + (n % 10)); n /= 10; }
    int j = 0; while (i > 0) out[j++] = t[--i]; out[j] = '\0';
}

static int cmd_adduser(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: sudo adduser [nama_user]"); return SHELL_ERR; }
    int k = 0;
    while (argv[1][k] && k < (int)sizeof(sh->pending_arg) - 1) { sh->pending_arg[k] = argv[1][k]; k++; }
    sh->pending_arg[k] = '\0';
    sh->pending = PEND_ADDUSER;
    sh->pending_mask = 1;
    str_copy(sh->pending_prompt, "Masukkan password untuk user baru: ", sizeof(sh->pending_prompt));
    shell_write(sh, sh->pending_prompt);
    return SHELL_STATUS_ASK_INPUT;
}

static void user_create(shell_t* sh, const char* name, const char* pass) {
    static char buffer[1024];
    uint32_t fsize = sys_file_size("users.sys");
    if (fsize > 1023) fsize = 1023;
    sys_read_file_to_buffer("users.sys", buffer, sizeof(buffer));
    buffer[fsize] = '\0';
    buffer[1023] = '\0';   // jamin terminator apa pun isi file

    int len = 0;
    while (len < 1023 && buffer[len]) len++;
    int lines = 0;
    for (int k = 0; k < len; k++) if (buffer[k] == '\n') lines++;
    uint32_t new_uid = 1000 + (uint32_t)(lines > 0 ? lines - 1 : 0);

    char uid_str[16]; num_to_str(new_uid, uid_str);

    // Susun "name:pass:uid\n" secara eksplisit + bounded (tanpa helper scan).
    const char* parts[6];
    parts[0] = name;  parts[1] = ":";
    parts[2] = pass;  parts[3] = ":";
    parts[4] = uid_str; parts[5] = "\n";
    int trunc = 0;
    for (int i = 0; i < 6; i++) {
        for (const char* p = parts[i]; *p; p++) {
            if (len >= 1023) { trunc = 1; break; }
            buffer[len++] = *p;
        }
        if (trunc) break;
    }
    buffer[len] = '\0';
    if (trunc) { shell_writeln(sh, "adduser: users.sys penuh"); return; }

    fs_delete("users.sys");
    sys_create_file("users.sys", buffer, (uint32_t)len);
    shell_write(sh, "Berhasil! User '"); shell_write(sh, name);
    shell_writeln(sh, "' berhasil ditambahkan.");
}

// --- format (destruktif; portable lewat fs_format) ---
static int cmd_format(shell_t* sh, int argc, char** argv) {
    (void)sh; (void)argc; (void)argv;
    fs_format();
    return SHELL_OK;
}

// --- install_app (menulis app.bin contoh) ---
static int cmd_install_app(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    char dummy_bin[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x0D, 0x00, 0x80, 0x00, 0xCD, 0x80, 0xC3,
        'H', 'a', 'l', 'o', ' ', 'd', 'a', 'r', 'i', ' ', 'B', 'I', 'N', 'A', 'R', 'Y', '!', '\n', '\0'
    };
    sys_create_file("app.bin", dummy_bin, 33);
    shell_writeln(sh, "Aplikasi app.bin berhasil di-install ke Hard Disk!");
    return SHELL_OK;
}

// --- nettest (portable; socket syscalls ada di kedua konteks) ---
static int parse_uint_(const char* s, uint32_t* out) {
    if (!s || !out) return 0;
    while (*s == ' ') s++;
    if (!*s) return 0;
    uint32_t v = 0;
    while (*s) { if (*s < '0' || *s > '9') return 0; v = v * 10 + (uint32_t)(*s - '0'); s++; }
    *out = v; return 1;
}
static int parse_ipv4_(const char* s, uint32_t* out) {
    uint32_t o[4];
    for (int i = 0; i < 4; i++) {
        uint32_t v = 0;
        if (*s < '0' || *s > '9') return 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
        if (v > 255) return 0;
        o[i] = v;
        if (i < 3) { if (*s != '.') return 0; s++; }
    }
    *out = o[0] | (o[1] << 8) | (o[2] << 16) | (o[3] << 24);
    return 1;
}
static int cmd_nettest(shell_t* sh, int argc, char** argv) {
    if (argc < 3) {
        shell_writeln(sh, "Penggunaan: nettest [ip] [port]");
        shell_writeln(sh, "Contoh  : nettest 10.0.2.2 7");
        return SHELL_ERR;
    }
    uint32_t ip_be = 0, port = 0;
    if (!parse_ipv4_(argv[1], &ip_be)) { shell_writeln(sh, "nettest: IP tidak valid (format: a.b.c.d)"); return SHELL_ERR; }
    if (!parse_uint_(argv[2], &port) || port == 0 || port > 65535) {
        shell_writeln(sh, "nettest: port tidak valid (1..65535)");
        return SHELL_ERR;
    }
    int sfd = sys_socket();
    if (sfd < 0) { shell_writeln(sh, "nettest: gagal buat socket"); return SHELL_ERR; }
    shell_writeln(sh, "nettest: connecting...");
    if (sys_connect(sfd, ip_be, (uint16_t)port) != 0) {
        shell_writeln(sh, "nettest: connect GAGAL (timeout/refused)");
        sys_sock_close(sfd);
        return SHELL_ERR;
    }
    shell_writeln(sh, "nettest: connected. Mengirim pesan...");
    const char* msg = "halo dari kyuzen\n";
    uint32_t mlen = 0; while (msg[mlen]) mlen++;
    if (sys_send(sfd, msg, mlen) < 0) {
        shell_writeln(sh, "nettest: send GAGAL");
    } else {
        char rbuf[128];
        int n = sys_recv(sfd, rbuf, sizeof(rbuf) - 1);
        if (n > 0) { rbuf[n] = '\0'; shell_write(sh, "nettest: diterima: "); shell_writeln(sh, rbuf); }
        else if (n == 0) shell_writeln(sh, "nettest: peer menutup koneksi");
        else shell_writeln(sh, "nettest: recv GAGAL/timeout");
    }
    sys_sock_close(sfd);
    shell_writeln(sh, "nettest: selesai");
    return SHELL_OK;
}

// ---------- alur input tertunda ----------
int shell_awaiting_input(shell_t* sh) { return sh && sh->pending != PEND_NONE; }
const char* shell_pending_prompt(shell_t* sh) { return sh ? sh->pending_prompt : ""; }
int shell_pending_mask(shell_t* sh) { return sh && sh->pending_mask; }

int shell_supply_input(shell_t* sh, const char* line) {
    if (!sh || sh->pending == PEND_NONE) {
        shell_writeln(sh, "input: tidak ada permintaan");
        return SHELL_ERR;
    }
    uint8_t kind = sh->pending;
    sh->pending = PEND_NONE;

    if (kind == PEND_SUDO) {
        if (!current_password_match(line)) { shell_writeln(sh, "sudo: password salah"); return SHELL_ERR; }
        return run_elevated(sh, sh->pending_line);
    }
    if (kind == PEND_ADDUSER) {
        if (!line[0]) { shell_writeln(sh, "adduser: password kosong, dibatalkan"); return SHELL_ERR; }
        user_create(sh, sh->pending_arg, line);
        return SHELL_OK;
    }
    return SHELL_ERR;
}

// --- perintah informasi / kenyamanan (portable) ---
static int cmd_logout(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "Keluar dari sesi...");
    return SHELL_STATUS_EXIT;
}

static int cmd_whoami(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    char u[32]; shell_username(u, sizeof(u));
    shell_writeln(sh, u[0] ? u : "user");
    return SHELL_OK;
}

static int cmd_id(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    char u[32]; shell_username(u, sizeof(u));
    shell_write(sh, "uid="); shell_writenum(sh, sys_get_uid());
    shell_write(sh, "("); shell_write(sh, u[0] ? u : "?"); shell_writeln(sh, ")");
    return SHELL_OK;
}

static int cmd_users(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    if (!sys_file_exists("users.sys")) { shell_writeln(sh, "users: tidak ada users.sys"); return SHELL_ERR; }
    uint32_t fsize = sys_file_size("users.sys");
    if (fsize > 1023) fsize = 1023;
    static char b[1024];
    sys_read_file_to_buffer("users.sys", b, sizeof(b));
    b[fsize] = '\0'; b[1023] = '\0';
    int i = 0;
    while (b[i]) {
        char name[32], uid[16]; int j = 0;
        while (b[i] && b[i] != ':' && j < 31) name[j++] = b[i++];
        name[j] = '\0';
        if (b[i] == ':') i++;
        while (b[i] && b[i] != ':') i++;         // lewati password
        if (b[i] == ':') i++;
        j = 0;
        while (b[i] && b[i] != '\n' && j < 15) uid[j++] = b[i++];
        uid[j] = '\0';
        if (b[i] == '\n') i++;
        if (name[0]) {
            shell_write(sh, "  "); shell_write(sh, name);
            shell_write(sh, "  (uid "); shell_write(sh, uid); shell_writeln(sh, ")");
        }
    }
    return SHELL_OK;
}

static int cmd_df(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    uint32_t tot  = sys_get_total_disk() / 1048576u;
    uint32_t used = sys_get_used_disk()  / 1048576u;
    shell_write(sh, "Total : "); shell_writenum(sh, tot);  shell_writeln(sh, " MB");
    shell_write(sh, "Used  : "); shell_writenum(sh, used); shell_writeln(sh, " MB");
    shell_write(sh, "Free  : "); shell_writenum(sh, tot - used); shell_writeln(sh, " MB");
    return SHELL_OK;
}

static int cmd_uname(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    shell_writeln(sh, "KyuzenOS x86_64");
    return SHELL_OK;
}

static int cmd_wait(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: wait [ms]"); return SHELL_ERR; }
    uint32_t ms = 0;
    if (!parse_uint_(argv[1], &ms)) { shell_writeln(sh, "wait: angka ms tidak valid"); return SHELL_ERR; }
    sys_sleep(ms);
    shell_write(sh, "wait: "); shell_writenum(sh, ms); shell_writeln(sh, " ms");
    return SHELL_OK;
}

static const shell_cmd_entry_t g_builtins[] = {
    { "help",     cmd_help,     "Info ini",                    0 },
    { "clear",    cmd_clear,    "Bersihkan layar",             0 },
    { "echo",     cmd_echo,     "Cetak teks",                  0 },
    { "ls",       cmd_ls,       "Daftar file",                 0 },
    { "baca",     cmd_baca,     "Baca isi file",               0 },
    { "hapus",    cmd_hapus,    "Hapus file",                  0 },
    { "mkdir",    cmd_mkdir,    "Buat folder",                 0 },
    { "fetch",    cmd_fetch,    "Spek OS",                     0 },
    { "neofetch", cmd_neofetch, "Spek OS + banner",            0 },
    { "sched",    cmd_sched,    "Status CPU",                  0 },
    { "time",     cmd_time,     "Waktu sekarang",              0 },
    { "start",    cmd_start,    "Jalankan app konkuren",       0 },
    { "ping",     cmd_ping,     "Ping host",                   0 },
    { "install_app", cmd_install_app, "Instal app.bin",        0 },
    { "nettest",  cmd_nettest,  "Tes TCP socket",              0 },
    { "whoami",   cmd_whoami,   "Nama user saat ini",          0 },
    { "id",       cmd_id,       "UID + nama user",             0 },
    { "users",    cmd_users,    "Daftar akun",                 0 },
    { "df",       cmd_df,       "Pemakaian disk",              0 },
    { "uname",    cmd_uname,    "Info sistem",                 0 },
    { "wait",     cmd_wait,     "Tunda N milidetik",           0 },
    { "cat",      cmd_baca,     "Alias baca",                  0 },
    { "rm",       cmd_hapus,    "Alias hapus",                 0 },
    { "date",     cmd_time,     "Alias time",                  0 },
    { "adduser",  cmd_adduser,  "Tambah user",                 1 },
    { "format",   cmd_format,   "Format disk ke KZFS",         1 },
    { "logout",   cmd_logout,   "Keluar dari sesi",            0 },
    { "sudo",     cmd_sudo,     "Jalankan perintah sebagai root", 0 },
    { "shutdown", cmd_shutdown, "Matikan OS",                  1 },
    { "restart",  cmd_restart,  "Restart OS",                  1 },
    { "reboot",   cmd_restart,  "Alias restart",               1 },
};

// ---------- engine ----------
int shell_has_command(shell_t* sh, const char* name) {
    if (!sh || !name) return 0;
    for (int i = 0; i < sh->n; i++)
        if (strcmp(sh->cmds[i].name, name) == 0) return 1;
    return 0;
}

shell_t* shell_init(const shell_io_t* io) {
    g_shell.io = io;
    g_shell.n  = 0;
    const int nb = (int)(sizeof(g_builtins) / sizeof(g_builtins[0]));
    for (int i = 0; i < nb; i++)
        shell_register_command(&g_shell, g_builtins[i].name,
                               g_builtins[i].fn, g_builtins[i].desc);
    return &g_shell;
}

int shell_execute(shell_t* sh, const char* line) {
    if (!sh || !line) return SHELL_ERR;

    char buf[SHELL_LINE_MAX];
    int i = 0;
    while (line[i] == ' ') i++;                       // buang spasi depan
    int n = 0;
    while (line[i] && n < SHELL_LINE_MAX - 1) buf[n++] = line[i++];
    buf[n] = '\0';
    while (n > 0 && buf[n-1] == ' ') buf[--n] = '\0'; // buang spasi belakang
    if (n == 0) return SHELL_OK;

    char* argv[SHELL_MAX_ARGV];
    int argc = 0;
    char* p = buf;
    while (*p && argc < SHELL_MAX_ARGV) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (argc == 0) return SHELL_OK;

    for (int c = 0; c < sh->n; c++) {
        if (strcmp(sh->cmds[c].name, argv[0]) == 0) {
            // Perintah sensitif: wajib lewat sudo (root sendiri boleh langsung).
            if (sh->cmds[c].sudo && !sh->elevated && sys_get_uid() != 0) {
                shell_error(sh, "sudo: perintah sensitif. Gunakan: sudo ");
                shell_error(sh, argv[0]);
                shell_error(sh, " ...\n");
                return SHELL_ERR;
            }
            return sh->cmds[c].fn(sh, argc, argv);
        }
    }
    shell_error(sh, "Perintah tidak dikenali: ");
    shell_error(sh, argv[0]);
    shell_error(sh, "\n");
    return SHELL_ERR;
}
