// user_apps/terminal.c — Terminal (Phase 10): console window (libui).
//
// FRONTEND saja. Semua perintah/parsing/identitas dimiliki shell engine
// bersama (apps/shell_core.c, API di include/shell.h). Terminal hanya:
// window + transcript TextEdit + input Enter → shell_execute() → output.
// Menambah perintah TIDAK perlu menyentuh file ini.
//
// Build: terminal.o + userlib.o + userutil.o + shell_core.o + libgui.o + libui.o

#include "userlib.h"
#include "shell.h"
#include "libui.h"

static ui_widget_t* out;    // transcript (TextEdit editable: header+prompt+output)
static ui_window_t* g_win;  // untuk menutup window saat `logout`
static shell_t* g_sh;
static char g_prompt[64];   // "<username>@kyuzen:~$ "

static int slen(const char* s) { int n = 0; while (s[n]) n++; return n; }

// ---------- output adapter: shell → TextEdit ----------
static void term_out(void* ctx, const char* text) {
    (void)ctx;
    ui_textedit_append(out, text);
}
static void term_clear(void* ctx) {
    (void)ctx;
    ui_textedit_clear(out);
}

static void on_enter(void* userdata) {
    (void)userdata;
    // Baris terakhir transkrip (setelah '\n' terakhir).
    const char* t = ui_textedit_text(out);
    int len = slen(t);
    int ls = len;
    while (ls > 0 && t[ls - 1] != '\n') ls--;
    char raw[256]; int rn = 0;
    for (int i = ls; i < len && rn < 255; i++) raw[rn++] = t[i];
    raw[rn] = '\0';

    ui_textedit_append(out, "\n");   // tutup baris

    // Mode input tertunda (password sudo / password user baru): baris = prompt + input.
    if (shell_awaiting_input(g_sh)) {
        const char* pf = shell_pending_prompt(g_sh);
        int skip = 0; while (pf[skip] && raw[skip] == pf[skip]) skip++;
        int st = shell_supply_input(g_sh, raw + skip);
        if (st == SHELL_STATUS_EXIT) { ui_window_request_close(g_win); return; }
        ui_textedit_append(out, g_prompt);
        return;
    }

    // Mode perintah: buang prompt yang sudah dicetak → teks ketikan user.
    int n = 0;
    int p = slen(g_prompt);
    if (p > rn) p = rn;
    char line[256];
    for (int i = p; i < rn && n < 255; i++) line[n++] = raw[i];
    line[n] = '\0';

    // `logout`: tutup window terminal (kembali ke desktop).
    if (line[0] && shell_execute(g_sh, line) == SHELL_STATUS_EXIT) {
        ui_window_request_close(g_win);
        return;
    }
    // Jika shell minta input, ia sudah mencetak promptnya — jangan tambahkan
    // prompt; user mengetik di baris yang sama.
    if (!shell_awaiting_input(g_sh)) ui_textedit_append(out, g_prompt);
}

void main(void) {
    ui_window_t* win = ui_window_create(720, 480);
    if (!win) { sys_exit(); }
    g_win = win;
    ui_window_set_title(win, "Terminal");

    // Tema terminal: latar hitam, teks terang, accent biru muda (prompt/caret).
    ui_theme_t theme;
    theme.bg           = 0x0B0D10;
    theme.fg           = 0xD7DCE2;
    theme.accent       = 0x7CC7FF;
    theme.button_bg    = 0x0B0D10;
    theme.button_fg    = 0xD7DCE2;
    theme.button_hover = 0x16191D;
    ui_window_set_theme(win, &theme);

    // Satu permukaan transcript: prompt adalah baris terakhir, Enter = submit.
    out = ui_textedit_create(win, 704, 464);
    ui_textedit_set_enter(out, on_enter, 0);
    ui_window_add(win, out);
    ui_window_focus(win, out);           // langsung bisa mengetik

    // Shell engine bersama + prompt dari akun yang login.
    shell_io_t io;
    io.out = term_out; io.err = term_out; io.clear = term_clear; io.ctx = 0;
    g_sh = shell_init(&io);
    shell_build_prompt(g_prompt, sizeof(g_prompt), "@kyuzen:~$ ");

    ui_textedit_append(out, "KyuzenOS Terminal\n");
    ui_textedit_append(out, "Type 'help' for available commands.\n\n");
    ui_textedit_append(out, g_prompt);

    ui_window_run(win);   // blocking; keluar via X titlebar / ESC
    ui_window_destroy(win);
    sys_exit();
}
