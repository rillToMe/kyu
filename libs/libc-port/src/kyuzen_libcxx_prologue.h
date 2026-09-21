// ============================================================
// KyuzenOS — prologue force-include untuk TU libc++ Phase 6.
//
// Mendeklarasikan fungsi C di luar subset (strtof/strtod/strtold) yang
// DIREFERENSIKAN libcxx/src/string.cpp (as_float_helper untuk stof/stod)
// tetapi tidak dideklarasikan header C SDK (parsing FP ditunda —
// implementasi FP butuh SSE/tabel strtofloat).
//
// Hanya DEKLARASI (tanpa codegen — aman di -mno-sse); definisi = stub
// abort() di kyuzen_libcxx_shims.cpp. Dipaksa via -include HANYA untuk
// kompilasi TU libc++ (LIBCXXRT_FLAGS), bukan untuk app. Bila C library
// kelak menyediakan yang asli, hapus file ini + shims.
// ============================================================
#pragma once

// Dependensi antar-TU hulu yang tak dinyatakan eksplisit oleh file .cpp-nya:
// algorithm.cpp memakai ranges::less tanpa meng-include <functional>
// (di build hulu lolos via unity/include berantai; di sini eksplisit).
#include <functional>

#ifdef __cplusplus
extern "C" {
#endif

float strtof(const char *__s, char **__end);
double strtod(const char *__s, char **__end);
long double strtold(const char *__s, char **__end);

#ifdef __cplusplus
}
#endif
