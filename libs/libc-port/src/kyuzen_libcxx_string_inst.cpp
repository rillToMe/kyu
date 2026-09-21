// ============================================================
// KyuzenOS — explicit instantiation slice untuk basic_string<char>.
//
// KONTEKS: <string> hulu memakai extern-template declarations, sehingga
// app TIDAK mengemisikan member non-inline (compare/append/dll) dan
// mengharapkannya dari library. Rumah hulu-nya (libcxx/src/string.cpp)
// TIDAK DAPAT dikompilasi dengan flag SDK: stof/stod/stold mengembalikan
// float/double/long-double dan clang menolak ("SSE register return with
// SSE disabled") — lihat audit §18.
//
// ISI FILE INI: SATU BARIS instantiation eksplisit. Badan-badan fungsi
// adalah template header LLVM VERBATIM (bukan implementasi ulang Kyuzen).
// File ini hanya memaksa emisi member non-inline yang ditahan extern
// template — slicing TU, bukan logika baru. Tidak menyentuh FP sama sekali
// (stof/stod/stold tetap tak tersedia → stub abort di shims).
//
// Bila C SDK kelak mendukung FP dan string.cpp hulu bisa dikompilasi,
// hapus file ini (dan aturannya di Makefile) — simbol dari string.o hulu
// menggantikannya tanpa perubahan app.
// ============================================================
#include <string>

template class std::basic_string<char>;
