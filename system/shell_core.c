// ============================================================
// system/shell_core.c — Kyuzen Shell engine (shared by all frontends).
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
    shell_writeln(sh, "Redir/pipe (app eksternal): cmd > f | cmd >> f | cmd < f | a | b");
    shell_writeln(sh, "Ctrl-C menghentikan foreground pipeline yang sedang jalan.");
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

// tree [path] — tampilkan pohon direktori secara rekursif. Memakai fd
// direktori (sys_open + sys_readdir), jadi tidak perlu API khusus: langsung
// di atas filesystem tree kernel. Dua lintasan per direktori (hitung entri,
// lalu cetak) supaya bisa menandai entri terakhir tanpa menampung semuanya.
#define TREE_MAX_DEPTH 8
#define TREE_PATH_MAX  224

static int tree_is_dot(const char* nm) {
    return nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'));
}

static void tree_walk(shell_t* sh, const char* path, const char* prefix, int depth) {
    int fd = sys_open(path, 0);           // 0 = O_RDONLY
    if (fd < 0) {
        shell_write(sh, prefix); shell_write(sh, "`-- ");
        shell_writeln(sh, "<tidak bisa dibuka>");
        return;
    }

    char name[64];
    uint8_t is_dir = 0;
    int total = 0;
    for (uint32_t i = 0; sys_readdir(fd, i, name, sizeof(name), &is_dir) == 0; i++)
        if (!tree_is_dot(name)) total++;

    int seen = 0;
    for (uint32_t i = 0; sys_readdir(fd, i, name, sizeof(name), &is_dir) == 0; i++) {
        if (tree_is_dot(name)) continue;
        int last = (++seen == total);
        shell_write(sh, prefix);
        shell_write(sh, last ? "`-- " : "|-- ");
        shell_writeln(sh, name);
        if (!is_dir || depth >= TREE_MAX_DEPTH) continue;

        // Gabung path: "/" + name untuk root, "<path>/<name>" selainnya.
        int p = 0;
        char child[TREE_PATH_MAX];
        int root = (path[0] == '/' && path[1] == '\0');
        if (root) child[p++] = '/';
        else {
            while (path[p] && p < TREE_PATH_MAX - 2) { child[p] = path[p]; p++; }
            if (p > 0 && child[p - 1] != '/') child[p++] = '/';
        }
        int k = 0;
        while (name[k] && p < TREE_PATH_MAX - 1) child[p++] = name[k++];
        child[p] = '\0';
        if (name[k] != '\0') {              // path kepanjangan — jangan rekursi
            shell_write(sh, prefix);
            shell_writeln(sh, "`-- <path terlalu panjang>");
            continue;
        }

        char next[64];
        int q = 0;
        const char* cont = last ? "    " : "|   ";
        while (prefix[q] && q < 60) { next[q] = prefix[q]; q++; }
        for (int m = 0; cont[m] && q < 63; m++) next[q++] = cont[m];
        next[q] = '\0';
        tree_walk(sh, child, next, depth + 1);
    }
    sys_close(fd);
}

