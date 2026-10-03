// libs/time/src/kztime.c — aritmetika kalender + aturan zona waktu POSIX.
//
// Murni integer, tanpa alokasi, tanpa syscall. Alasan lengkap ada di kztime.h.
#include "kztime.h"

// ------------------------------------------------------------
// Tabel zona bawaan.
//
// Sengaja KECIL dan datar: sistem ini tidak punya tzdata, dan menyalin basis
// data zona IANA (ribuan entri, ratusan KB) tidak sebanding dengan kebutuhan
// sebuah jam desktop. Yang dipakai adalah aturan POSIX untuk kota-kota yang
// benar-benar ditampilkan. Menambah kota = menambah satu baris.
//
// Urutan: Jakarta lebih dulu karena itu zona sistem saat ini (kernel dulu
// memaksa WIB), jadi index 0 tetap berarti "perilaku lama" bagi config yang
// belum pernah ditulis.
//
// Aturan DST ditulis eksplisit (bukan offset tetap) supaya kota seperti London
// dan New York benar sepanjang tahun. Bentuk Mm.w.d = bulan.minggu.hari,
// minggu 5 = terakhir, hari 0 = Minggu.
//
// Waktu transisi (/jam) BUKAN opsional demi kelengkapan: Eropa bertransisi
// pada 01:00 UTC, yang berarti 02:00 waktu standar saat mulai tetapi 03:00
// waktu DST saat berakhir. Aturan tanpa /jam (default 02:00 lokal) memberi
// Berlin/London hasil yang meleset satu jam selama 7 jam terakhir DST setiap
// tahun. Nilai di bawah sudah diverifikasi terhadap basis data zona IANA
// (tests/host/unit/time_test.c mengunci instan transisinya).
// ------------------------------------------------------------
static const kz_zone_t g_zones[] = {
    // id                    label             aturan POSIX
    { "Asia/Jakarta",       "Jakarta",        "WIB-7" },
    { "Asia/Makassar",      "Makassar",       "WITA-8" },
    { "Asia/Jayapura",      "Jayapura",       "WIT-9" },
    { "Asia/Singapore",     "Singapore",      "SGT-8" },
    { "Asia/Tokyo",         "Tokyo",          "JST-9" },
    { "Asia/Seoul",         "Seoul",          "KST-9" },
    { "Asia/Shanghai",      "Shanghai",       "CST-8" },
    { "Asia/Kolkata",       "New Delhi",      "IST-5:30" },
    { "Asia/Dubai",         "Dubai",          "GST-4" },
    { "Europe/Istanbul",    "Istanbul",       "TRT-3" },
    { "Europe/Moscow",      "Moscow",         "MSK-3" },
    { "Europe/Berlin",      "Berlin",         "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",       "Paris",          "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",        "Rome",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",      "Madrid",         "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam",   "Amsterdam",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/London",      "London",         "GMT0BST,M3.5.0/1,M10.5.0/2" },
    { "Europe/Lisbon",      "Lisbon",         "WET0WEST,M3.5.0/1,M10.5.0/2" },
    { "America/New_York",   "New York",       "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",    "Chicago",        "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",     "Denver",         "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles","Los Angeles",    "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Sao_Paulo",  "Sao Paulo",      "BRT3" },
    { "America/Mexico_City","Mexico City",    "CST6" },
    { "Africa/Cairo",       "Cairo",          "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Africa/Johannesburg","Johannesburg",   "SAST-2" },
    { "Africa/Lagos",       "Lagos",          "WAT-1" },
    { "Asia/Karachi",       "Karachi",        "PKT-5" },
    { "Australia/Sydney",   "Sydney",         "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Perth",    "Perth",          "AWST-8" },
    { "Pacific/Auckland",   "Auckland",       "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "UTC",                "UTC",            "UTC0" },
};

const kz_zone_t* kz_zone_table(int* count) {
    if (count) *count = (int)(sizeof(g_zones) / sizeof(g_zones[0]));
    return g_zones;
}

int kz_zone_find(const char* id) {
    if (!id) return -1;
    int n = (int)(sizeof(g_zones) / sizeof(g_zones[0]));
    for (int i = 0; i < n; i++) {
        const char* a = g_zones[i].id;
        const char* b = id;
        int k = 0;
        while (a[k] && b[k] && a[k] == b[k]) k++;
        if (a[k] == '\0' && b[k] == '\0') return i;
    }
    return -1;
}

int kz_zone_default(void) { return 0; }   // Asia/Jakarta

// ------------------------------------------------------------
// Kalender.
// ------------------------------------------------------------

int kz_is_leap(int32_t y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// Hari dalam bulan; 0 bila bulan di luar 1..12 (pemanggil memperlakukannya
// sebagai error, bukan diam-diam memakai Januari).
int kz_days_in_month(int32_t y, int32_t m) {
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 0;
    if (m == 2 && kz_is_leap(y)) return 29;
    return d[m - 1];
}

// Hinnant, days_from_civil (public domain): hari sejak 1970-01-01.
// Integer murni; pembagian ke arah nol dikompensasi dengan `era` negatif.
int64_t kz_days_from_civil(int32_t y, int32_t m, int32_t d) {
    int32_t yy = y;
    yy -= (m <= 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int32_t yoe = (int32_t)(yy - era * 400);                    // 0..399
    int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // 0..365
    int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;        // 0..146096
    return era * 146097 + doe - 719468;
}

// Kebalikannya (civil_from_days).
void kz_civil_from_days(int64_t z, int32_t* oy, int32_t* om, int32_t* od) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                    // 0..146096
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);              // 0..365
    int64_t mp = (5 * doy + 2) / 153;                                   // 0..11
    int64_t d = doy - (153 * mp + 2) / 5 + 1;                           // 1..31
    int64_t m = mp + (mp < 10 ? 3 : -9);                                // 1..12
    if (oy) *oy = (int32_t)(y + (m <= 2));
    if (om) *om = (int32_t)m;
    if (od) *od = (int32_t)d;
}

void kz_civil_from_unix(int64_t unix_sec, kz_civil_t* out) {
    if (!out) return;
    // floor-div: detik negatif (sebelum 1970) tetap memberi hari yang benar.
    int64_t days = unix_sec / 86400;
    int64_t rem  = unix_sec % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }

    int32_t y = 1970, m = 1, d = 1;
    kz_civil_from_days(days, &y, &m, &d);

    out->year   = y;
    out->month  = m;
    out->day    = d;
    out->hour   = (int32_t)(rem / 3600);
    out->minute = (int32_t)((rem % 3600) / 60);
    out->second = (int32_t)(rem % 60);
    // 1970-01-01 = Kamis. +4 menggeser Minggu=0.
    int64_t wd = (days + 4) % 7;
    if (wd < 0) wd += 7;
    out->weekday = (int32_t)wd;
    out->yday = (int32_t)(days - kz_days_from_civil(y, 1, 1));
}

