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
// TERM_SERIAL_MIRROR (test build only): echo the transcript to the tty so
// the GUI terminal is observable over COM1. No-op in production builds.
static void term_mirror(const char* text) {
#ifdef TERM_SERIAL_MIRROR
    int n = slen(text);
    if (n > 0) sys_write_fd(1, text, (uint32_t)n);
#else
    (void)text;
#endif
}
static void term_out(void* ctx, const char* text) {
    (void)ctx;
    term_mirror(text);
    ui_textedit_append(out, text);
}
static void term_clear(void* ctx) {
    (void)ctx;
    term_mirror("\n[term:clear]\n");
    ui_textedit_clear(out);
}
// Semua append transkrip lewat sini agar mirror serial mencakup prompt/banner.
static void term_show(const char* text) {
    term_mirror(text);
    ui_textedit_append(out, text);
}

// P0 Phase 6A poll: pompa antrian event KWM selama join foreground.
// Ctrl+C (scancode fisik 0x2E + Ctrl, layout-independen) dikonsumsi -> 1.
// Event lain ikut termakan (terdokumentasi: tanpa type-ahead dan tanpa
// interaksi window selama pipeline berjalan; klik lagi setelahnya).
static int term_poll_input(void* ctx) {
    (void)ctx;
    kyuzen_event_t ev;
    while (sys_get_event(&ev)) {
        if (ev.type == EVENT_KEY_PRESS &&
            (ev.param2 & KEY_MOD_CTRL) &&
            (ev.param3 & 0x1FF) == 0x2Eu) {
            return 1;
        }
    }
    return 0;
}

// P0 Phase 6A: Ctrl-C tanpa foreground (loop event jalan normal) —
// shortcut menelan keypress SEBELUM masuk TextBox (tak ada huruf 'c'
// nyasar). Potong baris input saat ini, cetak ^C, prompt segar.
// Saat pipeline jalan, loop terblokir di shell_execute: shortcut tak
// menyala, event mentah dibaca term_poll_input sebagai interupsi.
static void on_ctrl_c(void* userdata) {
    (void)userdata;
    if (shell_awaiting_input(g_sh)) return;   // prompt password: telan saja
    const char* t = ui_textedit_text(out);
    int len = slen(t), ls = len;
    while (ls > 0 && t[ls - 1] != '\n') ls--;   // buang "prompt+ketikan"
    char keep[8192]; int k = 0;
    for (int i = 0; i < ls && k < 8180; i++) keep[k++] = t[i];
    const char* mark = "^C\n";
    for (int i = 0; mark[i] && k < 8180; i++) keep[k++] = mark[i];
    for (int i = 0; g_prompt[i] && k < 8180; i++) keep[k++] = g_prompt[i];
    keep[k] = '\0';
    ui_textedit_set_text(out, keep);
    term_mirror("\n^C\n");
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

    term_show("\n");   // tutup baris

    // Mode input tertunda (password sudo / password user baru): baris = prompt + input.
    if (shell_awaiting_input(g_sh)) {
        const char* pf = shell_pending_prompt(g_sh);
        int skip = 0; while (pf[skip] && raw[skip] == pf[skip]) skip++;
        int st = shell_supply_input(g_sh, raw + skip);
        if (st == SHELL_STATUS_EXIT) { ui_window_request_close(g_win); return; }
        term_show(g_prompt);
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
    if (!shell_awaiting_input(g_sh)) term_show(g_prompt);
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
    // P0 Phase 6A: Ctrl-C tanpa foreground (shift mengubah P1 -> daftarkan
    // dua-duanya, pola widget_demo). Saat pipeline jalan tak menyala.
    ui_window_add_shortcut(win, KEY_MOD_CTRL, 'c', on_ctrl_c, 0);
    ui_window_add_shortcut(win, KEY_MOD_CTRL, 'C', on_ctrl_c, 0);

    // Shell engine bersama + prompt dari akun yang login.
    shell_io_t io;
    io.out = term_out; io.err = term_out; io.clear = term_clear; io.ctx = 0;
    io.poll_input = term_poll_input;
    g_sh = shell_init(&io);
    shell_build_prompt(g_prompt, sizeof(g_prompt), "@kyuzen:~$ ");
    // Prompt berwarna (gaya shell Linux): "user@kyuzen:~$ " biru muda,
    // kontras terhadap output putih di baris yang sama.
    ui_textedit_set_prompt_style(out, g_prompt, 0x7CC7FF);

    term_show("KyuzenOS Terminal\n");
    term_show("Type 'help' for available commands.\n\n");
    term_show(g_prompt);

    ui_window_run(win);   // blocking; keluar via X titlebar / ESC
    ui_window_destroy(win);
    sys_exit();
}
