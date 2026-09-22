// user_apps/filemanager/fs.hpp — adapter filesystem.
//
// SATU-SATUNYA tempat aplikasi ini memanggil syscall filesystem
// (sys_open/sys_readdir/sys_stat/sys_mkdir/sys_create_file/sys_rename/fs_delete).
// Lapisan UI/state tidak tahu apa-apa soal syscall: mereka memakai tipe hasil di
// sini. Ini juga tempat batas ABI ditegakkan (path > 63 byte ditolak di depan,
// bukan dipotong diam-diam oleh kernel — memotong path bisa mengubah berkas
// yang SALAH).
//
// ABI yang dipakai (docs: include/userlib.h §"filesystem tree"):
//   sys_open(dir, O_RDONLY) -> fd        sys_readdir(fd, index, name, cap, &is_dir)
//   sys_stat(path, &size, &is_dir)       sys_mkdir(path) / sys_create_file(path, data, size)
//   sys_rename(old, new)                 fs_delete(path)  (folder harus kosong)
// Tidak ada inode/dirent/KyuzenFS internal yang disentuh dari sini.
#ifndef FM_FS_HPP
#define FM_FS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "platform.hpp"

namespace fm {

// Path dibatasi 64 byte TERMASUK NUL: itu batas ABI syscall (UC_MAX_FNAME di
// include/usercopy.h). KyuzenFS sendiri mengizinkan nama 255 byte dan kedalaman
// direktori jauh lebih besar, jadi nama panjang tetap terdaftar — hanya operasi
// (stat/rename/delete/open) yang ditolak dengan alasan yang jelas.
constexpr int kPathMax = 64;
// Batas nama entri untuk tampilan/enumerasi (KyuzenFS: 255). Nama yang lebih
// panjang dipotong HANYA untuk ditampilkan, dan entri itu ditandai agar tidak
// pernah dipakai sebagai target operasi.
constexpr int kNameMax = 96;

enum class EntryKind : std::uint8_t { Dir, File };

struct DirEntry {
    std::string name;      // nama penuh dari readdir (bukan versi terpotong)
    EntryKind   kind  = EntryKind::File;
    std::uint32_t size = 0;  // 0 untuk direktori
    bool  stat_ok = false;   // false = tidak bisa di-stat (mis. path terlalu panjang)
    bool  truncated_name = false;   // nama lebih panjang dari kNameMax
};

// Hasil enumerasi satu direktori. `more` = masih ada entri yang tidak ikut
// dikumpulkan (batas `max`), supaya UI bisa mengatakannya apa adanya.
struct DirListing {
    bool ok = false;         // enumerasi berhasil (fd terbuka + readdir jalan)
    bool more = false;
    std::vector<DirEntry> entries;
};

enum class OpResult : std::uint8_t {
    Ok,
    NotFound,
    NotADirectory,
    IsADirectory,
    AlreadyExists,
    NotEmpty,
    PathTooLong,
    InvalidName,
    Failed,        // filesystem menolak tanpa alasan spesifik
};

class FileSystem {
public:
    // Baca isi `dir` (tanpa "." / ".."), maksimal `max` entri, tiap entri
    // di-stat supaya kind+size berasal dari filesystem, bukan dari ekstensi.
    static DirListing readDirectory(const std::string& dir, int max);

    // -1 tidak ada, 0 berkas, 1 direktori (dua panggilan stat yang jelas).
    static int probe(const std::string& path);
    static bool isDirectory(const std::string& path);
    static bool isFile(const std::string& path);
    static bool exists(const std::string& path);

    // Operasi tulis. Semua validasi nama ada di sini supaya UI tidak pernah
    // memanggil syscall dengan path yang tidak masuk akal.
    static OpResult makeDirectory(const std::string& path);
    static OpResult createFile(const std::string& path);
    static OpResult rename(const std::string& from, const std::string& to);
    // Hapus berkas ATAU folder KOSONG. Folder tidak kosong → NotEmpty: KyuzenFS
    // tidak punya recursive delete, dan lapisan ini tidak menambahkannya
    // (perilaku filesystem dipertahankan apa adanya).
    static OpResult remove(const std::string& path);

    // Direktori kosong (selain "." dan "..")? Dipakai untuk pesan error yang
    // tepat sebelum remove() — ABI delete hanya bilang -1 untuk "kosong/tidak".
    static bool isEmptyDirectory(const std::string& path);

    // Path anak "<dir>/<name>" dengan batas ABI ditegakkan. false = terlalu
    // panjang (pemanggil menampilkan alasan, bukan memotong).
    static bool join(const std::string& dir, const std::string& name, std::string& out);
    // Nama valid untuk komponen tunggal (tanpa '/' / kosong / nama titik).
    static bool validName(const std::string& name);

    // Nama unik "New Folder", "New Folder (2)", ... di dalam `dir`.
    static std::string uniqueName(const std::string& dir, const std::string& base,
                                  const char* suffix);
};

}  // namespace fm

#endif // FM_FS_HPP