int64_t kz_civil_to_unix(const kz_civil_t* in) {
    if (!in) return 0;
    int64_t days = kz_days_from_civil(in->year, in->month, in->day);
    return days * 86400 + (int64_t)in->hour * 3600
                         + (int64_t)in->minute * 60 + in->second;
}

// ------------------------------------------------------------
// Parser aturan POSIX TZ.
//
// Tata bahasa yang didukung (subset POSIX yang dipakai tabel di atas):
//   std offset [dst [offset] [,start[/time],end[/time]]]
//   offset := [+-]hh[:mm[:ss]]
//   start/end := Jn | n | Mm.w.d   (opsional /time)
//
// Aturan yang tidak bisa dibaca -> offset 0 (UTC). Tidak pernah gagal keras:
// jam yang menampilkan UTC lebih baik daripada jam yang tidak tampil.
// ------------------------------------------------------------
typedef struct {
    int kind;        // 0 = Jn, 1 = n (0-based), 2 = Mm.w.d
    int a, b, c;     // Jn: a=n · n: a=n · Mm.w.d: a=m, b=w, c=d
    int seconds;     // waktu transisi lokal (detik sejak 00:00)
} tz_rule_t;

typedef struct {
    int32_t   off_std;              // detik (konvensi POSIX: UTC = lokal + off)
    int32_t   off_dst;
    int       has_dst;
    tz_rule_t start, end;
    char      abbr_std[KZ_TZ_ABBR_MAX];
    char      abbr_dst[KZ_TZ_ABBR_MAX];
} tz_parsed_t;

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Salin nama singkatan (huruf saja). Return panjang; 0 bila kurang dari 3
// huruf (POSIX mensyaratkan >= 3 supaya tidak ambigu dengan offset).
static int tz_take_abbr(const char** p, char* out, int cap) {
    int n = 0;
    const char* s = *p;
    while (is_alpha(s[0]) && n < cap - 1) { out[n++] = *s++; }
    out[n] = '\0';
    *p = s;
    return n >= 3 ? n : 0;
}

