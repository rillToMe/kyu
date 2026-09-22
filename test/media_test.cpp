// test/media_test.cpp — uji host-side untuk library media bersama + cache
// thumbnail Gallery.
//
// Pola yang sama dengan test-pipe/test-desktop: kode PRODUKSI asli
// (apps/media.c dan user_apps/gallery/thumbs.cpp) dijalankan di atas syscall
// mock — bukan salinan logika. Yang diuji:
//   - klasifikasi tipe (satu tabel, termasuk format "keluarga gambar" yang
//     BELUM bisa didekode → tidak boleh diiklankan sebagai supported),
//   - util path (basename/extension/join/dirname),
//   - format ukuran + format integer + satu baris metadata,
//   - media_scan: filter tipe, path gabungan, urutan (folder dulu, lalu nama),
//   - media_probe_image: parse header PNG (IHDR) & BMP tanpa decode penuh,
//   - media_fit_box + media_scale_rgba (downscale box premultiplied),
//   - ThumbCache: hit tanpa decode ulang, LRU, batas memori, evict callback
//     (aplikasi diberi tahu), dan tidak ada retry setelah decode gagal.
//
// Jalankan: make test-media   (compiler HOST, bukan target bare-metal)
// CATATAN flag build: header Kyuzen di-resolve lewat -iquote (bukan -I), supaya
// `#include <stdlib.h>`/`<string.h>` tetap menunjuk header HOST — include/ punya
// stdlib.h/string.h versi Kyuzen (mode kernel/user-space) yang menaungi keduanya.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// platform.hpp = satu-satunya pintu header C user-space (linkage C), lalu
// thumbs.hpp = cache thumbnail produksi yang diuji.
#include "gallery/platform.hpp"
#include "gallery/thumbs.hpp"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (strcmp((actual), (expected)) != 0) {                             \
            printf("  FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,   \
                   (actual), (expected));                                    \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

// ============================================================
// Mock filesystem + syscall user-space
// ============================================================
namespace {

struct MockFile {
    const char* path;
    const uint8_t* data;
    uint32_t size;
};

MockFile g_files[8];
int g_nfiles = 0;
int g_fd_off[8];      // offset baca per "fd" (fd = index+1)

void mock_add_file(const char* path, const uint8_t* data, uint32_t size) {
    g_files[g_nfiles].path = path;
    g_files[g_nfiles].data = data;
    g_files[g_nfiles].size = size;
    g_nfiles++;
}

int mock_find(const char* path) {
    for (int i = 0; i < g_nfiles; i++)
        if (strcmp(g_files[i].path, path) == 0) return i;
    return -1;
}

struct MockDir { const char* path; file_info_t* entries; int count; };
MockDir g_dirs[4];
int g_ndirs = 0;

void mock_add_dir(const char* path, file_info_t* entries, int count) {
    g_dirs[g_ndirs].path = path;
    g_dirs[g_ndirs].entries = entries;
    g_dirs[g_ndirs].count = count;
    g_ndirs++;
}

}  // namespace

// --- syscall mock (linkage C: dipanggil dari apps/media.c) ---
extern "C" int sys_file_exists(char* filename) {
    return mock_find(filename) >= 0 ? 1 : 0;
}

extern "C" uint32_t sys_file_size(char* filename) {
    const int i = mock_find(filename);
    return i >= 0 ? g_files[i].size : 0;
}

extern "C" int sys_open(const char* path, uint32_t flags) {
    (void)flags;
    const int i = mock_find(path);
    if (i < 0) return -1;
    g_fd_off[i] = 0;
    return i + 1;
}

extern "C" int sys_read_fd(int fd, void* buf, uint32_t count) {
    const int i = fd - 1;
    if (i < 0 || i >= g_nfiles) return -1;
    uint32_t avail = g_files[i].size - (uint32_t)g_fd_off[i];
    uint32_t n = count < avail ? count : avail;
    memcpy(buf, g_files[i].data + g_fd_off[i], n);
    g_fd_off[i] += (int)n;
    return (int)n;
}

extern "C" int sys_close(int fd) { (void)fd; return 0; }

extern "C" int sys_get_file_list(char* path, file_info_t* buffer, int max) {
    for (int i = 0; i < g_ndirs; i++) {
        if (strcmp(g_dirs[i].path, path) != 0) continue;
        int n = g_dirs[i].count < max ? g_dirs[i].count : max;
        for (int k = 0; k < n; k++) buffer[k] = g_dirs[i].entries[k];
        return n;
    }
    return -1;   // direktori tidak ada
}

// --- dekoder gambar mock (ThumbCache memanggil image_decode/image_free) ---
namespace {
struct DecodeStat {
    char path[64];
    int calls;
};
DecodeStat g_dec[16];
int g_ndec = 0;

DecodeStat* dec_slot(const char* path) {
    for (int i = 0; i < g_ndec; i++)
        if (strcmp(g_dec[i].path, path) == 0) return &g_dec[i];
    DecodeStat* d = &g_dec[g_ndec++];
    int n = 0;
    for (; path[n] && n < 63; n++) d->path[n] = path[n];
    d->path[n] = '\0';
    d->calls = 0;
    return d;
}

int dec_calls(const char* path) {
    for (int i = 0; i < g_ndec; i++)
        if (strcmp(g_dec[i].path, path) == 0) return g_dec[i].calls;
    return 0;
}
}  // namespace

extern "C" uint32_t* image_decode(const char* filename, int* out_w, int* out_h) {
    DecodeStat* d = dec_slot(filename);
    d->calls++;
    if (strstr(filename, "fail") != NULL) return 0;     // simulasi berkas rusak
    // 2:1 → thumbnail 110x55 di dalam kotak 110 yang diuji.
    const int w = 220, h = 110;
    uint32_t* buf = (uint32_t*)malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) buf[i] = 0xFF204060u;  // solid, opaque
    *out_w = w;
    *out_h = h;
    return buf;
}

