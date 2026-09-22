// ============================================================
// libs/core/userutil.c — helper identitas user bersama
//
// Satu implementasi untuk DUA konteks: kernel Ring 0 (shell via
// libs/core/kernel_userlib.c) dan user-space (terminal.elf via libs/core/userlib.c).
// Hanya memakai API generik yang tersedia di kedua konteks (sys_file_*,
// sys_get_uid), jadi tidak ada parser yang diduplikasi di shell/terminal.
// ============================================================

#include "userlib.h"

// users.sys per baris: "username:password:uid". Username maks 31 + NUL.
#define USERUTIL_NAME_MAX 32

int current_username(char* out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (!sys_file_exists("users.sys")) return 0;

    uint32_t fsize = sys_file_size("users.sys");
    if (fsize >= 1024) fsize = 1023;
    char buffer[1024];
    sys_read_file_to_buffer("users.sys", buffer, sizeof(buffer));
    buffer[fsize] = '\0';

    uint32_t want = sys_get_uid();
    uint32_t i = 0;
    while (buffer[i] != '\0') {
        char f_user[USERUTIL_NAME_MAX], f_uid[16];
        uint32_t j = 0;
        // Username
        while (buffer[i] != ':' && buffer[i] != '\0' && j < USERUTIL_NAME_MAX - 1)
            f_user[j++] = buffer[i++];
        f_user[j] = '\0';
        if (buffer[i] == ':') i++;
        // Lewati password (tanpa validasi)
        while (buffer[i] != ':' && buffer[i] != '\0') i++;
        if (buffer[i] == ':') i++;
        // UID
        j = 0;
        while (buffer[i] != '\n' && buffer[i] != '\0' && j < 15)
            f_uid[j++] = buffer[i++];
        f_uid[j] = '\0';
        if (buffer[i] == '\n') i++;

        uint32_t parsed = 0;
        for (uint32_t k = 0; f_uid[k] != '\0'; k++)
            parsed = parsed * 10 + (uint32_t)(f_uid[k] - '0');
        if (f_user[0] != '\0' && parsed == want) {
            uint32_t k = 0;
            while (f_user[k] != '\0' && k < cap - 1) { out[k] = f_user[k]; k++; }
            out[k] = '\0';
            return 1;
        }
    }
    return 0;
}

// True jika `pw` cocok dengan password akun UID saat ini (bukan login).
int current_password_match(const char* pw) {
    if (!pw) return 0;
    if (!sys_file_exists("users.sys")) return 0;

    uint32_t fsize = sys_file_size("users.sys");
    if (fsize >= 1024) fsize = 1023;
    char buffer[1024];
    sys_read_file_to_buffer("users.sys", buffer, sizeof(buffer));
    buffer[fsize] = '\0';

    uint32_t want = sys_get_uid();
    uint32_t i = 0;
    while (buffer[i] != '\0') {
        char f_pass[32], f_uid[16];
        uint32_t j = 0;
        while (buffer[i] != ':' && buffer[i] != '\0') i++;   // lewati username
        if (buffer[i] == ':') i++;
        j = 0;
        while (buffer[i] != ':' && buffer[i] != '\0' && j < 31) f_pass[j++] = buffer[i++];
        f_pass[j] = '\0';
        if (buffer[i] == ':') i++;
        j = 0;
        while (buffer[i] != '\n' && buffer[i] != '\0' && j < 15) f_uid[j++] = buffer[i++];
        f_uid[j] = '\0';
        if (buffer[i] == '\n') i++;

        uint32_t parsed = 0;
        for (uint32_t k = 0; f_uid[k] != '\0'; k++)
            parsed = parsed * 10 + (uint32_t)(f_uid[k] - '0');
        if (parsed == want && strcmp(f_pass, pw) == 0) return 1;
    }
    return 0;
}

