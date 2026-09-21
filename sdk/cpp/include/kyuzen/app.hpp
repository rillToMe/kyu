// Kyuzen C++ SDK — antarmuka entry aplikasi (Phase 7).
//
// CRT Phase 5 tetap otoritatif: `_start` memanggil `main` ber-linkage C
// dengan `RDI=argc`, `RSI=argv`, dan `RSP%16==0`. Header ini tidak membuat
// `_start` kedua dan tidak mengubah startup; ia hanya memberi tipe dan
// helper dispatch kecil agar badan aplikasi dapat diuji/dipanggil eksplisit.

#pragma once

#include <kyuzen/config.hpp>

namespace kyuzen {

// Bentuk entry yang diterima CRT.
using app_main = int(int, char**);

// Panggil badan aplikasi yang sudah berbentuk entry CRT. Penjaga null di
// sini menggantikan boilerplate yang seharusnya tidak ditulis tiap app.
inline int run(app_main* app, int argc, char** argv) {
  if (app == nullptr)
    return 1;
  return app(argc, argv);
}

} // namespace kyuzen