// [+-]hh[:mm[:ss]] -> detik. Mengembalikan 1 bila berhasil.
static int tz_take_offset(const char** p, int32_t* out) {
    const char* s = *p;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    if (!is_digit(s[0])) return 0;
    int32_t h = 0;
    while (is_digit(s[0]) && h < 1000) h = h * 10 + (*s++ - '0');
    int32_t mm = 0, ss = 0;
    if (*s == ':') {
        s++;
        if (!is_digit(s[0])) return 0;
        while (is_digit(s[0])) mm = mm * 10 + (*s++ - '0');
        if (*s == ':') {
            s++;
            if (!is_digit(s[0])) return 0;
            while (is_digit(s[0])) ss = ss * 10 + (*s++ - '0');
        }
    }
    if (mm > 59 || ss > 59) return 0;
    int32_t v = h * 3600 + mm * 60 + ss;
    *out = neg ? -v : v;
    *p = s;
    return 1;
}

static int tz_take_int(const char** p, int* out) {
    const char* s = *p;
    if (!is_digit(s[0])) return 0;
    int v = 0;
    while (is_digit(s[0]) && v < 100000) v = v * 10 + (*s++ - '0');
    *out = v;
    *p = s;
    return 1;
}

// Jn | n | Mm.w.d  (opsional "/time")
static int tz_take_rule(const char** p, tz_rule_t* r) {
    const char* s = *p;
    r->seconds = 2 * 3600;   // default POSIX: 02:00 waktu lokal
    if (*s == 'J') {
        s++;
        int n = 0;
        if (!tz_take_int(&s, &n) || n < 1 || n > 365) return 0;
        r->kind = 0; r->a = n;
    } else if (*s == 'M') {
        s++;
        int m = 0, w = 0, d = 0;
        if (!tz_take_int(&s, &m) || *s != '.') return 0;
        s++;
        if (!tz_take_int(&s, &w) || *s != '.') return 0;
        s++;
        if (!tz_take_int(&s, &d)) return 0;
        if (m < 1 || m > 12 || w < 1 || w > 5 || d < 0 || d > 6) return 0;
        r->kind = 2; r->a = m; r->b = w; r->c = d;
    } else if (is_digit(*s)) {
        int n = 0;
        if (!tz_take_int(&s, &n) || n > 365) return 0;
        r->kind = 1; r->a = n;
    } else {
        return 0;
    }
    if (*s == '/') {
        s++;
        int32_t sec = 0;
        if (!tz_take_offset(&s, &sec)) return 0;
        r->seconds = sec;
    }
    *p = s;
    return 1;
}

