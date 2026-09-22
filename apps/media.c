// apps/media.c — abstraksi media bersama user-space (lihat include/media.h).
//
// Library bersama pola apps/png.c: dikompilasi SEKALI (-O2) dan di-link oleh
// app yang membutuhkannya (Gallery, ImageView, kelak VideoPlayer/AudioPlayer).
// Isinya murni utilitas: tabel ekstensi, path, pemindaian direktori, format
// ukuran, probe header gambar. TIDAK ada state global yang bisa tumbuh, tidak
// ada alokasi dinamis selain yang dipakai lewat image_decode() (apps/png.c).
//
// Batas modul ini sengaja kecil dan datar: menambah format media = satu baris
// di k_media_ext[] (+ dekoder bila sudah ada). Tidak ada registry/plugin.
#include "userlib.h"
#include "media.h"

// ------------------------------------------------------------
// Helper teks lokal
// ------------------------------------------------------------
// Hanya yang dibutuhkan modul ini; user-space KyuzenOS tidak menautkan
// libc (memcpy/memset weak dari apps/userlib.c tetap dipakai, disediakan
// compiler). Semua static → tidak ada simbol tambahan yang bocor ke app.
static int m_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static char m_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int m_eq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    int i = 0;
    for (; a[i] && b[i]; i++)
        if (m_lower(a[i]) != m_lower(b[i])) return 0;
    return a[i] == b[i];
}

static void m_copy(char* dst, int cap, const char* src) {
    if (cap <= 0) return;
    int i = 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// ------------------------------------------------------------
// Tabel ekstensi → tipe media (SATU sumber kebenaran)
// ------------------------------------------------------------
// `decodable` = ada implementasi nyata hari ini. Jangan pernah mengiklankan
// format yang belum ada dekodernya: .jpg/.jpeg adalah keluarga gambar, tapi
// belum ada dekoder JPEG di KyuzenOS (stb JPEG butuh FP; build -msoft-float
// -mno-sse) → media_is_supported_image() menolaknya dan UI menampilkan
// "unsupported format", bukan mencoba dan gagal misterius.
typedef struct {
    const char* ext;        // lowercase, dengan titik
    uint8_t     type;       // media_type_t
    uint8_t     decodable;  // 1 = handler/dekoder terpasang
} media_ext_t;

static const media_ext_t k_media_ext[] = {
    { ".png",  MEDIA_IMAGE,    1 },
    { ".bmp",  MEDIA_IMAGE,    1 },
    { ".jpg",  MEDIA_IMAGE,    0 },
    { ".jpeg", MEDIA_IMAGE,    0 },
    { ".gif",  MEDIA_IMAGE,    0 },
    { ".mp4",  MEDIA_VIDEO,    0 },
    { ".webm", MEDIA_VIDEO,    0 },
    { ".mp3",  MEDIA_AUDIO,    0 },
    { ".wav",  MEDIA_AUDIO,    0 },
    { ".txt",  MEDIA_DOCUMENT, 1 },   // handler: notepad (teks polos)
};

#define MEDIA_EXT_COUNT (int)(sizeof(k_media_ext) / sizeof(k_media_ext[0]))

static const media_ext_t* m_find_ext(const char* path) {
    if (!path) return 0;
    const char* dot = 0;
    for (const char* p = path; *p; p++) {
        if (*p == '/') dot = 0;           // ekstensi hanya di komponen terakhir
        else if (*p == '.') dot = p;
    }
    if (!dot || !dot[1]) return 0;
    for (int i = 0; i < MEDIA_EXT_COUNT; i++)
        if (m_eq_ci(dot, k_media_ext[i].ext)) return &k_media_ext[i];
    return 0;
}

// ------------------------------------------------------------
// Deteksi tipe
// ------------------------------------------------------------
media_type_t media_type_of(const char* path) {
    const media_ext_t* e = m_find_ext(path);
    return e ? (media_type_t)e->type : MEDIA_UNKNOWN;
}

int media_is_image(const char* path) {
    return media_type_of(path) == MEDIA_IMAGE;
}

int media_is_supported_image(const char* path) {
    const media_ext_t* e = m_find_ext(path);
    return (e && e->type == MEDIA_IMAGE && e->decodable) ? 1 : 0;
}

const char* media_type_name(media_type_t t) {
    switch (t) {
    case MEDIA_FOLDER:   return "Folder";
    case MEDIA_IMAGE:    return "Image";
    case MEDIA_VIDEO:    return "Video";
    case MEDIA_AUDIO:    return "Audio";
    case MEDIA_DOCUMENT: return "Document";
    default:             return "Unknown";
    }
}

// ------------------------------------------------------------
// Path
// ------------------------------------------------------------
const char* media_basename(const char* path) {
    if (!path) return "";
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') base = p + 1;
    return base;
}

// Extension lowercase. Buffer static (satu instance, tidak reentrant) —
// pola yang sama dipakai app_icons.cpp untuk nilai kembalian pointer statis.
const char* media_extension(const char* path) {
    static char buf[8];
    const char* base = media_basename(path);
    const char* dot = 0;
    for (const char* p = base; *p; p++)
        if (*p == '.') dot = p;
    if (!dot || !dot[1]) { buf[0] = '\0'; return buf; }
    int i = 0;
    for (; dot[i] && i < (int)sizeof(buf) - 1; i++) buf[i] = m_lower(dot[i]);
    buf[i] = '\0';
    return buf;
}

void media_path_join(char* out, int cap, const char* dir, const char* name) {
    if (cap <= 0) return;
    int n = 0;
    if (!dir || !dir[0] || (dir[0] == '/' && dir[1] == '\0')) {
        out[n++] = '/';                       // akar: "/name", bukan "//name"
    } else {
        int d = m_len(dir);
        while (d > 1 && dir[d - 1] == '/') d--;   // buang separator ganda di ujung
        for (int i = 0; i < d && n < cap - 2; i++) out[n++] = dir[i];
        if (n == 0 || out[n - 1] != '/') out[n++] = '/';
    }
    if (name) for (int i = 0; name[i] && n < cap - 1; i++) out[n++] = name[i];
    out[n] = '\0';
}

void media_dirname(char* out, int cap, const char* path) {
    if (cap <= 0) return;
    if (!path || !path[0]) { m_copy(out, cap, "/"); return; }
    int last = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/') last = i;
    if (last <= 0) { m_copy(out, cap, "/"); return; }   // "/x.png" atau "x.png" → akar
    int n = last;
    if (n >= cap) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = path[i];
    out[n] = '\0';
}

// ------------------------------------------------------------
// Ukuran berkas human-readable (gaya Explorer: "512 B", "12.3 KB", "2.4 MB")
// Semua integer (app dibangun -msoft-float): desimal lewat fixed-point.
// ------------------------------------------------------------
static int put_u32(char* b, int i, uint32_t v) {
    char t[12];
    int k = 0;
    if (v == 0) t[k++] = '0';
    while (v > 0 && k < 11) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k > 0) b[i++] = t[--k];
    return i;
}