extern "C" void image_free(uint32_t* buf) { free(buf); }

// Alokator user-space: identitas ke malloc host. ThumbCache memakai
// sys_alloc/sys_free (ukuran byte) — di host cukup diteruskan.
extern "C" void* sys_alloc(uint32_t size) { return malloc(size); }
extern "C" void sys_free(void* ptr) { free(ptr); }

// ============================================================
// Header gambar contoh
// ============================================================
namespace {

uint8_t g_png[64];
uint8_t g_bmp[54];
uint8_t g_junk[64];

void be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
void le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void le16(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

void build_headers() {
    memset(g_png, 0, sizeof(g_png));
    const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(g_png, sig, 8);
    be32(g_png + 8, 13);                     // panjang chunk IHDR
    memcpy(g_png + 12, "IHDR", 4);
    be32(g_png + 16, 640);                   // lebar
    be32(g_png + 20, 480);                   // tinggi
    g_png[24] = 8; g_png[25] = 6;            // bit depth / color type

    memset(g_bmp, 0, sizeof(g_bmp));
    g_bmp[0] = 'B'; g_bmp[1] = 'M';
    le32(g_bmp + 2, 54 + 320 * 200 * 3);     // ukuran berkas
    le32(g_bmp + 10, 54);                    // offset piksel
    le32(g_bmp + 14, 40);                    // BITMAPINFOHEADER
    le32(g_bmp + 18, 320);                   // lebar (int32 LE)
    le32(g_bmp + 22, 200);                   // tinggi
    le16(g_bmp + 26, 1);                     // planes
    le16(g_bmp + 28, 24);                    // bpp

    memset(g_junk, 0xAB, sizeof(g_junk));    // bukan gambar apa pun
}

}  // namespace

// ============================================================
// Uji
// ============================================================
static void test_type_detection() {
    printf("tipe media\n");
    CHECK(media_type_of("/pics/a.png") == MEDIA_IMAGE);
    CHECK(media_type_of("/pics/a.BMP") == MEDIA_IMAGE);      // case-insensitive
    CHECK(media_type_of("a.png") == MEDIA_IMAGE);            // tanpa direktori
    CHECK(media_type_of("/pics/a.jpeg") == MEDIA_IMAGE);
    CHECK(media_type_of("/pics/a.mp4") == MEDIA_VIDEO);
    CHECK(media_type_of("/pics/a.wav") == MEDIA_AUDIO);
    CHECK(media_type_of("/pics/a.txt") == MEDIA_DOCUMENT);
    CHECK(media_type_of("/pics/app.elf") == MEDIA_UNKNOWN);
    CHECK(media_type_of("/pics/noext") == MEDIA_UNKNOWN);
    CHECK(media_type_of("/dir.with.dots/file") == MEDIA_UNKNOWN);  // titik di dir
    CHECK(media_type_of("/pics/a.") == MEDIA_UNKNOWN);             // titik tanpa ext
    CHECK(media_type_of(0) == MEDIA_UNKNOWN);

    // Format yang BELUM ada dekodernya tetap "gambar", tapi TIDAK supported:
    // UI tidak boleh menawarkannya (jangan memalsukan dukungan JPEG).
    CHECK(media_is_image("/pics/a.jpg") == 1);
    CHECK(media_is_supported_image("/pics/a.jpg") == 0);
    CHECK(media_is_supported_image("/pics/a.png") == 1);
    CHECK(media_is_supported_image("/pics/a.bmp") == 1);
    CHECK(media_is_supported_image("/pics/a.txt") == 0);
    CHECK_STR(media_type_name(MEDIA_IMAGE), "Image");
    CHECK_STR(media_type_name(MEDIA_VIDEO), "Video");
    CHECK_STR(media_type_name(MEDIA_UNKNOWN), "Unknown");
}

static void test_path_utils() {
    printf("util path\n");
    CHECK_STR(media_basename("/a/b/c.png"), "c.png");
    CHECK_STR(media_basename("c.png"), "c.png");
    CHECK_STR(media_basename(""), "");
    CHECK_STR(media_extension("/a/B.PNG"), ".png");
    CHECK_STR(media_extension("/a/b"), "");
    CHECK_STR(media_extension("/a/b."), "");

    char out[64];
    media_path_join(out, sizeof(out), "/", "a.png");
    CHECK_STR(out, "/a.png");
    media_path_join(out, sizeof(out), "/pics", "a.png");
    CHECK_STR(out, "/pics/a.png");
    media_path_join(out, sizeof(out), "/pics/", "a.png");   // separator ganda
    CHECK_STR(out, "/pics/a.png");
    media_path_join(out, sizeof(out), "", "a.png");
    CHECK_STR(out, "/a.png");
    media_path_join(out, sizeof(out), "/deep/dir", "b.bmp");
    CHECK_STR(out, "/deep/dir/b.bmp");

    media_dirname(out, sizeof(out), "/pics/a.png");
    CHECK_STR(out, "/pics");
    media_dirname(out, sizeof(out), "/a.png");
    CHECK_STR(out, "/");
    media_dirname(out, sizeof(out), "a.png");
    CHECK_STR(out, "/");
    media_dirname(out, sizeof(out), "/pics/");
    CHECK_STR(out, "/pics");
}

static void test_formatting() {
    printf("format teks\n");
    char b[96];
    media_format_size(0, b, sizeof(b));
    CHECK_STR(b, "0 B");
    media_format_size(512, b, sizeof(b));
    CHECK_STR(b, "512 B");
    media_format_size(1024, b, sizeof(b));
    CHECK_STR(b, "1.0 KB");
    media_format_size(1536, b, sizeof(b));
    CHECK_STR(b, "1.5 KB");
    media_format_size(2621440, b, sizeof(b));
    CHECK_STR(b, "2.5 MB");

    media_format_int(b, sizeof(b), 0);
    CHECK_STR(b, "0");
    media_format_int(b, sizeof(b), 42);
    CHECK_STR(b, "42");
    media_format_int(b, sizeof(b), 1000);
    CHECK_STR(b, "1000");
    media_format_int(b, sizeof(b), -7);
    CHECK_STR(b, "-7");

    media_format_info("a.png", 1920, 1080, "PNG", 2621440, b, sizeof(b));
    CHECK_STR(b, "a.png   1920x1080   PNG   2.5 MB");
    // Bagian yang tak diketahui dilewati (tanpa spasi ekor).
    media_format_info("a.png", 0, 0, 0, 0, b, sizeof(b));
    CHECK_STR(b, "a.png");
}

static void test_probe() {
    printf("probe header\n");
    int w = 0, h = 0;
    const char* fmt = 0;
    CHECK(media_probe_image("/pics/a.png", &w, &h, &fmt) == 1);
    CHECK(w == 640 && h == 480);
    CHECK_STR(fmt, "PNG");

    w = h = 0; fmt = 0;
    CHECK(media_probe_image("/pics/b.bmp", &w, &h, &fmt) == 1);
    CHECK(w == 320 && h == 200);
    CHECK_STR(fmt, "BMP");

    // Berkas pendek / bukan gambar / tidak ada: gagal dengan rapi (tanpa crash).
    CHECK(media_probe_image("/pics/short.png", &w, &h, &fmt) == 0);
    CHECK(media_probe_image("/pics/junk.png", &w, &h, &fmt) == 0);
    CHECK(media_probe_image("/pics/missing.png", &w, &h, &fmt) == 0);
    CHECK(media_probe_image(0, &w, &h, &fmt) == 0);

    uint32_t sz = 0;
    CHECK(media_file_stat("/pics/a.png", &sz) == 1 && sz == sizeof(g_png));
    CHECK(media_file_stat("/pics/missing.png", &sz) == 0);
}

static void test_scan() {
    printf("pemindaian direktori\n");
    static file_info_t entries[6];
    const char* names[6] = { "b.png", "a.png", "notes.txt", "sub", "x.jpg", "z.PNG" };
    const uint32_t sizes[6] = { 522, 100, 10, 0, 50, 20 };
    const uint8_t folder[6] = { 0, 0, 0, 1, 0, 0 };
    for (int i = 0; i < 6; i++) {
        memset(&entries[i], 0, sizeof(entries[i]));
        strncpy(entries[i].filename, names[i], sizeof(entries[i].filename) - 1);
        entries[i].size = sizes[i];
        entries[i].is_folder = folder[i];
    }
    mock_add_dir("/pics", entries, 6);

    media_entry_t out[8];
    const int n = media_scan("/pics", out, 8, MEDIA_UNKNOWN);
    CHECK(n == 6);
    // Urutan: folder dulu, lalu nama menaik (ASCII, tanpa beda besar-kecil).
    CHECK_STR(out[0].name, "sub");
    CHECK_STR(out[1].name, "a.png");
    CHECK_STR(out[2].name, "b.png");
    CHECK_STR(out[3].name, "notes.txt");
    CHECK_STR(out[4].name, "x.jpg");
    CHECK_STR(out[5].name, "z.PNG");
    // Path gabungan + ukuran + flag supported.
    CHECK_STR(out[1].path, "/pics/a.png");
    CHECK(out[1].size == 100);
    CHECK(out[0].type == MEDIA_FOLDER && out[0].supported == 0);
    CHECK(out[1].type == MEDIA_IMAGE && out[1].supported == 1);
    CHECK(out[3].type == MEDIA_DOCUMENT && out[3].supported == 1);
    CHECK(out[4].type == MEDIA_IMAGE && out[4].supported == 0);   // .jpg belum ada dekodernya
    CHECK(out[5].supported == 1);                                  // .PNG huruf besar

    // Filter tipe: hanya gambar (dipakai ImageView untuk Next/Previous).
    const int ni = media_scan("/pics", out, 8, MEDIA_IMAGE);
    CHECK(ni == 4);
    CHECK_STR(out[0].name, "a.png");
    CHECK_STR(out[3].name, "z.PNG");

    // Direktori tak ada → -1 (bukan crash), dan buffer NULL aman.
    CHECK(media_scan("/nope", out, 8, MEDIA_UNKNOWN) == -1);
    CHECK(media_scan("/pics", 0, 8, MEDIA_UNKNOWN) == 0);
    CHECK(media_scan("/pics", out, 0, MEDIA_UNKNOWN) == 0);
}

// Oracle kecil untuk media_sharpen_rgba: versi salinan-penuh (bukan cincin 3
// baris in-place). Yang diuji adalah TRAVERSAL-nya — baris y+1 harus dibaca
// dari salinan, bukan dari px yang baris y-nya sudah ditimpa output —
// sedangkan aritmetikanya sengaja identik (bukan "logika kedua").
static int clamp_sc(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void naive_sharpen_sc(uint32_t* px, int w, int h, int pct) {
    uint32_t orig[8 * 6];
    for (int i = 0; i < w * h; i++) orig[i] = px[i];
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t aa = 0, ar = 0, ag = 0, ab = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int yy = clamp_sc(y + dy, 0, h - 1);
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = clamp_sc(x + dx, 0, w - 1);
                    uint32_t v = orig[yy * w + xx];
                    uint32_t a = v >> 24;
                    aa += a;
                    ar += ((v >> 16) & 0xFFu) * a;
                    ag += ((v >> 8) & 0xFFu) * a;
                    ab += (v & 0xFFu) * a;
                }
            }
            uint32_t v = orig[y * w + x];
            uint32_t a = v >> 24;
            int sa = clamp_sc((int)a + ((int)a - (int)(aa / 9)) * pct / 100,
                              0, 255);
            if (sa == 0) {
                px[y * w + x] = 0;
                continue;
            }
            uint32_t pr = ((v >> 16) & 0xFFu) * a;
            uint32_t pg = ((v >> 8) & 0xFFu) * a;
            uint32_t pb = (v & 0xFFu) * a;
            int hi = sa * 255;
            int sr =
                clamp_sc((int)pr + ((int)pr - (int)(ar / 9)) * pct / 100, 0, hi);
            int sg =
                clamp_sc((int)pg + ((int)pg - (int)(ag / 9)) * pct / 100, 0, hi);
            int sb =
                clamp_sc((int)pb + ((int)pb - (int)(ab / 9)) * pct / 100, 0, hi);
            px[y * w + x] = ((uint32_t)sa << 24) | ((uint32_t)(sr / sa) << 16) |
                            ((uint32_t)(sg / sa) << 8) | (uint32_t)(sb / sa);
        }
    }
}

