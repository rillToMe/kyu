// apps/taskmgr/format.hpp — helper formatting kecil (header-only).
//
// std::to_string TIDAK tersedia di subset libc++ Kyuzen (butuh FP/SSE yang
// dimatikan — lihat apps/settings/system.cpp). Satu-satunya formatting adalah
// append integer + operator+ std::string, dipakai semua page/model.
#ifndef TASKMGR_FORMAT_HPP
#define TASKMGR_FORMAT_HPP

#include <cstdint>
#include <string>

namespace taskmgr {

// Tambah representasi desimal `v` ke `s` (tanpa alokasi selain growth string).
inline void appendUlong(std::string& s, unsigned long v) {
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

inline std::string toStr(unsigned long v) {
    std::string s;
    appendUlong(s, v);
    return s;
}

inline std::string toStr32(std::int32_t v) {
    if (v < 0) return std::string("-") + toStr((unsigned long)(-(long)v));
    return toStr((unsigned long)v);
}

// "145 MB" dari byte (integer saja, tanpa FP).
inline std::string mbStr(std::uint64_t bytes) {
    return toStr((unsigned long)(bytes / 1024 / 1024)) + " MB";
}

}  // namespace taskmgr

#endif // TASKMGR_FORMAT_HPP