static int tz_parse(const char* rule, tz_parsed_t* out) {
    if (!rule || !out) return 0;
    out->off_std = 0; out->off_dst = 0; out->has_dst = 0;
    out->abbr_std[0] = '\0'; out->abbr_dst[0] = '\0';

    const char* s = rule;
    if (!tz_take_abbr(&s, out->abbr_std, KZ_TZ_ABBR_MAX)) return 0;
    if (!tz_take_offset(&s, &out->off_std)) return 0;
    if (*s == '\0') return 1;                 // zona tanpa DST

    if (!tz_take_abbr(&s, out->abbr_dst, KZ_TZ_ABBR_MAX)) return 0;
    out->has_dst = 1;
    // Offset DST default = std - 1 jam (POSIX: DST selalu 1 jam lebih maju).
    out->off_dst = out->off_std - 3600;
    if (*s != ',') {
        if (!tz_take_offset(&s, &out->off_dst)) return 0;
    }
    if (*s != ',') return 0;
    s++;
    if (!tz_take_rule(&s, &out->start)) return 0;
    if (*s != ',') return 0;
    s++;
    if (!tz_take_rule(&s, &out->end)) return 0;
    return *s == '\0';
}

// Hari transisi sebagai JUMLAH HARI SEJAK EPOCH.
//
// Mengembalikan hari absolut (bukan tanggal-dalam-bulan): pemanggil
// mengalikannya dengan 86400, jadi mengembalikan tanggal 1..31 akan
// menempatkan transisi Maret pada hari ke-8 JANUARI — bug yang pernah ada di
// sini dan membuat DST New York/London aktif di musim dingin.
static int64_t tz_rule_days(const tz_rule_t* r, int32_t year) {
    if (r->kind == 0) {                       // Jn: Feb 29 tidak dihitung
        int n = r->a;
        if (kz_is_leap(year) && n >= 60) n += 1;
        return kz_days_from_civil(year, 1, 1) + (n - 1);
    }
    if (r->kind == 1) {                       // n: 0-based, Feb 29 dihitung
        return kz_days_from_civil(year, 1, 1) + r->a;
    }

    // Mm.w.d — minggu ke-w hari ke-d (5 = terakhir) dalam bulan ke-m.
    int32_t m = r->a;
    int32_t wd_first = (int32_t)((kz_days_from_civil(year, m, 1) + 4) % 7);
    if (wd_first < 0) wd_first += 7;
    int32_t dom = 1 + ((r->c - wd_first + 7) % 7) + (r->b - 1) * 7;
    int32_t dim = kz_days_in_month(year, m);
    while (dom > dim) dom -= 7;               // minggu ke-5 = minggu terakhir
    return kz_days_from_civil(year, m, dom);
}

// Detik Unix transisi DST untuk tahun `year`. `is_start` memilih aturan.
// Waktu transisi dinyatakan dalam waktu lokal dari sisi yang berlaku:
// awal dalam STD, akhir dalam DST — sesuai POSIX.
static int64_t tz_transition_utc(const tz_parsed_t* p, int is_start,
                                 int32_t year) {
    const tz_rule_t* r = is_start ? &p->start : &p->end;
    int64_t local = tz_rule_days(r, year) * 86400 + r->seconds;
    // UTC = lokal + offset POSIX.
    return local + (is_start ? p->off_std : p->off_dst);
}

int kz_tz_is_dst(const char* rule, int64_t unix_sec) {
    tz_parsed_t p;
    if (!tz_parse(rule, &p) || !p.has_dst) return 0;
    // Tahun perkiraan dari waktu lokal standar — transisi DST tidak pernah
    // dekat 1 Januari pada tabel zona di atas, jadi aproksimasi ini aman.
    int64_t approx = unix_sec - p.off_std;
    int32_t y = 1970, m = 1, d = 1;
    kz_civil_from_days(approx / 86400, &y, &m, &d);

    int64_t s = tz_transition_utc(&p, 1, y);
    int64_t e = tz_transition_utc(&p, 0, y);
    if (s < e) return (unix_sec >= s && unix_sec < e);   // belahan utara
    return (unix_sec >= s || unix_sec < e);              // belahan selatan
}

