// apps/filemanager/fs.cpp — implementasi adapter filesystem (lihat fs.hpp).
#include "fs.hpp"

namespace fm {
namespace {

// Satu komponen nama yang aman (tanpa '/', bukan "", bukan "." / "..").
bool badComponent(const std::string& n) {
    if (n.empty()) return true;
    if (n == "." || n == "..") return true;
    for (char c : n)
        if (c == '/') return true;
    return false;
}

// Path yang sudah pasti muat untuk syscall (-1 = ABI tak bisa menerimanya).
bool fits(const std::string& p) {
    return !p.empty() && (int)p.size() < kPathMax;
}

}  // namespace

DirListing FileSystem::readDirectory(const std::string& dir, int max) {
    DirListing out;
    int fd = sys_open(dir.c_str(), O_RDONLY);
    if (fd < 0) return out;                    // ok = false
    out.ok = true;

    std::string name;
    name.resize(kNameMax + 1);
    for (std::uint32_t idx = 0;; ++idx) {
        std::uint8_t is_dir = 0;
        name[0] = '\0';
        if (sys_readdir(fd, idx, &name[0], (std::uint32_t)name.size(), &is_dir) != 0)
            break;                             // habis (atau fd bukan direktori)
        std::size_t len = 0;
        while (len < name.size() && name[len] != '\0') ++len;
        if (len == 0) break;
        if (len == 1 && name[0] == '.') continue;
        if (len == 2 && name[0] == '.' && name[1] == '.') continue;

        if ((int)out.entries.size() >= max) { out.more = true; break; }

        DirEntry e;
        // Nama = `len` byte PERTAMA (bukan kNameMax): substr(0, kNameMax) pada
        // buffer 97 byte yang di-resize() menghasilkan string 96 byte berisi NUL
        // di ekor — c_str() tampak benar, tapi size() salah dan setiap path anak
        // jadi >64 byte sehingga ABI menolaknya.
        e.truncated_name = len > (std::size_t)kNameMax;
        e.name = name.substr(0, len < (std::size_t)kNameMax ? len : (std::size_t)kNameMax);
        e.kind = is_dir ? EntryKind::Dir : EntryKind::File;

        std::string full;
        if (join(dir, e.name, full)) {
            std::uint32_t size = 0;
            std::uint8_t isdir = 0;
            if (sys_stat(full.c_str(), &size, &isdir) == 0) {
                e.stat_ok = true;
                e.kind = isdir ? EntryKind::Dir : EntryKind::File;
                e.size = isdir ? 0 : size;
            }
        }
        out.entries.push_back(std::move(e));
    }
    sys_close(fd);
    return out;
}

int FileSystem::probe(const std::string& path) {
    if (!fits(path)) return -1;
    std::uint32_t size = 0;
    std::uint8_t is_dir = 0;
    if (sys_stat(path.c_str(), &size, &is_dir) != 0) return -1;
    return is_dir ? 1 : 0;
}

bool FileSystem::exists(const std::string& path) { return probe(path) >= 0; }
bool FileSystem::isDirectory(const std::string& path) { return probe(path) == 1; }
bool FileSystem::isFile(const std::string& path) { return probe(path) == 0; }

OpResult FileSystem::makeDirectory(const std::string& path) {
    if (!fits(path)) return OpResult::PathTooLong;
    if (exists(path)) return OpResult::AlreadyExists;
    return sys_mkdir(const_cast<char*>(path.c_str())) == 1 ? OpResult::Ok
                                                           : OpResult::Failed;
}

OpResult FileSystem::createFile(const std::string& path) {
    if (!fits(path)) return OpResult::PathTooLong;
    if (exists(path)) return OpResult::AlreadyExists;
    return sys_create_file(const_cast<char*>(path.c_str()), const_cast<char*>(""), 0) == 1
               ? OpResult::Ok
               : OpResult::Failed;
}

OpResult FileSystem::rename(const std::string& from, const std::string& to) {
    if (!fits(from) || !fits(to)) return OpResult::PathTooLong;
    if (exists(to)) return OpResult::AlreadyExists;
    if (!exists(from)) return OpResult::NotFound;
    return sys_rename(from.c_str(), to.c_str()) == 0 ? OpResult::Ok
                                                     : OpResult::Failed;
}

OpResult FileSystem::remove(const std::string& path) {
    if (!fits(path)) return OpResult::PathTooLong;
    int kind = probe(path);
    if (kind < 0) return OpResult::NotFound;
    if (kind == 1 && !isEmptyDirectory(path)) return OpResult::NotEmpty;
    return fs_delete(const_cast<char*>(path.c_str())) == 0 ? OpResult::Ok
                                                          : OpResult::Failed;
}

bool FileSystem::isEmptyDirectory(const std::string& path) {
    int fd = sys_open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    std::string name;
    name.resize(kNameMax + 1);
    bool empty = true;
    for (std::uint32_t idx = 0;; ++idx) {
        std::uint8_t is_dir = 0;
        name[0] = '\0';
        if (sys_readdir(fd, idx, &name[0], (std::uint32_t)name.size(), &is_dir) != 0)
            break;
        std::size_t len = 0;
        while (len < name.size() && name[len] != '\0') ++len;
        if (len == 0) break;
        if (len == 1 && name[0] == '.') continue;
        if (len == 2 && name[0] == '.' && name[1] == '.') continue;
        empty = false;
        break;
    }
    sys_close(fd);
    return empty;
}

bool FileSystem::join(const std::string& dir, const std::string& name, std::string& out) {
    std::string p = dir;
    if (p.empty()) p = "/";
    if (p.back() != '/') p.push_back('/');
    p += name;
    if ((int)p.size() >= kPathMax) {
        out.clear();
        return false;
    }
    out.swap(p);
    return true;
}

bool FileSystem::validName(const std::string& name) {
    return !badComponent(name) && (int)name.size() < kNameMax;
}

std::string FileSystem::uniqueName(const std::string& dir, const std::string& base,
                                   const char* suffix) {
    std::string sfx = suffix ? suffix : "";
    for (int i = 1; i < 100; ++i) {
        std::string cand = base;
        if (i > 1) {
            cand += " (";
            char num[8];
            int n = 0;
            int v = i;
            char rev[8];
            while (v > 0) { rev[n++] = (char)('0' + (v % 10)); v /= 10; }
            for (int k = 0; k < n; ++k) num[k] = rev[n - 1 - k];
            num[n] = '\0';
            cand += num;
            cand += ")";
        }
        cand += sfx;
        std::string full;
        if (!join(dir, cand, full)) continue;
        if (!exists(full)) return cand;
    }
    return base;
}

}  // namespace fm
