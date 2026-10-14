#ifndef KZTIME_H
#define KZTIME_H

// ============================================================
// libs/time — waktu sipil (civil time) untuk KyuzenOS.
//
// MASALAH YANG DISELESAIKAN
// Sebelum ini zona waktu di-hardcode di KERNEL (drivers/rtc.c menambahkan +7
// jam WIB ke jam CMOS) dan tanggalnya tidak ikut bergeser saat penjumlahan itu
// melewati tengah malam. Akibatnya tanggal salah selama 7 jam setiap hari, dan
// zona waktu tidak bisa dikonfigurasi sama sekali.
//
// Sekarang kernel melaporkan UTC apa adanya, dan seluruh penerjemahan
// UTC -> waktu lokal terjadi di USERSPACE lewat header ini. Itu membuat zona
// waktu bisa diubah tanpa menyentuh kernel, dan membuat logika kalender bisa
// diuji di host (tests/host) tanpa boot.
//
// PEMBAGIAN TANGGUNG JAWAB
//   kernel (drivers/rtc.c)  -> UTC mentah dari CMOS
//   libs/time (file ini)    -> aritmetika kalender + aturan zona waktu
//   aplikasi / desktop      -> memilih zona dari /time.ui, lalu memformat
//
// ATURAN
//   * Integer murni, dan itu PILIHAN. Kalender adalah aritmetika bilangan
//     bulat; float hanya menambah pembulatan. (Dulu ini keharusan karena
//     build memakai -mno-sse -msoft-float; sejak Phase 3 userspace boleh
//     memakai FP, tapi di sini tidak ada gunanya.)
//   * Tanpa alokasi, tanpa syscall, tanpa state global. Semua fungsi murni
//     sehingga aman dipanggil dari mana saja dan bisa diuji di host.
//   * Header + satu .c: bisa dipakai kernel, userspace, dan host test.
//
//     CATATAN: file ini dipakai KERNEL juga, dan kernel tetap -mno-sse
//     (interrupt entry tidak menyimpan XMM). Karena itu isinya harus tetap
//     bebas float/double — bukan karena aturan lama, tapi karena file ini
//     dikompilasi ke dalam image kernel.
// ============================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------
// Waktu sipil (kalender proleptic Gregorian).
// ------------------------------------------------------------
typedef struct {
    int32_t  year;    // 1970.. (tahun penuh, mis. 2026)
    int32_t  month;   // 1..12
    int32_t  day;     // 1..31
    int32_t  hour;    // 0..23
    int32_t  minute;  // 0..59
    int32_t  second;  // 0..59 (60 tidak pernah dihasilkan)
    int32_t  weekday; // 0=Minggu .. 6=Sabtu
    int32_t  yday;    // 0..365 (hari ke berapa dalam tahun)
} kz_civil_t;

// Detik Unix UTC <-> waktu sipil. Rentang aman: tahun 1970..2100.
// days_from_civil: algoritma Howard Hinnant (public domain), integer murni.
int64_t kz_days_from_civil(int32_t y, int32_t m, int32_t d);
void    kz_civil_from_days(int64_t days, int32_t* y, int32_t* m, int32_t* d);

// Detik Unix -> struct sipil. Membalikkan kz_civil_to_unix.
void kz_civil_from_unix(int64_t unix_sec, kz_civil_t* out);
// Struct sipil -> detik Unix. Mengabaikan weekday/yday (dihitung ulang).
int64_t kz_civil_to_unix(const kz_civil_t* in);

// Hari dalam bulan (memperhitungkan kabisat). Return 0 untuk bulan invalid.
int kz_days_in_month(int32_t year, int32_t month);
// 1 bila tahun kabisat.
int kz_is_leap(int32_t year);

// ------------------------------------------------------------
// Zona waktu sebagai ATURAN POSIX (bukan sekadar offset tetap).
//
// Kenapa aturan, bukan offset: offset tetap salah dua kali setahun di setiap
// zona yang memakai DST, dan World Clock harus bisa menunjukkan perbedaan
// tanggal yang benar. Aturan POSIX cukup ~20 byte per zona, integer murni,
// dan bisa diuji di host.
//
//   "WIB-7"                       offset tetap +7
//   "UTC0"                        offset tetap 0
//   "EST5EDT,M3.2.0,M11.1.0"      DST: mulai Minggu ke-2 Maret 02:00,
//                                 berakhir Minggu ke-1 November 02:00
//
// CATATAN TANDA: pada aturan POSIX, tanda offset adalah KEBALIKAN dari yang
// biasa ditulis manusia. "WIB-7" berarti UTC+7 (bukan UTC-7). Ini standar
// POSIX dan sengaja diikuti apa adanya supaya string bisa dipertukarkan
// dengan dunia luar.
// ------------------------------------------------------------
#define KZ_TZ_ABBR_MAX  8    // termasuk NUL ("WIB", "CEST", ...)
#define KZ_TZ_MAX      40    // panjang maksimum string aturan, termasuk NUL
#define KZ_TZ_TABLE_MAX 32   // jumlah zona bawaan