int32_t kz_tz_offset(const char* rule, int64_t unix_sec) {
    tz_parsed_t p;
    if (!tz_parse(rule, &p)) return 0;
    // local = UTC - offset, jadi offset POSIX dinegasikan di sini.
    if (p.has_dst && kz_tz_is_dst(rule, unix_sec)) return -p.off_dst;
    return -p.off_std;
}

void kz_tz_abbr(const char* rule, int64_t unix_sec, char* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    tz_parsed_t p;
    if (!tz_parse(rule, &p)) return;
    const char* src = (p.has_dst && kz_tz_is_dst(rule, unix_sec))
                    ? p.abbr_dst : p.abbr_std;
    int k = 0;
    for (; src[k] && k < cap - 1; k++) out[k] = src[k];
    out[k] = '\0';
}

void kz_civil_local(const char* rule, int64_t unix_sec, kz_civil_t* out) {
    if (!out) return;
    kz_civil_from_unix(unix_sec + kz_tz_offset(rule, unix_sec), out);
}

// ------------------------------------------------------------
// Format.
// ------------------------------------------------------------
static void put(char* out, int cap, int* k, char c) {
    if (*k < cap - 1) out[(*k)++] = c;
}
static void put2(char* out, int cap, int* k, int v) {
    put(out, cap, k, (char)('0' + (v / 10) % 10));
    put(out, cap, k, (char)('0' + v % 10));
}
static void puts_(char* out, int cap, int* k, const char* s) {
    for (int i = 0; s[i]; i++) put(out, cap, k, s[i]);
}
static void finish(char* out, int cap, int k) {
    if (cap > 0) out[k < cap ? k : cap - 1] = '\0';
}

void kz_fmt_hhmm(int hour, int minute, int hour12, char* out, int cap) {
    if (!out || cap <= 0) return;
    int k = 0;
    if (hour12) {
        int h = hour % 12;
        if (h == 0) h = 12;
        put2(out, cap, &k, h);
        put(out, cap, &k, ':');
        put2(out, cap, &k, minute);
        puts_(out, cap, &k, hour < 12 ? " AM" : " PM");
    } else {
        put2(out, cap, &k, hour);
        put(out, cap, &k, ':');
        put2(out, cap, &k, minute);
    }
    finish(out, cap, k);
}

void kz_fmt_hhmmss(int hour, int minute, int second, int hour12,
                   char* out, int cap) {
    if (!out || cap <= 0) return;
    int k = 0;
    if (hour12) {
        int h = hour % 12;
        if (h == 0) h = 12;
        put2(out, cap, &k, h);
        put(out, cap, &k, ':');
        put2(out, cap, &k, minute);
        put(out, cap, &k, ':');
        put2(out, cap, &k, second);
        puts_(out, cap, &k, hour < 12 ? " AM" : " PM");
    } else {
        put2(out, cap, &k, hour);
        put(out, cap, &k, ':');
        put2(out, cap, &k, minute);
        put(out, cap, &k, ':');
        put2(out, cap, &k, second);
    }
    finish(out, cap, k);
}

