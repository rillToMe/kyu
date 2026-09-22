#ifndef KYUZEN_MEDIA_H
#define KYUZEN_MEDIA_H

// ============================================================
// media.h — abstraksi media bersama aplikasi GUI KyuzenOS.
//
// Satu tempat untuk:
//   - klasifikasi tipe berkas (gambar/video/audio/dokumen) — TIDAK ada
//     `if (ext == ".png")` yang tersebar di UI,
//   - util path (basename/ext/dirname/join) yang dipakai bersama,
//   - pemindaian direktori → daftar entri terurut,
//   - format ukuran berkas human-readable,
//   - probe metadata gambar tanpa decode penuh (header saja).
//
// Implementasi: libs/media/media.c (library bersama user-space, pola libs/media/png.c —
// dikompilasi sekali, di-link oleh app yang membutuhkannya). Gallery dan
// ImageView memakainya; VideoPlayer/AudioPlayer/DocumentViewer kelak cukup
// menambah baris di tabel ekstensi di media.c.
//
// SENGAJA kecil dan datar: tidak ada factory/registry/plugin. Tabel statis
// + fungsi murni. Tipe media baru = satu baris di k_media_ext[].
// ============================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Batas ukuran string, selaras ABI filesystem (file_info_t.filename[24]).
#define MEDIA_NAME_MAX    24
#define MEDIA_PATH_MAX    64

// Batas entri satu pemindaian direktori. Kernel membatasi sys_get_file_list
// pada UC_MAX_ENTRIES (128); app memakai angka lebih kecil supaya RAM app
// (daftar + nama) tetap terbatas. Gallery memakai grid virtual, jadi batas
// ini adalah batas jumlah item yang diketahui sekaligus — bukan batas visible.
#define MEDIA_MAX_ENTRIES 64

typedef enum {
    MEDIA_UNKNOWN = 0,
    MEDIA_FOLDER,
    MEDIA_IMAGE,
    MEDIA_VIDEO,
    MEDIA_AUDIO,
    MEDIA_DOCUMENT
} media_type_t;

// Satu berkas hasil pemindaian. `name` = basename (dipotong MEDIA_NAME_MAX-1,
// mengikuti batas ABI listing KyuzenFS), `path` = path lengkap.
typedef struct {
    char     name[MEDIA_NAME_MAX];
    char     path[MEDIA_PATH_MAX];
    uint32_t size;       // byte; 0 bila tak diketahui (folder)
    uint8_t  type;       // media_type_t
    uint8_t  supported;  // 1 = ada handler/dekoder yang terpasang saat ini
} media_entry_t;

// ------------------------------------------------------------
// Deteksi tipe
// ------------------------------------------------------------
// Tipe berdasarkan ekstensi (case-insensitive). Path tanpa ekstensi →
// MEDIA_UNKNOWN. MEDIA_FOLDER hanya dipakai media_scan (entri direktori).
media_type_t media_type_of(const char* path);

// 1 = ekstensi keluarga gambar yang DIKENAL (termasuk yang belum bisa
// didekode). Dipakai UI untuk membedakan "bukan gambar" dari "gambar tapi
// formatnya belum didukung".
int media_is_image(const char* path);

// 1 = gambar yang benar-benar bisa didekode DEKODER YANG ADA (hari ini PNG +
// BMP; lihat tabel di libs/media/media.c). App HANYA boleh menawarkan format yang
// lolos fungsi ini — jangan pernah mengiklankan format yang belum ada
// dekodernya.
int media_is_supported_image(const char* path);

// Nama tipe untuk UI ("Image", "Video", "Folder", ...).
const char* media_type_name(media_type_t t);

// ------------------------------------------------------------
// Path (tanpa alokasi; index ke dalam string input)
// ------------------------------------------------------------
const char* media_basename(const char* path);   // "a b/c.png" → "c.png"
const char* media_extension(const char* path);   // ".png" (lowercase) / ""
// dir + "/" + name → out (selalu punya separator tunggal; bound-check).
void media_path_join(char* out, int cap, const char* dir, const char* name);
// Path tanpa komponen terakhir; akar → "/".
void media_dirname(char* out, int cap, const char* path);

// ------------------------------------------------------------
// Pemindaian direktori
// ------------------------------------------------------------
// Isi `out` (maks `max`) dengan entri `dir`, terurut: folder dulu, lalu nama
// menaik (ASCII). `only` = MEDIA_UNKNOWN → semua tipe.
// Return jumlah entri (>= 0), atau -1 bila direktori tak bisa dibaca.
int media_scan(const char* dir, media_entry_t* out, int max, media_type_t only);

// ------------------------------------------------------------
// Metadata
// ------------------------------------------------------------
// Ukuran human-readable gaya Explorer: "512 B", "12.3 KB", "2.4 MB".
// Selalu menulis string NUL-terminated (maks cap-1).
void media_format_size(uint32_t bytes, char* out, int cap);

// 1 = berkas ada; *size (boleh NULL) diisi ukuran byte-nya.
int media_file_stat(const char* path, uint32_t* size);

// Tulis integer desimal ke buffer NUL-terminated (tanpa printf: app GUI tidak
// menautkan libc). Return jumlah byte yang ditulis (tanpa terminator).
int media_format_int(char* out, int cap, int value);

// Satu baris metadata untuk status bar, dipakai bersama Gallery & ImageView:
//   "island.png   1920x1080   PNG   2.4 MB"
// Bagian yang tak diketahui (dimensi 0 / format kosong / ukuran 0) dilewati.
void media_format_info(const char* name, int width, int height,
                       const char* fmt, uint32_t size, char* out, int cap);

// Baca HANYA header berkas gambar untuk mendapat dimensi + format tanpa
// decode penuh (PNG IHDR / BMP BITMAPINFOHEADER). Mengembalikan 1 sukses,
// 0 gagal/tak dikenal. `fmt` diisi string statis ("PNG"/"BMP"), dimensi yang
// tak tersedia dibiarkan 0.
int media_probe_image(const char* path, int* w, int* h, const char** fmt);

// ------------------------------------------------------------
// Dekode piksel — implementasi di libs/media/png.c (stb_image: PNG + BMP).
// Hasil = buffer ARGB8888 (byte alpha), wajib dibebaskan dengan image_free().
// ------------------------------------------------------------
uint32_t* image_decode(const char* filename, int* out_w, int* out_h);
void image_free(uint32_t* buf);

#ifdef __cplusplus
}
#endif

#endif // KYUZEN_MEDIA_H