void media_format_size(uint32_t bytes, char* out, int cap) {
    if (cap <= 0) return;
    char b[32];
    int i = 0;
    if (bytes < 1024u) {
        i = put_u32(b, i, bytes);
        b[i++] = ' '; b[i++] = 'B';
    } else {
        uint32_t whole, tenth;
        const char* unit;
        if (bytes < 1024u * 1024u) {
            whole = bytes >> 10;
            tenth = ((bytes & 1023u) * 10u) >> 10;      // < 10240, aman 32-bit
            unit = "KB";
        } else if (bytes < 1024u * 1024u * 1024u) {
            whole = bytes >> 20;
            tenth = ((bytes & 0xFFFFFu) * 10u) >> 20;   // < 10485760, aman 32-bit
            unit = "MB";
        } else {
            whole = bytes >> 30;
            tenth = 0;
            unit = "GB";
        }
        i = put_u32(b, i, whole);
        b[i++] = '.'; b[i++] = (char)('0' + tenth);
        // Spasi sebelum satuan: konsisten dengan cabang byte di atas ("512 B")
        // dan dengan format yang didokumentasikan ("2.4 MB", gaya Explorer).
        b[i++] = ' ';
        for (int u = 0; unit[u]; u++) b[i++] = unit[u];
    }
    b[i] = '\0';
    m_copy(out, cap, b);
}

int media_file_stat(const char* path, uint32_t* size) {
    if (!path || !path[0]) return 0;
    if (!sys_file_exists((char*)path)) return 0;
    if (size) *size = sys_file_size((char*)path);
    return 1;
}

int media_format_int(char* out, int cap, int value) {
    if (cap <= 0) return 0;
    char t[12];
    int k = 0;
    unsigned v;
    if (value < 0) v = (unsigned)(-(value + 1)) + 1u;   // aman untuk INT_MIN
    else v = (unsigned)value;
    if (v == 0) t[k++] = '0';
    while (v > 0 && k < 11) { t[k++] = (char)('0' + (v % 10u)); v /= 10u; }
    int n = 0;
    if (value < 0 && n < cap - 1) out[n++] = '-';
    while (k > 0 && n < cap - 1) out[n++] = t[--k];
    out[n] = '\0';
    return n;
}

void media_format_info(const char* name, int width, int height,
                       const char* fmt, uint32_t size, char* out, int cap) {
    if (cap <= 0) return;
    char b[96];
    int i = 0;
    if (name && name[0]) {
        for (int k = 0; name[k] && i < (int)sizeof(b) - 24; k++) b[i++] = name[k];
        b[i++] = ' '; b[i++] = ' '; b[i++] = ' ';
    }
    if (width > 0 && height > 0) {
        i = put_u32(b, i, (uint32_t)width);
        b[i++] = 'x';
        i = put_u32(b, i, (uint32_t)height);
        b[i++] = ' '; b[i++] = ' '; b[i++] = ' ';
    }
    if (fmt && fmt[0]) {
        for (int k = 0; fmt[k] && i < (int)sizeof(b) - 24; k++) b[i++] = fmt[k];
        b[i++] = ' '; b[i++] = ' '; b[i++] = ' ';
    }
    if (size > 0) {
        char sz[24];
        media_format_size(size, sz, sizeof(sz));
        for (int k = 0; sz[k] && i < (int)sizeof(b) - 2; k++) b[i++] = sz[k];
    }
    while (i > 0 && b[i - 1] == ' ') i--;      // buang spasi ekor
    b[i] = '\0';
    m_copy(out, cap, b);
}