static void test_scaling() {
    printf("skalasi RGBA\n");
    int w = 0, h = 0;
    CHECK(media_fit_box(1920, 1080, 110, 110, &w, &h) == 1);
    CHECK(w == 110 && h == 61);
    CHECK(media_fit_box(50, 40, 110, 110, &w, &h) == 1);
    CHECK(w == 50 && h == 40);           // tidak melakukan upscale
    CHECK(media_fit_box(0, 10, 110, 110, &w, &h) == 0);

    // Warna solid: downscale 4x4 → 2x2 harus identik.
    uint32_t src[16];
    uint32_t dst[4];
    for (int i = 0; i < 16; i++) src[i] = 0xFF3366CCu;
    media_scale_rgba(src, 4, 4, dst, 2, 2);
    for (int i = 0; i < 4; i++) CHECK(dst[i] == 0xFF3366CCu);

    // Kiri 2 kolom merah, kanan 2 kolom biru: hasil 2x2 harus memisahkan
    // keduanya (bukti rata-rata box, bukan sampling satu piksel).
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            src[y * 4 + x] = (x < 2) ? 0xFFFF0000u : 0xFF0000FFu;
    media_scale_rgba(src, 4, 4, dst, 2, 2);
    CHECK(dst[0] == 0xFFFF0000u && dst[1] == 0xFF0000FFu);
    CHECK(dst[2] == 0xFFFF0000u && dst[3] == 0xFF0000FFu);

    // Transparan penuh → tetap transparan (alpha 0, bukan hitam pekat).
    for (int i = 0; i < 16; i++) src[i] = 0x00112233u;
    media_scale_rgba(src, 4, 4, dst, 2, 2);
    CHECK(dst[0] == 0);

    // Upscale = nearest.
    uint32_t up[16];
    src[0] = 0xAABBCCDDu; src[1] = 0xFF010203u;
    src[2] = 0xFF040506u; src[3] = 0xFF070809u;
    media_scale_rgba(src, 2, 2, up, 4, 4);
    CHECK(up[0] == 0xAABBCCDDu);         // sudut mengikuti piksel sumber
    CHECK(up[15] == 0xFF070809u);

    // --- media_sharpen_rgba: unsharp 3x3 premultiplied, in-place ---
    printf("  pertajam (unsharp 3x3)\n");
    uint32_t rowbuf[3 * 8];
    // Tepi abu 6x1: kontras tepi naik (40 -> 53), piksel yang sudah di ambang
    // lokal tidak berubah — tanpa overshoot, jadi tanpa halo.
    uint32_t step[6] = {0xFF646464u, 0xFF646464u, 0xFF8C8C8Cu,
                        0xFF8C8C8Cu, 0xFF646464u, 0xFF646464u};
    media_sharpen_rgba(step, 6, 1, rowbuf, 3 * 8, 50);
    CHECK(step[0] == 0xFF646464u && step[5] == 0xFF646464u);
    CHECK(step[1] == 0xFF5D5D5Du && step[2] == 0xFF929292u);
    CHECK(step[3] == 0xFF929292u && step[4] == 0xFF5D5D5Du);

    // amount 0 = tanpa perubahan (jalur ikon bisa dimatikan lewat knob).
    uint32_t flat[16];
    for (int i = 0; i < 16; i++) flat[i] = 0xFF3366CCu;
    media_sharpen_rgba(flat, 4, 4, rowbuf, 3 * 8, 0);
    for (int i = 0; i < 16; i++) CHECK(flat[i] == 0xFF3366CCu);
    // Bidang rata tetap persis; transparan penuh tetap 0 (bukan hitam pekat).
    media_sharpen_rgba(flat, 4, 4, rowbuf, 3 * 8, 50);
    for (int i = 0; i < 16; i++) CHECK(flat[i] == 0xFF3366CCu);
    uint32_t clr[16];
    for (int i = 0; i < 16; i++) clr[i] = 0x00112233u;
    media_sharpen_rgba(clr, 4, 4, rowbuf, 3 * 8, 50);
    for (int i = 0; i < 16; i++) CHECK(clr[i] == 0u);

    // Siluet ikon ikut lebih tegas: alpha tepi turun, bagian dalam tetap.
    uint32_t ramp[4] = {0x40202020u, 0x80202020u, 0xC0202020u, 0xFF202020u};
    media_sharpen_rgba(ramp, 4, 1, rowbuf, 3 * 8, 50);
    CHECK((ramp[0] >> 24) < 0x40u);
    CHECK((ramp[1] >> 24) == 0x80u && (ramp[2] >> 24) == 0xC0u);
    CHECK((ramp[3] >> 24) == 0xFFu);

    // Scratch kurang -> dilewati (tidak menulis di luar rowbuf).
    uint32_t keep[4] = {0xFF102030u, 0xFF102030u, 0xFF405060u, 0xFF405060u};
    media_sharpen_rgba(keep, 4, 1, rowbuf, 3, 50);
    CHECK(keep[0] == 0xFF102030u && keep[3] == 0xFF405060u);

    // Traversal in-place 7x5 (pola acak deterministik) vs oracle salinan-penuh:
    // mengunci urutan baca baris y+1 vs tulis baris y milik cincin 3 baris.
    uint32_t pat[35], ref[35];
    unsigned seed = 0x12345u;
    for (int i = 0; i < 35; i++) {
        unsigned v = 0;
        for (int c = 0; c < 4; c++) {
            seed = seed * 1103515245u + 12345u;
            v = (v << 8) | ((seed >> 16) & 0xFFu);
        }
        pat[i] = v;
        ref[i] = v;
    }
    naive_sharpen_sc(ref, 7, 5, 50);
    media_sharpen_rgba(pat, 7, 5, rowbuf, 3 * 8, 50);
    for (int i = 0; i < 35; i++) CHECK(pat[i] == ref[i]);
}

