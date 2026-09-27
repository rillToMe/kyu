#pragma once
// Engine string helpers — std::to_string TIDAK ada di subset libc++ Kyuzen
// (lihat apps/settings/system.cpp). Satu-satunya formatting numerik engine.
#include <string>

namespace browser {
namespace detail {

inline void append_ulong(std::string& s, unsigned long v) {
    char t[24];
    int i = 0;
    if (v == 0) {
        s += '0';
        return;
    }
    while (v > 0 && i < 23) {
        t[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) s += t[--i];
}

// operator+(const char*, string) TIDAK terinstansiasi di subset libc++ Kyuzen
// (hanya basic_string<char> members; lihat kyuzen_libcxx_string_inst.cpp).
// Helper ini = satu-satunya cara menggabung literal di depan string.
inline std::string lit_plus(const char* a, const std::string& b) {
    std::string s = a ? a : "";
    s += b;
    return s;
}

}  // namespace detail
}  // namespace browser
