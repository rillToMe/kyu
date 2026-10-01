#ifndef BOOT_CONSOLE_H
#define BOOT_CONSOLE_H

#include <stdint.h>

// ============================================================
// Boot console (kernel/boot_console.c).
//
// Presentasi status boot di konsol kernel: satu baris per tahap, dengan
// marker berwarna "[  OK  ]" / "[ FAIL ]" / "[ WARN ]" / "[ INFO ]".
// Diagnostik verbose tetap ke serial (lihat kprint_quiet di kprint.h).
//
// Dipisah dari kernel/kernel.c supaya urutan boot (kernel_main) terbaca
// sebagai urutan tahap, bukan tenggelam di helper formatting.
// ============================================================

// Warna marker status (teks setelahnya selalu netral).
#define BOOT_C_OK    0x7DDB8A   // hijau
#define BOOT_C_FAIL  0xFF6B6B   // merah
#define BOOT_C_WARN  0xFFCC66   // kuning/amber
#define BOOT_C_INFO  0x7CC7FF   // biru muda (informasional)
#define BOOT_C_TEXT  0xFFFFFF   // teks netral

// Cetak marker berwarna tanpa newline, lalu kembalikan fg ke netral.
void boot_marker(const char* state);

// Satu baris status: "[  OK  ] Label detail\n". `detail` boleh NULL/kosong.
void boot_state(const char* state, const char* label, const char* detail);

// Satu baris status dengan nilai numerik: "[  OK  ] Label <v><suffix>\n".
void boot_state_num(const char* state, const char* label, uint64_t v,
                    const char* suffix);

// uint64 → desimal. `out` minimal 21 byte (20 digit + NUL).
void boot_u64_str(uint64_t num, char* out);

// IPv4 network byte order → "a.b.c.d". `out` minimal 16 byte.
void boot_ip_str(uint32_t a, char* out);

#endif // BOOT_CONSOLE_H
