// apps/filemanager/icons.hpp — cache ikon (modul kecil, pola gallery/thumbs.*).
//
// Tugas: untuk setiap IconKind, dekode aset PNG yang sudah ada di akar
// KyuzenFS SEKALI, skalakan ke dua ukuran (baris tabel 16px, kisi 72px), lalu
// simpan pikselnya. UI hanya mem-blit pointer yang sudah siap — tidak ada decode
// per frame, dan TIDAK ada thumbnail berkas gambar (itu milik Gallery: File
// Manager hanya menampilkan ikon tipe).
//
// Kepemilikan: std::vector milik cache (RAII — tanpa new/delete manual dan
// tanpa sys_alloc yang bisa bocor). Toolkit menyimpan pointer ikon NON-OWNING,
// jadi umur cache harus lebih panjang dari window: cache adalah anggota
// FileManagerApp, sementara window dihancurkan di dalam run().
#ifndef FM_ICONS_HPP
#define FM_ICONS_HPP

#include <cstdint>
#include <vector>

#include "model.hpp"

namespace fm {

class IconCache {
public:
    static constexpr int kKinds = 5;     // banyaknya IconKind
    static constexpr int kRowPx = 16;    // ikon baris (Table::ICON_PX)
    static constexpr int kGridPx = 72;   // kotak thumbnail kisi

    IconCache() = default;
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    // Dekode semua aset. Aset yang gagal di-decode memakai ikon generik; kalau
    // generik pun gagal, sel digambar tanpa ikon (bukan crash).
    void load();

    // Pointer piksel ARGB8888 milik cache (nullptr = tidak ada ikon).
    const std::uint32_t* row(IconKind k) const;
    const std::uint32_t* grid(IconKind k) const;

private:
    std::vector<std::uint32_t> buf_[kKinds][2];   // [jenis][0=grid, 1=baris]
    void loadOne(IconKind k, const char* asset);
};

}  // namespace fm

#endif // FM_ICONS_HPP
