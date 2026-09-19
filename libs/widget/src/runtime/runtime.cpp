// libs/widget/src/runtime/runtime.cpp — dipindah apa adanya dari apps/libui.cpp.
#include "runtime/memory.hpp"

// ------------------------------------------------------------
// Runtime shim: C++ memori -> syscalls KyuzenOS
// ------------------------------------------------------------
void  _ui_free(void* p)     { if (p) sys_free(p); }
void* _ui_alloc(unsigned n) { return sys_alloc(n); }

int _ui_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
int _ui_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

// Copy string — toolkit OWNS salinannya (caller boleh pakai stack buffer).
char* _ui_strdup(const char* s) {
    int n = _ui_strlen(s) + 1;
    char* d = (char*)sys_alloc(n);
    if (!d) return 0;
    for (int i = 0; i < n; i++) d[i] = s[i];
    return d;
}

// C++ operator new/delete (global scope, bukan namespace) -> sys_alloc/sys_free.
// Ukuran memakai __SIZE_TYPE__ (bukan `unsigned long`) agar deklarasi ini tetap
// cocok saat file ini dikompilasi untuk HOST test (MinGW/LLP64: size_t =
// unsigned long long) maupun untuk kernel/app bare-metal.
void* operator new(__SIZE_TYPE__ n)              { return sys_alloc((uint32_t)n); }
void* operator new[](__SIZE_TYPE__ n)            { return sys_alloc((uint32_t)n); }
void  operator delete(void* p) noexcept          { if (p) sys_free(p); }
void  operator delete[](void* p) noexcept        { if (p) sys_free(p); }
void  operator delete(void* p, __SIZE_TYPE__) noexcept   { if (p) sys_free(p); }
void  operator delete[](void* p, __SIZE_TYPE__) noexcept { if (p) sys_free(p); }

// Dipanggil kalau vtable class abstrak terpanggil (bug) — jangan kembali.
extern "C" void __cxa_pure_virtual() { for (;;) {} }