// Callback evict: Gallery memakai ini untuk mengosongkan sel, jadi uji bahwa
// nama yang dilaporkan tepat dan tidak ada slot "tampil" yang dibuang.
namespace {
char g_evicted[16][24];
int g_nevicted = 0;
void on_evict_cb(void* ud, const char* name) {
    (void)ud;
    if (g_nevicted >= 16) return;
    int i = 0;
    for (; name[i] && i < 23; i++) g_evicted[g_nevicted][i] = name[i];
    g_evicted[g_nevicted][i] = '\0';
    g_nevicted++;
}
bool evicted_contains(const char* name) {
    for (int i = 0; i < g_nevicted; i++)
        if (strcmp(g_evicted[i], name) == 0) return true;
    return false;
}
}  // namespace

static void test_thumb_cache() {
    printf("cache thumbnail\n");
    const int need = 110 * 55 * 4;      // satu thumbnail 220x110 di kotak 110

    gal::ThumbCache cache;
    cache.set_box(110);
    cache.set_budget(3 * need);
    cache.on_evict(on_evict_cb, 0);
    g_nevicted = 0;

    const char* paths[5] = { "/i0.png", "/i1.png", "/i2.png", "/i3.png", "/i4.png" };
    const uint32_t* ptr[5] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < 5; i++) {
        int w = 0, h = 0;
        ptr[i] = cache.get(paths[i], &w, &h);
        CHECK(ptr[i] != 0);
        CHECK(w == 110 && h == 55);
        CHECK(dec_calls(paths[i]) == 1);
    }
    // Batas memori dihormati: 3 thumbnail, bukan 5.
    CHECK(cache.ready_count() == 3);
    CHECK(cache.used_bytes() == 3 * need);
    CHECK(cache.used_bytes() <= cache.budget() + need);
    // Eviction LRU: dua berkas tertua yang dibuang, aplikasi diberi tahu.
    CHECK(g_nevicted == 2);
    CHECK(evicted_contains("i0.png"));
    CHECK(evicted_contains("i1.png"));
    CHECK(!evicted_contains("i4.png"));

    // Cache hit: tidak ada decode kedua, pointer identik.
    int w = 0, h = 0;
    CHECK(cache.get(paths[4], &w, &h) == ptr[4]);
    CHECK(dec_calls(paths[4]) == 1);

    // Slot yang dilindungi mark_live() tidak boleh dibuang: i4 sedang tampil,
    // jadi beban berikutnya harus mengorbankan i2 (terlama tak terpakai).
    const char* live[1] = { "i4.png" };
    cache.mark_live(live, 1);
    const uint32_t* back = cache.get(paths[0], &w, &h);
    CHECK(back != 0);
    CHECK(dec_calls(paths[0]) == 2);          // dimuat ulang setelah dibuang
    CHECK(evicted_contains("i2.png"));
    CHECK(!evicted_contains("i4.png"));
    CHECK(cache.get(paths[4], &w, &h) == ptr[4]);   // i4 selamat, tanpa decode ulang
    CHECK(dec_calls(paths[4]) == 1);
    CHECK(cache.ready_count() == 3);

    // Decode gagal: tidak dicoba lagi tiap frame (berhenti mencoba), dan
    // aplikasi diminta menggambar placeholder error.
    CHECK(cache.get("/fail.png", &w, &h) == 0);
    CHECK(dec_calls("/fail.png") == 1);
    CHECK(cache.get("/fail.png", &w, &h) == 0);
    CHECK(dec_calls("/fail.png") == 1);
    CHECK(cache.failed_count() == 1);

    // Batas ketat + banyak berkas: tetap berbatas dan tidak menggantung.
    gal::ThumbCache tight;
    tight.set_box(110);
    tight.set_budget(2 * need);
    for (int i = 0; i < 20; i++) {
        char p[32];
        snprintf(p, sizeof(p), "/bulk%d.png", i);
        CHECK(tight.get(p, &w, &h) != 0);
    }
    CHECK(tight.ready_count() == 2);
    CHECK(tight.used_bytes() == 2 * need);

    // Ganti kotak → skala berbeda, isi lama tidak dipakai ulang.
    tight.set_box(64);
    CHECK(tight.ready_count() == 0);
    CHECK(tight.used_bytes() == 0);
}

int main() {
    build_headers();
    mock_add_file("/pics/a.png", g_png, sizeof(g_png));
    mock_add_file("/pics/b.bmp", g_bmp, sizeof(g_bmp));
    mock_add_file("/pics/short.png", g_png, 10);
    mock_add_file("/pics/junk.png", g_junk, sizeof(g_junk));

    test_type_detection();
    test_path_utils();
    test_formatting();
    test_probe();
    test_scan();
    test_scaling();
    test_thumb_cache();

    printf("media_test: %d cek, %d gagal\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