static int cmd_tree(shell_t* sh, int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "/";
    uint32_t size = 0;
    uint8_t is_dir = 0;
    if (sys_stat(path, &size, &is_dir) != 0) {
        shell_writeln(sh, "tree: path tidak ada");
        return SHELL_ERR;
    }
    if (!is_dir) {
        shell_writeln(sh, "tree: bukan direktori");
        return SHELL_ERR;
    }
    shell_writeln(sh, path);
    tree_walk(sh, path, "", 0);
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
    if (argc < 2) { shell_writeln(sh, "Penggunaan: start [app] [arg...]"); return SHELL_ERR; }
    char elf[32]; int i = 0;
    while (argv[1][i] && i < 27) { elf[i] = argv[1][i]; i++; }
    elf[i] = '\0';
    int has_ext = (i >= 4 && elf[i-4]=='.' && elf[i-3]=='e' && elf[i-2]=='l' && elf[i-1]=='f');
    if (!has_ext && i < 28) { elf[i++]='.'; elf[i++]='e'; elf[i++]='l'; elf[i++]='f'; elf[i]='\0'; }
    char app[40]; build_app_path(app, sizeof(app), elf);
    if (!sys_file_exists(app)) { shell_writeln(sh, "start: file tidak ada"); return SHELL_ERR; }
    // P0 Phase 2: teruskan argumen ke child argv.
    // child argc = argc-1, child argv[0] = nama app seperti diketik (argv[1]),
    // argv[1..] = argv[2..]. Tanpa argumen ekstra: argc=1 (kompatibel lama).
    int cargc = argc - 1;
    if (cargc < 1) cargc = 1;
    if (cargc > PROC_MAX_ARGC) cargc = PROC_MAX_ARGC;
    char* cargv[PROC_MAX_ARGC];
    for (int k = 0; k < cargc; k++) cargv[k] = argv[1 + k];
    int tid = sys_spawn_argv(app, cargc, cargv);
    if (tid < 0) {
        // Fallback: spawn lama tanpa argumen (kompatibilitas).
        tid = sys_spawn(app);
    }
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
// P0 Phase 1: elevasi lewat sys_set_uid butuh caller root (kernel boundary).
// Non-root yang lolos verifikasi password TETAP ditolak kernel di sini —
// sudo password-mandiri bukan mekanisme privilegel; eskalasi non-root yang
// benar (setuid/sudoers) adalah fase berikutnya. Gagal = jangan eksekusi.
static int run_elevated(shell_t* sh, const char* line) {
    uint32_t old_uid = sys_get_uid();
    if (old_uid != 0 && sys_set_uid(0) != 0) {
        shell_writeln(sh, "sudo: elevation ditolak kernel (butuh root)");
        return SHELL_ERR;
    }
    sh->elevated = 1;
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

// --- format (destruktif; portable lewat fs_format, root-only di kernel) ---
static int cmd_format(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    if (fs_format() != 0) {
        shell_writeln(sh, "format: ditolak kernel (butuh root)");
        return SHELL_ERR;
    }
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

// P0 Phase 3: daftar child milik shell ini (read-only, tanpa WNOHANG).
// Reaping eksplisit lewat `reap <pid>`; tidak ada polling latar.
static const char* job_state_(uint8_t st) {
    switch (st) {
        case 0: return "READY";
        case 1: return "RUNNING";
        case 2: return "SLEEP";
        case 4: return "BLOCK";
        case 5: return "ZOMBIE";
        default: return "?";
    }
}

static int cmd_jobs(shell_t* sh, int argc, char** argv) {
    (void)argc; (void)argv;
    proc_info_t list[16];
    int n = sys_proc_list(list, 16);
    if (n < 0) { shell_writeln(sh, "jobs: gagal"); return SHELL_ERR; }
    int32_t me = sys_getpid();
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].ppid != me) continue;
        found = 1;
        shell_write(sh, "  ["); shell_writenum(sh, (uint32_t)list[i].pid);
        shell_write(sh, "] "); shell_write(sh, job_state_(list[i].state));
        shell_write(sh, " "); shell_writeln(sh, list[i].name);
    }
    if (!found) shell_writeln(sh, "(tidak ada child)");
    return SHELL_OK;
}

// P0 Phase 3: tunggu SATU child sampai keluar lalu reap (blocking eksplisit).
// Tanpa WNOHANG tidak ada reap non-blocking; jangan polling waitpid di sini.
// P0 Phase 3: minta terminasi proses (otorisasi di kernel: child/root).
static int cmd_kill(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: kill [pid]"); return SHELL_ERR; }
    uint32_t pid = 0;
    if (!parse_uint_(argv[1], &pid)) { shell_writeln(sh, "kill: pid tidak valid"); return SHELL_ERR; }
    if (sys_kill((int)pid) != 0) {
        shell_writeln(sh, "kill: ditolak (bukan child / tak valid / sudah keluar)");
        return SHELL_ERR;
    }
    shell_write(sh, "kill: PID "); shell_writenum(sh, pid); shell_writeln(sh, " dihentikan");
    return SHELL_OK;
}

