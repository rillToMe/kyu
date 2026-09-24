#ifndef KYUZEN_VERSION_H
#define KYUZEN_VERSION_H

// ============================================================
// KyuzenOS — versi kanonis (single source of truth).
//
// Dipakai banner boot kernel (kernel/kernel.c) dan Settings → About
// (apps/settings/about.cpp). Jangan duplikasi angka versi di tempat lain:
// ubah di sini, semua konsumen ikut.
//
// Cocok C maupun C++ (hanya konstanta preprocessor).
// ============================================================

#define KYUZEN_VERSION_MAJOR 0
#define KYUZEN_VERSION_MINOR 3
#define KYUZEN_VERSION_PATCH 1

#define KYUZEN_VERSION "0.3.1"

#endif // KYUZEN_VERSION_H
