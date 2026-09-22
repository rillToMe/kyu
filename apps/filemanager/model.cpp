// apps/filemanager/model.cpp — implementasi lapisan model.
#include "model.hpp"

#include <algorithm>
#include <string_view>

namespace fm {
namespace {

bool endsWithCI(std::string_view s, std::string_view suffix) {
    if (suffix.size() > s.size()) return false;
    std::size_t off = s.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char a = s[off + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return true;
}

bool isApplication(std::string_view name) {
    return endsWithCI(name, ".elf") || endsWithCI(name, ".app");
}

// Perbandingan nama tanpa peduli besar-kecil (ASCII), stabil terhadap
// tie-break: dipakai sebagai kunci sekunder/utama.
int compareNames(const std::string& a, const std::string& b) {
    std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

}  // namespace

bool DirectoryModel::load(const std::string& path, int max_entries) {
    path_ = path;
    entries_.clear();
    truncated_ = false;
    valid_ = false;

    DirListing listing = FileSystem::readDirectory(path, max_entries);
    if (!listing.ok) return false;

    valid_ = true;
    truncated_ = listing.more;
    entries_.reserve(listing.entries.size());
    for (DirEntry& e : listing.entries) {
        FileEntry f;
        f.name = std::move(e.name);
        f.kind = e.kind;
        f.size = e.size;
        f.stat_ok = e.stat_ok;
        f.name_truncated = e.truncated_name;
        f.type_label = typeLabelFor(f.name, f.kind);
        f.icon = iconFor(f.name, f.kind);
        std::string child;
        f.path_ok = FileSystem::join(path, f.name, child);
        entries_.push_back(std::move(f));
    }
    sort(spec_);
    return true;
}

int DirectoryModel::folderCount() const {
    int n = 0;
    for (const FileEntry& e : entries_)
        if (e.kind == EntryKind::Dir) ++n;
    return n;
}

int DirectoryModel::fileCount() const {
    return (int)entries_.size() - folderCount();
}

void DirectoryModel::sort(const SortSpec& spec) {
    spec_ = spec;
    std::sort(entries_.begin(), entries_.end(), [&](const FileEntry& a, const FileEntry& b) {
        // Folder selalu di atas, apa pun kuncinya (kebijakan tampilan).
        if (a.kind != b.kind) return a.kind == EntryKind::Dir;
        int r = 0;
        switch (spec.key) {
        case SortKey::Type:
            r = compareNames(a.type_label, b.type_label);
            if (r == 0) r = compareNames(a.name, b.name);
            break;
        case SortKey::Size:
            r = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
            if (r == 0) r = compareNames(a.name, b.name);
            break;
        case SortKey::Name:
        default:
            r = compareNames(a.name, b.name);
            break;
        }
        return spec.descending ? r > 0 : r < 0;
    });
}

int DirectoryModel::indexOf(const std::string& name) const {
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].name == name) return (int)i;
    return -1;
}

void DirectoryModel::clear() {
    entries_.clear();
    valid_ = false;
    truncated_ = false;
}

// --- PathHistory ------------------------------------------------------------
void PathHistory::reset(const std::string& current) {
    nback_ = 0;
    nfwd_ = 0;
    if (!current.empty()) back_[nback_++] = current;
}

void PathHistory::record(const std::string& current) {
    if (current.empty()) return;
    if (nback_ > 0 && back_[nback_ - 1] == current) return;   // tidak dobel
    if (nback_ == kMax) {                                     // buang yang tertua
        for (int i = 1; i < kMax; ++i) back_[i - 1] = back_[i];
        nback_ = kMax - 1;
    }
    back_[nback_++] = current;
    nfwd_ = 0;      // cabang baru: riwayat maju dibuang
}

bool PathHistory::back(const std::string& current, std::string& target) {
    if (nback_ <= 0) return false;
    if (nfwd_ == kMax) {
        for (int i = 1; i < kMax; ++i) fwd_[i - 1] = fwd_[i];
        nfwd_ = kMax - 1;
    }
    fwd_[nfwd_++] = current;
    target = back_[--nback_];
    return true;
}

bool PathHistory::forward(const std::string& current, std::string& target) {
    if (nfwd_ <= 0) return false;
    // Kebalikan back(): current masuk riwayat MUNDUR tanpa menghapus riwayat
    // maju (pindah maju bukan cabang baru), target diambil dari tumpukan maju.
    if (nback_ == kMax) {
        for (int i = 1; i < kMax; ++i) back_[i - 1] = back_[i];
        nback_ = kMax - 1;
    }
    back_[nback_++] = current;
    target = fwd_[--nfwd_];
    return true;
}

// --- util -------------------------------------------------------------------
std::string parentOf(const std::string& path) {
    if (path.empty() || path == "/") return "/";
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    std::size_t slash = path.rfind('/', end ? end - 1 : 0);
    if (slash == std::string::npos || slash == 0) return "/";
    return path.substr(0, slash);
}

std::string typeLabelFor(const std::string& name, EntryKind kind) {
    if (kind == EntryKind::Dir) return "Folder";
    if (isApplication(name)) return "Application";
    switch (media_type_of(name.c_str())) {
    case MEDIA_IMAGE:    return "Image";
    case MEDIA_VIDEO:    return "Video";
    case MEDIA_AUDIO:    return "Audio";
    case MEDIA_DOCUMENT: return "Text";
    default:             return "File";
    }
}

IconKind iconFor(const std::string& name, EntryKind kind) {
    if (kind == EntryKind::Dir) return IconKind::Folder;
    if (isApplication(name)) return IconKind::App;
    switch (media_type_of(name.c_str())) {
    case MEDIA_IMAGE:    return IconKind::Image;
    case MEDIA_DOCUMENT: return IconKind::Text;
    default:             return IconKind::Generic;
    }
}

const char* describe(OpResult r) {
    switch (r) {
    case OpResult::Ok:             return "Done.";
    case OpResult::NotFound:       return "The item no longer exists.";
    case OpResult::NotADirectory:  return "This path is a file, not a folder.";
    case OpResult::IsADirectory:   return "This path is a folder, not a file.";
    case OpResult::AlreadyExists:  return "An item with this name already exists.";
    case OpResult::NotEmpty:       return "This folder is not empty.";
    case OpResult::PathTooLong:    return "The path is too long for the filesystem API.";
    case OpResult::InvalidName:    return "That name cannot be used.";
    case OpResult::Failed:
    default:                       return "The filesystem refused the operation.";
    }
}

bool samePath(const std::string& a, const std::string& b) { return a == b; }

}  // namespace fm