const char* kz_weekday_name(int w) {
    static const char* n[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                "Thursday", "Friday", "Saturday" };
    return (w >= 0 && w < 7) ? n[w] : "";
}
const char* kz_month_name(int m) {
    static const char* n[12] = { "January", "February", "March", "April",
                                 "May", "June", "July", "August",
                                 "September", "October", "November", "December" };
    return (m >= 1 && m <= 12) ? n[m - 1] : "";
}
const char* kz_weekday_abbr(int w) {
    static const char* n[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    return (w >= 0 && w < 7) ? n[w] : "";
}
const char* kz_month_abbr(int m) {
    static const char* n[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    return (m >= 1 && m <= 12) ? n[m - 1] : "";
}

void kz_fmt_date_long(const kz_civil_t* t, char* out, int cap) {
    if (!out || cap <= 0) return;
    if (!t) { out[0] = '\0'; return; }
    int k = 0;
    puts_(out, cap, &k, kz_weekday_name(t->weekday));
    puts_(out, cap, &k, ", ");
    puts_(out, cap, &k, kz_month_name(t->month));
    put(out, cap, &k, ' ');
    // Hari tanpa nol di depan ("October 3", bukan "October 03").
    if (t->day >= 10) put(out, cap, &k, (char)('0' + (t->day / 10) % 10));
    put(out, cap, &k, (char)('0' + t->day % 10));
    finish(out, cap, k);
}

void kz_fmt_date_short(const kz_civil_t* t, char* out, int cap) {
    if (!out || cap <= 0) return;
    if (!t) { out[0] = '\0'; return; }
    int k = 0;
    put(out, cap, &k, (char)('0' + t->day % 10));
    put(out, cap, &k, ' ');
    puts_(out, cap, &k, kz_month_abbr(t->month));
    put(out, cap, &k, ' ');
    int y = t->year;
    put(out, cap, &k, (char)('0' + (y / 1000) % 10));
    put(out, cap, &k, (char)('0' + (y / 100) % 10));
    put(out, cap, &k, (char)('0' + (y / 10) % 10));
    put(out, cap, &k, (char)('0' + y % 10));
    finish(out, cap, k);
}

void kz_fmt_date_iso(const kz_civil_t* t, char* out, int cap) {
    if (!out || cap <= 0) return;
    if (!t) { out[0] = '\0'; return; }
    int k = 0;
    int y = t->year;
    put(out, cap, &k, (char)('0' + (y / 1000) % 10));
    put(out, cap, &k, (char)('0' + (y / 100) % 10));
    put(out, cap, &k, (char)('0' + (y / 10) % 10));
    put(out, cap, &k, (char)('0' + y % 10));
    put(out, cap, &k, '-');
    put2(out, cap, &k, t->month);
    put(out, cap, &k, '-');
    put2(out, cap, &k, t->day);
    finish(out, cap, k);
}

// ------------------------------------------------------------
// Preferensi /time.ui.
// ------------------------------------------------------------
void kz_time_cfg_default(kz_time_cfg_t* out) {
    if (!out) return;
    out->zone = kz_zone_default();
    out->hour12 = 0;
}

int kz_time_cfg_encode(const kz_time_cfg_t* cfg, char out[KZ_TIME_CFG_LEN]) {
    if (!out) return 0;
    kz_time_cfg_t d;
    if (!cfg) { kz_time_cfg_default(&d); cfg = &d; }
    out[0] = KZ_TIME_CFG_TAG0;
    out[1] = KZ_TIME_CFG_TAG1;
    out[2] = KZ_TIME_CFG_TAG2;
    out[3] = KZ_TIME_CFG_TAG3;
    out[4] = (char)(cfg->zone < 0 ? 0 : cfg->zone);
    out[5] = (char)(cfg->hour12 ? 1 : 0);
    return KZ_TIME_CFG_LEN;
}

void kz_time_cfg_decode(const char* buf, int n, kz_time_cfg_t* out) {
    if (!out) return;
    if (!buf || n != KZ_TIME_CFG_LEN) { kz_time_cfg_default(out); return; }
    if (buf[0] != KZ_TIME_CFG_TAG0 || buf[1] != KZ_TIME_CFG_TAG1 ||
        buf[2] != KZ_TIME_CFG_TAG2 || buf[3] != KZ_TIME_CFG_TAG3) {
        kz_time_cfg_default(out);
        return;
    }
    int zones = 0;
    kz_zone_table(&zones);
    int z = (int)(unsigned char)buf[4];
    // Zona di luar rentang (file rusak / tabel menyusut) -> default.
    out->zone = (z >= 0 && z < zones) ? z : kz_zone_default();
    out->hour12 = (buf[5] & 1) ? 1 : 0;
}
