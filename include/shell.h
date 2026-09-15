#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

// ============================================================
// Kyuzen Shell — engine bersama untuk SEMUA terminal/frontend.
//
// Shell memiliki: parser, command registry, built-in, identitas user
// (UID -> users.sys), dan prompt. Frontend (console TTY / GUI terminal)
// hanya menyediakan kanal output + aksi layar, dan mengirim baris perintah
// ke shell_execute(); frontend TIDAK PERNAH mengimplementasikan perintah.
//
//   add command once  →  every terminal gets it.
//
// Satu instance shell per address space (static, tanpa alokasi dinamis).
// ============================================================

typedef struct shell_s shell_t;

// Handler perintah. argc >= 1, argv[0] = nama perintah. argv[1..] = argumen.
// Return 0 = sukses, non-zero = error (lihat SHELL_*).
typedef int (*shell_cmd_fn)(shell_t* sh, int argc, char** argv);

// Kanal output frontend. `err` boleh NULL (fallback ke `out`); `clear` boleh
// NULL (perintah `clear` jadi no-op). ctx diteruskan apa adanya.
// `poll_input` (boleh NULL = tak didukung): dipanggil shell saat menunggu
// foreground pipeline; kembalikan 1 bila Ctrl-C tiba (frontend mengonsumsi
// eventnya), 0 bila tidak ada. Hanya untuk interupsi — bukan kanal data.
typedef struct {
    void (*out)(void* ctx, const char* text);
    void (*err)(void* ctx, const char* text);
    void (*clear)(void* ctx);
    int (*poll_input)(void* ctx);
    void* ctx;
} shell_io_t;

#define SHELL_OK          0
#define SHELL_ERR         1
#define SHELL_STATUS_EXIT 2   // frontend diminta keluar (mis. perintah `logout`)
#define SHELL_STATUS_ASK_INPUT 3   // shell minta satu baris input (sudo/adduser)
#define SHELL_STATUS_ASK_PASSWORD SHELL_STATUS_ASK_INPUT  // alias lama

#define SHELL_MAX_CMDS    48

// Teks yang dicetak shell saat meminta password (tanpa newline). Frontend
// berbaris (GUI) memakainya untuk memotong baris input dari transkrip.
#define SHELL_PASSWORD_PROMPT "Password: "

// Buat/init shell global (mendaftarkan built-in). Return pointer instance.
shell_t* shell_init(const shell_io_t* io);

// Daftarkan perintah (dipakai shell_init + frontend utk perintah eksklusifnya).
// Return 0 sukses, -1 penuh/duplikat.
int shell_register_command(shell_t* sh, const char* name, shell_cmd_fn fn,
                           const char* desc);

// Tandai perintah sebagai sensitif: hanya boleh lewat `sudo <perintah>`.
// Return 0 sukses, -1 perintah tak ditemukan.
int shell_require_sudo(shell_t* sh, const char* name);

// --- Alur input tertunda (deferred), aman utk semua frontend ---
// Perintah (sudo/adduser) menulis prompt lalu return SHELL_STATUS_ASK_INPUT.
// Frontend membaca satu baris (masked utk password) dan memanggil
// shell_supply_input(); shell melanjutkan perintah tertunda.
int shell_awaiting_input(shell_t* sh);                 // 1 jika menunggu input
const char* shell_pending_prompt(shell_t* sh);         // prefix yg dicetak shell
int shell_pending_mask(shell_t* sh);                   // 1 = sembunyikan (password)
int shell_supply_input(shell_t* sh, const char* line); // lanjutkan perintah

// Parse & jalankan satu baris perintah. Return status (SHELL_*).
int shell_execute(shell_t* sh, const char* line);

// 1 jika `name` adalah perintah terdaftar (dipakai frontend console untuk
// fallback "implicit exec" <nama>.elf pada perintah tak dikenal).
int shell_has_command(shell_t* sh, const char* name);

// P0 Phase 6A: 1 bila frontend melaporkan Ctrl-C tertunda (dikonsumsi),
// 0 bila tidak ada / poll tak didukung. Dipakai join foreground pipeline.
int shell_poll_ctrlc(shell_t* sh);

// --- Output helpers (dipakai command handler) ---
void shell_write(shell_t* sh, const char* text);
void shell_writeln(shell_t* sh, const char* text);   // text + "\n"
void shell_error(shell_t* sh, const char* text);
void shell_writenum(shell_t* sh, uint32_t n);

// --- Identitas / prompt ---
// Username akun dengan UID saat ini (via users.sys). 1 sukses, 0 gagal.
int shell_username(char* out, uint32_t cap);
// Bangun prompt: "<username><suffix>" (mis. suffix "@kyuzen> " / "@kyuzen:~$ ").
void shell_build_prompt(char* out, uint32_t cap, const char* suffix);
uint32_t shell_uid(shell_t* sh);

#endif
