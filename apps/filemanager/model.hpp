// apps/filemanager/model.hpp — lapisan model (TANPA UI, TANPA syscall).
//
// Isi model: entri direktori + urutan tampilan + riwayat navigasi. Semuanya
// logika murni di atas tipe dari fs.hpp, jadi bisa diuji di host tanpa kartu
// grafis. UI (app.cpp) hanya MEMBACA model dan menggambar; tidak ada keputusan
// urutan/tipe berkas yang dibuat di lapisan UI.
#ifndef FM_MODEL_HPP
#define FM_MODEL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "fs.hpp"

namespace fm {

// Ikon yang dipakai UI. Pemetaan berkas → ikon adalah kebijakan app dan
// diletakkan di model supaya List dan Icon view tidak punya tabel sendiri.
enum class IconKind : std::uint8_t { Folder, Image, Text, App, Generic };

enum class ViewMode : std::uint8_t { List, Icon };
enum class SortKey : std::uint8_t { Name, Type, Size };

struct SortSpec {
    SortKey key = SortKey::Name;
    bool descending = false;
};

// Satu entri siap-tampil: hasil fs::DirEntry + metadatanya untuk UI.
struct FileEntry {
    std::string name;
    EntryKind   kind = EntryKind::File;
    std::uint32_t size = 0;
    bool  stat_ok = false;
    bool  path_ok = false;        // path anak muat untuk ABI syscall
    bool  name_truncated = false; // nama dipotong utk tampilan (bukan target op)
    std::string type_label;       // "Folder" / "Image" / "Text" / ...
    IconKind icon = IconKind::Generic;
};

// Direktori + isinya pada satu titik waktu (snapshot). UI memakai snapshot ini
// sebagai satu-satunya sumber kebenaran listing; handle fd tidak disimpan
// (kadaluarsa setelah refresh).
class DirectoryModel {
public:
    // Muat ulang isi `path`. Return false bila direktori tidak bisa dibaca
    // (model dikosongkan dan `path` tetap diset supaya UI bisa melaporkannya).
    bool load(const std::string& path, int max_entries);

    const std::string& path() const { return path_; }
    const std::vector<FileEntry>& entries() const { return entries_; }
    bool truncated() const { return truncated_; }        // masih ada entri lain
    bool valid() const { return valid_; }                // enumerasi berhasil
    int  folderCount() const;
    int  fileCount() const;

    // Urutkan sesuai `spec` (folder selalu di atas; nama sebagai tie-break).
    void sort(const SortSpec& spec);
    const SortSpec& sortSpec() const { return spec_; }

    // Index entri bernama `name`, -1 bila tidak ada.
    int indexOf(const std::string& name) const;

    void clear();

private:
    std::string path_;
    std::vector<FileEntry> entries_;
    SortSpec spec_{};
    bool valid_ = false;
    bool truncated_ = false;
};

// Riwayat Back/Forward. Aturan yang dijaga: navigasi baru SETELAH mundur
// membersihkan seluruh riwayat maju (tidak ada cabang ganda).
class PathHistory {
public:
    static constexpr int kMax = 16;

    void reset(const std::string& current);
    // Catat `current` sebagai titik kembali sebelum pindah ke tempat lain.
    void record(const std::string& current);
    // false = tidak ada yang bisa dimundurkan/dimajukan.
    bool back(const std::string& current, std::string& target);
    bool forward(const std::string& current, std::string& target);
    bool canGoBack() const { return nback_ > 0; }
    bool canGoForward() const { return nfwd_ > 0; }

private:
    std::string back_[kMax];
    std::string fwd_[kMax];
    int nback_ = 0;
    int nfwd_ = 0;
};

// --- util path/tampilan (tipis, memakai media.h untuk yang sudah ada) --------
// Path tanpa komponen terakhir; akar → "/".
std::string parentOf(const std::string& path);
// Label tipe dari nama: "Folder", "Image", "Text", "Application", "File".
std::string typeLabelFor(const std::string& name, EntryKind kind);
// Ikon untuk entri (kebijakan app; satu tempat untuk kedua mode tampilan).
IconKind iconFor(const std::string& name, EntryKind kind);
// Pesan manusia untuk hasil operasi filesystem.
const char* describe(OpResult r);
bool samePath(const std::string& a, const std::string& b);

}  // namespace fm

#endif // FM_MODEL_HPP