typedef struct {
    char    id[24];                 // "Asia/Jakarta" (stabil, dipakai config)
    char    label[24];              // "Jakarta" (ditampilkan di UI)
    char    rule[KZ_TZ_MAX];        // aturan POSIX, mis. "WIB-7"
} kz_zone_t;

// Tabel zona bawaan (read-only, POD, tanpa alokasi).
// Return pointer tabel, atau 0 bila tidak ada; *count diisi jumlahnya.
const kz_zone_t* kz_zone_table(int* count);
// Cari index zona berdasarkan id. Return -1 bila tidak ada.
int kz_zone_find(const char* id);
// Zona default (WIB) — index yang selalu valid.
int kz_zone_default(void);
// Offset UTC dalam DETIK untuk `unix_sec` menurut aturan POSIX `rule`.
// Return 0 bila aturan tidak bisa dibaca (aman: berarti UTC).
int32_t kz_tz_offset(const char* rule, int64_t unix_sec);
// Apakah `unix_sec` berada dalam periode DST menurut `rule`?
int kz_tz_is_dst(const char* rule, int64_t unix_sec);
// Isi `out` (kapasitas cap) dengan singkatan zona pada saat itu ("WIB"/"CEST").
// Selalu NUL-terminated; kosong bila aturan tidak punya singkatan.
void kz_tz_abbr(const char* rule, int64_t unix_sec, char* out, int cap);
// Terapkan aturan zona ke detik Unix -> struct sipil LOKAL.
void kz_civil_local(const char* rule, int64_t unix_sec, kz_civil_t* out);

// ------------------------------------------------------------
// Format ke buffer (tanpa alokasi, tanpa printf — aman di semua lapisan).
// Semua menulis NUL dan tidak pernah melewati `cap`.
// ------------------------------------------------------------
// "13:15" atau "01:15 PM" bila hour12 != 0.
void kz_fmt_hhmm(int hour, int minute, int hour12, char* out, int cap);
// "13:15:42" atau "01:15:42 PM".
void kz_fmt_hhmmss(int hour, int minute, int second, int hour12,
                   char* out, int cap);
// "Saturday, October 3"
void kz_fmt_date_long(const kz_civil_t* t, char* out, int cap);
// "3 Oct 2026"
void kz_fmt_date_short(const kz_civil_t* t, char* out, int cap);
// "2026-10-03"
void kz_fmt_date_iso(const kz_civil_t* t, char* out, int cap);
// Nama hari/bulan bahasa Inggris (ASCII saja — tidak ada locale di sistem).
const char* kz_weekday_name(int weekday);   // 0..6 -> "Sunday".."Saturday"
const char* kz_month_name(int month);       // 1..12 -> "January".."December"
const char* kz_weekday_abbr(int weekday);   // 0..6 -> "Sun".."Sat"
const char* kz_month_abbr(int month);       // 1..12 -> "Jan".."Dec"

// ------------------------------------------------------------
// Preferensi pengguna, dibaca dari "/time.ui" (pola sama dengan /font.ui).
//
//   offset 0..3 : "KZT1" (tag)
//   offset 4    : zone index (0..KZ_TZ_TABLE_MAX-1)
//   offset 5    : bit0 = format 12 jam
//   total 6 byte
//
// Fungsi encode/decode murni (tanpa IO) supaya bisa dipakai desktop, Settings,
// aplikasi, dan host test tanpa duplikasi aturan validasi.
// ------------------------------------------------------------
#define KZ_TIME_CFG_PATH "/time.ui"
#define KZ_TIME_CFG_LEN  6
#define KZ_TIME_CFG_TAG0 'K'
#define KZ_TIME_CFG_TAG1 'Z'
#define KZ_TIME_CFG_TAG2 'T'
#define KZ_TIME_CFG_TAG3 '1'

typedef struct {
    int zone;     // index zona (di-sanitize)
    int hour12;   // 0 = 24 jam, 1 = 12 jam
} kz_time_cfg_t;

// Isi default: zona default sistem, format 24 jam.
void kz_time_cfg_default(kz_time_cfg_t* out);
// Encode ke 6 byte. Return panjang (selalu KZ_TIME_CFG_LEN).
int  kz_time_cfg_encode(const kz_time_cfg_t* cfg, char out[KZ_TIME_CFG_LEN]);
// Decode; input rusak -> default (tidak pernah gagal keras).
void kz_time_cfg_decode(const char* buf, int n, kz_time_cfg_t* out);

// Baca "/time.ui" lewat syscall userlib, fallback ke default bila absen/rusak.
// Disimpan di sini supaya SETIAP pembaca waktu lokal memakai aturan validasi
// yang sama — pola yang sama dengan kz_font_cfg_decode: formatnya satu sumber
// kebenaran, IO-nya satu implementasi.
//
// HANYA boleh dipanggil dari user-space (butuh sys_open/sys_read). Kernel dan
// host test memakai kz_time_cfg_decode langsung.
int kz_time_cfg_load(kz_time_cfg_t* out);
// Tulis "/time.ui". Return 1 sukses, 0 gagal (dipakai Settings).
int kz_time_cfg_save(const kz_time_cfg_t* cfg);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KZTIME_H
