// kernel/boot_console.c — presentasi status boot (marker berwarna + label).
//
// Dipisah dari kernel/kernel.c: helper di sini murni formatting/presentasi
// dan tidak tahu apa-apa soal urutan boot. kernel_main memanggilnya per tahap.
//
// Pola pemakaian:
//     boot_state("  OK  ", "Memory", "512 MB");
//     boot_state_num("  OK  ", "SMP", 4, " CPUs");
//
// Marker diwarnai sesuai state; teks label selalu netral. State dikenali dari
// huruf pertama setelah spasi: O/F/W/I (OK/FAIL/WARN/INFO). State lain
// (mis. "SKIP") dicetak netral.

#include "boot_console.h"
#include "kprint.h"
#include "tty.h"
#include "serial.h"

// Cetak marker "[  OK  ]" dengan warna sesuai state, lalu kembalikan fg netral.
void boot_marker(const char* state) {
    int i = 0; while (state[i] == ' ') i++;
    uint32_t col = BOOT_C_TEXT;
    switch (state[i]) {
    case 'O': col = BOOT_C_OK;   break;
    case 'F': col = BOOT_C_FAIL; break;
    case 'W': col = BOOT_C_WARN; break;
    case 'I': col = BOOT_C_INFO; break;
    default: break;             // SKIP/dll → netral
    }
    tty_set_fg(col);
    kprint("["); kprint(state); kprint("] ");
    tty_set_fg(BOOT_C_TEXT);
}

// Satu baris status: "[  OK  ] Label" atau "[  OK  ] Label detail"
// (satu spasi biasa; tanpa kolom detail fixed-width). Marker berwarna.
void boot_state(const char* state, const char* label, const char* detail) {
    boot_marker(state);
    kprint(label);
    if (detail && detail[0]) { kprint(" "); kprint(detail); }
    kprint("\n");
}

void boot_state_num(const char* state, const char* label, uint64_t v,
                    const char* suffix) {
    boot_marker(state);
    kprint(label);
    kprint(" ");
    kprint_num(v);
    if (suffix) kprint(suffix);
    kprint("\n");
}

void boot_u64_str(uint64_t num, char* out) {
    if (num == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[24]; int i = 0;
    while (num > 0 && i < 23) { tmp[i++] = (char)('0' + (num % 10)); num /= 10; }
    int j = 0; while (i > 0) out[j++] = tmp[--i]; out[j] = '\0';
}

// IPv4 network byte order → "a.b.c.d" ke buffer (pola sama dengan net_init.c).
void boot_ip_str(uint32_t a, char* out) {
    int q = 0;
    for (int sh = 0; sh <= 24; sh += 8) {
        char nb[4]; boot_u64_str((uint64_t)((a >> sh) & 0xFF), nb);
        for (int j = 0; nb[j]; j++) out[q++] = nb[j];
        if (sh < 24) out[q++] = '.';
    }
    out[q] = '\0';
}
