// libs/widget/src/core/text.cpp — storage + helper text provider.
//
// SATU definisi global (bukan tabel per-window): font UI adalah satu keputusan
// per proses, lihat penjelasan di core/text_provider.hpp. Tanpa objek global
// dengan konstruktor non-trivial (aturan toolkit: ELF loader tidak menjalankan
// .init_array), jadi penyimpanannya pointer POD yang di-nol-kan secara statis.
#include "core/text_provider.hpp"

namespace ui {

namespace {
// Pointer POD; nol = belum dipasang (toolkit memakai bitmap 8x16).
const TextProvider* g_provider = 0;
}  // namespace

void text_provider_set(const TextProvider* p) {
    // Provider tidak lengkap diperlakukan sebagai "tidak ada": lebih baik
    // kembali ke bitmap daripada menggambar separuh (mis. mengukur dengan
    // font nyata tapi menggambar dengan bitmap = tata letak rusak).
    if (p && p->measure && p->line_height && p->draw) g_provider = p;
    else g_provider = 0;
}

const TextProvider* text_provider_get() { return g_provider; }

bool text_provider_active() { return g_provider != 0; }

} // namespace ui