static int cmd_reap(shell_t* sh, int argc, char** argv) {
    if (argc < 2) { shell_writeln(sh, "Penggunaan: reap [pid]"); return SHELL_ERR; }
    uint32_t pid = 0;
    if (!parse_uint_(argv[1], &pid)) { shell_writeln(sh, "reap: pid tidak valid"); return SHELL_ERR; }
    int status = 0;
    int got = sys_waitpid((int)pid, &status, 0);
    if (got < 0) { shell_writeln(sh, "reap: bukan child / sudah di-reap"); return SHELL_ERR; }
    shell_write(sh, "reap: pid "); shell_writenum(sh, (uint32_t)got);
    shell_write(sh, " status ");
    if (status < 0) { shell_write(sh, "-"); shell_writenum(sh, (uint32_t)(-(int64_t)status)); }
    else shell_writenum(sh, (uint32_t)status);
    if (status == PROC_KILL_EXIT_CODE) shell_write(sh, " (killed)");
    shell_writeln(sh, "");
    return SHELL_OK;
}

static const shell_cmd_entry_t g_builtins[] = {
    { "help",     cmd_help,     "Info ini",                    0 },
    { "clear",    cmd_clear,    "Bersihkan layar",             0 },
    { "echo",     cmd_echo,     "Cetak teks",                  0 },
    { "ls",       cmd_ls,       "Daftar file",                 0 },
    { "tree",     cmd_tree,     "Pohon direktori (rekursif)",  0 },
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
    { "jobs",     cmd_jobs,     "Daftar child proses",         0 },
    { "kill",     cmd_kill,     "Hentikan proses (hak kernel)", 0 },
    { "reap",     cmd_reap,     "Tunggu+reap child (blokir)",  0 },
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

// ---------- P0 Phase 5: redirection + pipelines (app eksternal saja) ----------
// Operator HARUS token terpisah ("a | b", "cmd > f"). Tanpa quoting:
// "a>b" adalah satu kata (bukan operator). '<' hanya di stage pertama,
// '>'/'>>' hanya di stage terakhir, stderr selalu console, maks 4 stage.
// Builtin (echo/cat/start/...) jalan in-process via shell_io — tak bisa
// di-fd-redirect — jadi tiap stage HARUS /apps/*.elf (child spawn_redir
// + waitpid foreground). Tanpa '&', tanpa job control.
#define SHELL_MAX_STAGES 4
#define SHELL_MAX_WORDS  64

static int op_is_pipe(const char* w) { return w[0] == '|' && w[1] == '\0'; }
static int op_is_in(const char* w)   { return w[0] == '<' && w[1] == '\0'; }
static int op_is_out(const char* w)  { return w[0] == '>' && w[1] == '\0'; }
static int op_is_app(const char* w)  { return w[0] == '>' && w[1] == '>' && w[2] == '\0'; }
static int op_is_any(const char* w)  { return op_is_pipe(w) || op_is_in(w) || op_is_out(w) || op_is_app(w); }

// P0 Phase 6A: 1 bila frontend melaporkan Ctrl-C tertunda (dikonsumsi),
// 0 bila tidak ada / poll tak didukung. Frontend yang memutuskan arti
// "Ctrl-C tiba"; shell yang memutuskan artinya bagi foreground pipeline.
int shell_poll_ctrlc(shell_t* sh) {
    if (!sh || !sh->io || !sh->io->poll_input) return 0;
    return sh->io->poll_input(sh->io->ctx) == 1;
}

static int join_stages(shell_t* sh, char* sargv[SHELL_MAX_STAGES][SHELL_MAX_ARGV],
                       int pids[SHELL_MAX_STAGES], int nspawn);

// Resolve stage -> /apps/<nama>.elf. External-only: app eksternal menang
// atas builtin se-nama (mis. `echo` -> /apps/echo.elf agar bisa di-pipe;
// builtin echo jalan in-process dan tak bisa di-fd-redirect). Nama yang
// hanya builtin (help/start/...) ditolak; yang tak ada sama sekali ditolak.
// Return 0 sukses, -1 app tak ada, -2 hanya-builtin.
static int stage_resolve(shell_t* sh, const char* name, char app[40]) {
    if (!name || !name[0]) return -1;
    char elf[32]; int i = 0;
    while (name[i] && i < 27) { elf[i] = name[i]; i++; }
    elf[i] = '\0';
    int has_ext = (i >= 4 && elf[i-4] == '.' && elf[i-3] == 'e' && elf[i-2] == 'l' && elf[i-1] == 'f');
    if (!has_ext && i < 28) { elf[i++] = '.'; elf[i++] = 'e'; elf[i++] = 'l'; elf[i++] = 'f'; elf[i] = '\0'; }
    build_app_path(app, 40, elf);
    if (sys_file_exists(app)) return 0;
    if (shell_has_command(sh, name)) return -2;
    return -1;
}

// Tutup semua ujung parent (kunci disiplin EOF: pembaca hanya dapat EOF
// bila TAK ADA lagi referensi ujung tulis, termasuk milik parent).
static void close_parent_ends(int pipes[][2], int npipes, int fd_in, int fd_out) {
    for (int i = 0; i < npipes; i++) {
        if (pipes[i][0] >= 0) sys_close(pipes[i][0]);
        if (pipes[i][1] >= 0) sys_close(pipes[i][1]);
    }
    if (fd_in >= 0) sys_close(fd_in);
    if (fd_out >= 0) sys_close(fd_out);
}

// Fork-child half of a pipeline/redirection stage (P0 Phase 6C). Wires
// stdin/stdout with dup2, closes every pipe end + redir file (the parent
// keeps its own copies until all stages spawn — the close discipline
// that delivers EOF), then execs. Returns only if setup/exec fails:
// report on stderr (fd 2, always console) and exit 127. NEVER returns
// to the shell loop (the child is a task clone, not a shell).
static void stage_child(shell_t* sh, const char* name, const char* app,
                        int argc, char** argv,
                        int stdin_src, int stdout_dst,
                        int pipes[][2], int npipes, int fd_in, int fd_out) {
    if (stdin_src >= 0 && sys_dup2(stdin_src, 0) < 0) {
        shell_write(sh, "exec: dup2 stdin gagal: "); shell_writeln(sh, name);
        sys_exit_code(127);
    }
    if (stdout_dst >= 0 && sys_dup2(stdout_dst, 1) < 0) {
        shell_write(sh, "exec: dup2 stdout gagal: "); shell_writeln(sh, name);
        sys_exit_code(127);
    }
    close_parent_ends(pipes, npipes, fd_in, fd_out);
    sys_execve((char*)app, argc, argv);
    shell_write(sh, "exec: gagal: "); shell_writeln(sh, name);
    sys_exit_code(127);
}

static int run_stages(shell_t* sh, char* sargv[SHELL_MAX_STAGES][SHELL_MAX_ARGV],
                      int sargc[SHELL_MAX_STAGES], int nst,
                      const char* fin, const char* fout, int append) {
    char apps[SHELL_MAX_STAGES][40];
    for (int i = 0; i < nst; i++) {
        int r = stage_resolve(sh, sargv[i][0], apps[i]);
        if (r == -2) { shell_write(sh, "pipe: butuh app eksternal: "); shell_writeln(sh, sargv[i][0]); return SHELL_ERR; }
        if (r != 0)  { shell_write(sh, "pipe: app tidak ada: "); shell_writeln(sh, sargv[i][0]); return SHELL_ERR; }
    }
    int fd_in = -1, fd_out = -1;
    if (fin) {
        fd_in = sys_open(fin, O_RDONLY);
        if (fd_in < 0) { shell_write(sh, "redir: gagal buka: "); shell_writeln(sh, fin); return SHELL_ERR; }
    }
    if (fout) {
        fd_out = sys_open(fout, (uint32_t)(O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC)));
        if (fd_out < 0) {
            if (fd_in >= 0) sys_close(fd_in);
            shell_write(sh, "redir: gagal buka: "); shell_writeln(sh, fout); return SHELL_ERR;
        }
    }
    int pipes[SHELL_MAX_STAGES][2];
    int npipes = 0;
    for (int i = 0; i < nst - 1; i++) {
        pipes[i][0] = pipes[i][1] = -1;
        if (sys_pipe(pipes[i]) != 0) {
            shell_writeln(sh, "pipe: gagal buat pipe");
            close_parent_ends(pipes, npipes, fd_in, fd_out);
            return SHELL_ERR;
        }
        npipes++;
    }
    int pids[SHELL_MAX_STAGES];
    int nspawn = 0;
    for (int i = 0; i < nst; i++) {
        int stdin_src  = (i == 0) ? fd_in : pipes[i - 1][0];
        int stdout_dst = (i == nst - 1) ? fd_out : pipes[i][1];
        char* cargv[PROC_MAX_ARGC];
        for (int k = 0; k < sargc[i]; k++) cargv[k] = sargv[i][k];
        // P0 Phase 6C: fork-first. The child wires its own stdio and
        // execs (never returns to the shell); the parent table is
        // untouched. Kernel-context shells (console task 0: no user AS
        // to clone) get fork==-1 and fall back to the legacy spawn_redir
        // path with identical wiring — behavior preserved bit-for-bit.
        int pid = sys_fork();
        if (pid < 0) {
            spawn_stdio_t spec;
            spec.fd0 = (int32_t)stdin_src;
            spec.fd1 = (int32_t)stdout_dst;
            spec.fd2 = -1;   // stderr selalu console (2> tak didukung)
            pid = sys_spawn_redir(apps[i], sargc[i], cargv, &spec);
        } else if (pid == 0) {
            stage_child(sh, sargv[i][0], apps[i], sargc[i], cargv,
                        stdin_src, stdout_dst, pipes, npipes, fd_in, fd_out);
            sys_exit_code(127);   // unreachable unless the kernel breaks noreturn
        }
        if (pid < 0) {
            shell_write(sh, "pipe: gagal spawn: "); shell_writeln(sh, sargv[i][0]);
            // Kill what we started (a lone producer would block forever on
            // a pipe whose reader never spawned), close, then join remains.
            for (int j = 0; j < nspawn; j++) sys_kill(pids[j]);
            close_parent_ends(pipes, npipes, fd_in, fd_out);
            for (int j = 0; j < nspawn; j++) { int st = 0; sys_waitpid(pids[j], &st, 0); }
            return SHELL_ERR;
        }
        pids[nspawn++] = pid;
    }
    close_parent_ends(pipes, npipes, fd_in, fd_out);
    return join_stages(sh, sargv, pids, nspawn);
}

