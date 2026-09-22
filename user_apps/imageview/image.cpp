// user_apps/imageview/image.cpp — implementasi dokumen gambar + konteks folder.
#include "image.hpp"

namespace iv {

namespace {

// Bandingkan nama dengan batas MEDIA_NAME_MAX: nama hasil listing filesystem
// sudah dipotong oleh ABI (file_info_t.filename[24]), jadi perbandingan penuh
// akan gagal untuk nama panjang. Terpotong sama = dianggap sama.
bool name_eq(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i] && i < MEDIA_NAME_MAX - 1; i++)
        if (a[i] != b[i]) return false;
    return true;
}

void copy(char* dst, int cap, const char* src) {
    if (cap <= 0) return;
    int i = 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

}  // namespace

ImageDocument::ImageDocument() {
    path[0] = '\0';
    dir[0] = '\0';
    name[0] = '\0';
    format[0] = '\0';
    width = height = 0;
    size = 0;
    state = IMG_MISSING;
}

bool ImageDocument::open(const char* p) {
    path[0] = dir[0] = name[0] = format[0] = '\0';
    width = height = 0;
    size = 0;
    state = IMG_MISSING;
    if (!p || !p[0]) return false;

    copy(path, MEDIA_PATH_MAX, p);
    copy(name, MEDIA_NAME_MAX, media_basename(path));
    media_dirname(dir, MEDIA_PATH_MAX, path);
    // 1. Ada berkasnya? (ukuran juga dipakai di status bar)
    if (!media_file_stat(path, &size)) { size = 0; state = IMG_MISSING; return false; }

    // 2. Formatnya dikenal dan ada dekodernya? Terpisah dari "berkas rusak"
    //    supaya pesan error jujur: .jpg bukan "corrupt", tapi "belum didukung".
    const media_type_t t = media_type_of(path);
    if (t != MEDIA_IMAGE) { state = IMG_UNSUPPORTED; return false; }
    if (!media_is_supported_image(path)) { state = IMG_UNSUPPORTED; return false; }

    // 3. Probe header (dimensi + format) tanpa decode penuh.
    const char* fmt = 0;
    int w = 0, h = 0;
    if (!media_probe_image(path, &w, &h, &fmt)) { state = IMG_UNDECODABLE; return false; }
    width = w;
    height = h;
    copy(format, sizeof(format), fmt ? fmt : "");
    state = IMG_OK;
    return true;
}

void ImageDocument::mark_decode_failed() {
    if (state == IMG_OK) state = IMG_UNDECODABLE;
}

const char* ImageDocument::error_title() const {
    return "Unable to open image";
}

const char* ImageDocument::error_text() const {
    switch (state) {
    case IMG_MISSING:
        // Bedakan "tidak ada argumen" dari "berkas tidak ada" — keduanya
        // menghasilkan pesan yang berbeda supaya pemakaian CLI jelas.
        return path[0] ? "The file could not be found."
                       : "No image was specified. Use: imageview <file.png>";
    case IMG_UNSUPPORTED:
        return "This file format is not supported yet. "
               "Supported formats: PNG, BMP.";
    case IMG_UNDECODABLE:
        return "The file could not be decoded. It may be corrupt or too "
               "large for the available memory.";
    default:
        return "";
    }
}

// ------------------------------------------------------------
// ImageFolder
// ------------------------------------------------------------
ImageFolder::ImageFolder() : count(0), index(-1) {}

void ImageFolder::scan_for(const char* p) {
    count = 0;
    index = -1;
    if (!p || !p[0]) return;

    char d[MEDIA_PATH_MAX];
    media_dirname(d, MEDIA_PATH_MAX, p);
    media_entry_t all[MEDIA_MAX_ENTRIES];
    const int n = media_scan(d, all, MEDIA_MAX_ENTRIES, MEDIA_IMAGE);
    if (n <= 0) return;

    const char* want = media_basename(p);
    for (int i = 0; i < n && count < MEDIA_MAX_ENTRIES; i++) {
        if (!all[i].supported) continue;      // jangan pernah mendarat di .jpg
        entries[count] = all[i];
        if (name_eq(all[i].name, want)) index = count;
        count++;
    }
}

int ImageFolder::next_index() const {
    if (count <= 0) return -1;
    if (index < 0) return 0;
    return (index + 1) % count;
}

int ImageFolder::prev_index() const {
    if (count <= 0) return -1;
    if (index < 0) return 0;
    return (index + count - 1) % count;
}

const char* ImageFolder::path_at(int i) const {
    if (i < 0 || i >= count) return 0;
    return entries[i].path;
}

}  // namespace iv