// ------------------------------------------------------------
// Probe header gambar (tanpa decode penuh)
// ------------------------------------------------------------
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t le32(const uint8_t* p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}
static uint32_t le16(const uint8_t* p) {
    return ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}
static int sig_png(const uint8_t* h) {
    return h[0] == 0x89 && h[1] == 'P' && h[2] == 'N' && h[3] == 'G' &&
           h[4] == 0x0D && h[5] == 0x0A && h[6] == 0x1A && h[7] == 0x0A;
}

int media_probe_image(const char* path, int* w, int* h, const char** fmt) {
    if (!path) return 0;
    uint8_t hdr[64];
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = sys_read_fd(fd, hdr, (uint32_t)sizeof(hdr));
    sys_close(fd);
    if (n < 26) return 0;

    // PNG: signature + chunk IHDR (lebar/tinggi big-endian di offset 16/20).
    if (sig_png(hdr)) {
        if (hdr[12] != 'I' || hdr[13] != 'H' || hdr[14] != 'D' || hdr[15] != 'R')
            return 0;
        if (w) *w = (int)be32(hdr + 16);
        if (h) *h = (int)be32(hdr + 20);
        if (fmt) *fmt = "PNG";
        return 1;
    }

    // BMP: "BM" + DIB header. BITMAPINFOHEADER/BITMAPV4/V5 (>=40) = int32 LE;
    // BITMAPCOREHEADER (12) = int16. Tinggi negatif = baris top-down.
    if (hdr[0] == 'B' && hdr[1] == 'M') {
        uint32_t hdr_size = le32(hdr + 14);
        int iw = 0, ih = 0;
        if (hdr_size >= 40 && n >= 26) {
            iw = (int)le32(hdr + 18);
            ih = (int)le32(hdr + 22);
            if (ih < 0) ih = -ih;
        } else if (hdr_size == 12 && n >= 22) {
            iw = (int)le16(hdr + 18);
            ih = (int)le16(hdr + 20);
        } else {
            return 0;   // header DIB tak dikenal
        }
        if (iw <= 0 || ih <= 0) return 0;
        if (w) *w = iw;
        if (h) *h = ih;
        if (fmt) *fmt = "BMP";
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------
// Pemindaian direktori
// ------------------------------------------------------------
static int m_less(const media_entry_t* a, const media_entry_t* b) {
    // Folder selalu di atas, lalu nama menaik tanpa membedakan besar-kecil
    // (ASCII), tie-break case-sensitive agar urutan selalu sama.
    int af = (a->type == MEDIA_FOLDER) ? 1 : 0;
    int bf = (b->type == MEDIA_FOLDER) ? 1 : 0;
    if (af != bf) return af > bf;
    int i = 0;
    for (; a->name[i] && b->name[i]; i++) {
        char ca = m_lower(a->name[i]), cb = m_lower(b->name[i]);
        if (ca != cb) return ca < cb;
    }
    if (a->name[i] != b->name[i]) return a->name[i] == '\0';
    return m_len(a->name) < m_len(b->name);
}

int media_scan(const char* dir, media_entry_t* out, int max, media_type_t only) {
    if (!out || max <= 0) return 0;
    if (max > MEDIA_MAX_ENTRIES) max = MEDIA_MAX_ENTRIES;   // batas buffer di bawah
    const char* root = (dir && dir[0]) ? dir : "/";

    file_info_t raw[MEDIA_MAX_ENTRIES];
    int total = sys_get_file_list((char*)root, raw, max);
    if (total < 0) return -1;

    int n = 0;
    for (int i = 0; i < total && n < max; i++) {
        const char* nm = raw[i].filename;
        if (!nm[0]) continue;
        media_type_t t = raw[i].is_folder ? MEDIA_FOLDER : media_type_of(nm);
        if (t == MEDIA_UNKNOWN) continue;
        if (only != MEDIA_UNKNOWN && t != only) continue;
        m_copy(out[n].name, MEDIA_NAME_MAX, nm);
        media_path_join(out[n].path, MEDIA_PATH_MAX, root, nm);
        out[n].size = raw[i].size;
        out[n].type = (uint8_t)t;
        out[n].supported = (t == MEDIA_FOLDER) ? 0
                         : (t == MEDIA_IMAGE  ? (uint8_t)media_is_supported_image(nm)
                                              : (uint8_t)1);
        n++;
    }

    // Insertion sort — daftar kecil (<= MEDIA_MAX_ENTRIES) dan sudah nyaris
    // terurut dari filesystem, jadi murah. Tanpa qsort: app GUI tidak menautkan
    // libc (qsort libc hanya ada di jalur SDK).
    for (int i = 1; i < n; i++) {
        media_entry_t key = out[i];
        int j = i - 1;
        while (j >= 0 && m_less(&key, &out[j])) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}