// P0 Phase 6A foreground join: multiplex child completion with Ctrl-C.
// Polls the process snapshot (children of task 0 auto-reap, so presence —
// not zombie state — is the uniform completion signal for BOTH shells),
// snapshots zombie exit codes, then reaps for authoritative statuses.
// Ctrl-C (via frontend poll_input) kills every not-yet-requested fg pid
// through sys_kill only — kernel stays authoritative; P0.3 idempotence
// makes duplicate/exit-raced kills safe. Bounded: hung children fail the
// pipeline after ~60 s, never the shell.
static int join_stages(shell_t* sh, char* sargv[SHELL_MAX_STAGES][SHELL_MAX_ARGV],
                       int pids[SHELL_MAX_STAGES], int nspawn) {
    int requested[SHELL_MAX_STAGES]; int zcode[SHELL_MAX_STAGES]; int zseen[SHELL_MAX_STAGES];
    for (int j = 0; j < nspawn; j++) { requested[j] = 0; zcode[j] = 0; zseen[j] = 0; }
    int32_t me = sys_getpid();
    int timed_out = 1;
    for (int t = 0; t < 3000; t++) {
        if (shell_poll_ctrlc(sh)) {
            shell_write(sh, "^C\n");
            for (int j = 0; j < nspawn; j++) {
                if (!requested[j]) { sys_kill(pids[j]); requested[j] = 1; }
            }
        }
        proc_info_t list[16];
        int n = sys_proc_list(list, 16);
        if (n < 0) n = 0;
        int done = 1;
        for (int j = 0; j < nspawn; j++) {
            int terminal = 1;   // absent = gone (task-0 auto-reap)
            for (int i = 0; i < n; i++) {
                // ppid==me pins OUR child against PID-slot reuse.
                if (list[i].pid == pids[j] && list[i].ppid == me) {
                    if (list[i].state != 5) terminal = 0;   // 5 = ZOMBIE
                    else { zseen[j] = 1; zcode[j] = list[i].exit_code; }
                    break;
                }
            }
            if (!terminal) { done = 0; break; }
        }
        if (done) { timed_out = 0; break; }
        sys_sleep(20);
    }
    if (timed_out) {
        shell_writeln(sh, "pipe: timeout menunggu child");
        return SHELL_ERR;
    }
    int st = SHELL_OK;
    for (int j = 0; j < nspawn; j++) {
        int status = 0;
        int got = sys_waitpid(pids[j], &status, 0);
        if (got >= 0) {
            if (status != 0) {
                st = SHELL_ERR;
                shell_write(sh, "pipe: exit tidak nol: "); shell_writeln(sh, sargv[j][0]);
            }
        } else if (zseen[j] && zcode[j] != 0) {
            st = SHELL_ERR;
            shell_write(sh, "pipe: exit tidak nol: "); shell_writeln(sh, sargv[j][0]);
        }
    }
    return st;
}

