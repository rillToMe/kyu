// Kyuzen C++ SDK — compile-time configuration (Phase 7).
//
// Header publik ringan: hanya konstruksi bahasa inti, tanpa header libc++
// maupun header C. Versi mengikuti fase SDK yang mempublikasikannya.

#pragma once

#if !defined(__cplusplus)
#error "<kyuzen/config.hpp> membutuhkan kompilator C++"
#endif

#if __cplusplus < 201703L
#error "Kyuzen C++ SDK membutuhkan -std=c++17 atau lebih baru"
#endif

namespace kyuzen {

inline constexpr int sdk_major = 7;
inline constexpr int sdk_minor = 0;
inline constexpr long sdk_language_version = __cplusplus;

} // namespace kyuzen