// Jalur operator: dipanggil shell_execute bila baris mengandung |<>.
// Tanpa operator persis -> delegasi ke dispatch biasa (mis. "a>b").
static int shell_run_complex(shell_t* sh, char* buf);

static int shell_run_simple(shell_t* sh, char* buf) {
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

static int shell_run_complex(shell_t* sh, char* buf) {
    char* w[SHELL_MAX_WORDS]; int nw = 0;
    char* p = buf;
    while (*p && nw < SHELL_MAX_WORDS) {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        w[nw++] = p;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    if (*p) { shell_writeln(sh, "pipe: baris terlalu panjang"); return SHELL_ERR; }

    int has_op = 0;
    for (int i = 0; i < nw; i++) if (op_is_any(w[i])) { has_op = 1; break; }
    if (!has_op) return shell_run_simple(sh, buf);

    char* sargv[SHELL_MAX_STAGES][SHELL_MAX_ARGV];
    int sargc[SHELL_MAX_STAGES]; int nst = 1; int stage = 0;
    for (int i = 0; i < SHELL_MAX_STAGES; i++) sargc[i] = 0;
    const char* fin = NULL; const char* fout = NULL; int append = 0;
    int in_stage = -1, out_stage = -1;
    for (int i = 0; i < nw; i++) {
        if (op_is_pipe(w[i])) {
            stage++;
            if (stage >= SHELL_MAX_STAGES) { shell_writeln(sh, "pipe: kebanyakan stage (maks 4)"); return SHELL_ERR; }
            nst = stage + 1;
            continue;
        }
        if (op_is_in(w[i]) || op_is_out(w[i]) || op_is_app(w[i])) {
            int is_in = op_is_in(w[i]);
            if (i + 1 >= nw || op_is_any(w[i + 1])) { shell_writeln(sh, "pipe: operator tanpa file"); return SHELL_ERR; }
            if (is_in) {
                if (fin) { shell_writeln(sh, "pipe: input ganda"); return SHELL_ERR; }
                fin = w[i + 1]; in_stage = stage;
            } else {
                if (fout) { shell_writeln(sh, "pipe: output ganda"); return SHELL_ERR; }
                fout = w[i + 1]; append = op_is_app(w[i]); out_stage = stage;
            }
            i++;
            continue;
        }
        if (sargc[stage] >= SHELL_MAX_ARGV) { shell_writeln(sh, "pipe: argumen berlebih"); return SHELL_ERR; }
        sargv[stage][sargc[stage]++] = w[i];
    }
    for (int s = 0; s < nst; s++) {
        if (sargc[s] == 0) { shell_writeln(sh, "pipe: perintah kosong"); return SHELL_ERR; }
    }
    if (fin && in_stage != 0) { shell_writeln(sh, "pipe: '<' hanya di perintah pertama"); return SHELL_ERR; }
    if (fout && out_stage != nst - 1) { shell_writeln(sh, "pipe: '>' hanya di perintah terakhir"); return SHELL_ERR; }
    return run_stages(sh, sargv, sargc, nst, fin, fout, append);
}

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

    // P0 Phase 5: baris beroperator -> jalur pipeline/redirection
    // (shell_run_complex mendelegasikan balik bila tak ada operator persis).
    for (int t = 0; buf[t]; t++) {
        if (buf[t] == '|' || buf[t] == '>' || buf[t] == '<')
            return shell_run_complex(sh, buf);
    }
    return shell_run_simple(sh, buf);
}
