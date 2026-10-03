// tests/host/unit/time_test.c — libs/time: kalender + aturan zona waktu POSIX.
//
// Kenapa test ini ada: sebelum libs/time, kernel menambahkan +7 jam ke jam CMOS
// TANPA menggeser tanggal. Bug itu tidak terdeteksi test mana pun (test
// desktop men-stub sys_get_time). Test ini mengunci aritmetika kalender dan
// evaluasi DST sehingga kesalahan seperti itu tertangkap di host, bukan di QEMU.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "kztime.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_INT(got, want) do { \
    g_checks++; \
    long long _g = (long long)(got), _w = (long long)(want); \
    if (_g != _w) { \
        printf("FAIL %s:%d  %s = %lld, want %lld\n", \
               __FILE__, __LINE__, #got, _g, _w); \
        g_fail++; \
    } \
} while (0)

#define CHECK_STR(got, want) do { \
    g_checks++; \
    if (strcmp((got), (want)) != 0) { \
        printf("FAIL %s:%d  %s = \"%s\", want \"%s\"\n", \
               __FILE__, __LINE__, #got, (got), (want)); \
        g_fail++; \
    } \
} while (0)

// ------------------------------------------------------------
// 1. Kalender: konversi bolak-balik dan nilai yang diketahui.
// ------------------------------------------------------------
static void test_calendar(void) {
    // Titik acuan: epoch.
    CHECK_INT(kz_days_from_civil(1970, 1, 1), 0);
    CHECK_INT(kz_days_from_civil(1969, 12, 31), -1);
    CHECK_INT(kz_days_from_civil(1970, 1, 2), 1);

    // 2000-03-01 = 11017 (dihitung independen; kabisat abad).
    CHECK_INT(kz_days_from_civil(2000, 3, 1), 11017);
    // 2026-09-20 = 20716.
    CHECK_INT(kz_days_from_civil(2026, 9, 20), 20716);

    int32_t y = 0, m = 0, d = 0;
    kz_civil_from_days(11017, &y, &m, &d);
    CHECK_INT(y, 2000); CHECK_INT(m, 3); CHECK_INT(d, 1);

    kz_civil_from_days(0, &y, &m, &d);
    CHECK_INT(y, 1970); CHECK_INT(m, 1); CHECK_INT(d, 1);

    // Bolak-balik untuk rentang luas — ini yang menangkap bug pembulatan
    // pembagian negatif (era) di days_from_civil.
    int bad = 0;
    for (int64_t days = -20000; days <= 40000; days += 7) {
        int32_t yy, mm, dd;
        kz_civil_from_days(days, &yy, &mm, &dd);
        if (kz_days_from_civil(yy, mm, dd) != days) bad++;
    }
    CHECK_INT(bad, 0);

    // Kabisat.
    CHECK_INT(kz_is_leap(2000), 1);   // habis dibagi 400
    CHECK_INT(kz_is_leap(1900), 0);   // habis dibagi 100, bukan 400
    CHECK_INT(kz_is_leap(2024), 1);
    CHECK_INT(kz_is_leap(2026), 0);
    CHECK_INT(kz_days_in_month(2024, 2), 29);
    CHECK_INT(kz_days_in_month(2026, 2), 28);
    CHECK_INT(kz_days_in_month(2026, 12), 31);
    CHECK_INT(kz_days_in_month(2026, 13), 0);   // invalid, bukan diam-diam 31
}

// ------------------------------------------------------------
// 2. Unix <-> sipil, termasuk detik negatif (sebelum 1970).
// ------------------------------------------------------------
static void test_unix(void) {
    kz_civil_t t;
    kz_civil_from_unix(0, &t);
    CHECK_INT(t.year, 1970); CHECK_INT(t.month, 1); CHECK_INT(t.day, 1);
    CHECK_INT(t.hour, 0); CHECK_INT(t.minute, 0); CHECK_INT(t.second, 0);
    CHECK_INT(t.weekday, 4);   // 1970-01-01 = Kamis
    CHECK_INT(t.yday, 0);

    // 2026-09-20 13:15:42 UTC.
    kz_civil_t in;
    memset(&in, 0, sizeof in);
    in.year = 2026; in.month = 9; in.day = 20;
    in.hour = 13; in.minute = 15; in.second = 42;
    int64_t u = kz_civil_to_unix(&in);
    CHECK_INT(u, 20716LL * 86400 + 13 * 3600 + 15 * 60 + 42);

    kz_civil_from_unix(u, &t);
    CHECK_INT(t.year, 2026); CHECK_INT(t.month, 9); CHECK_INT(t.day, 20);
    CHECK_INT(t.hour, 13); CHECK_INT(t.minute, 15); CHECK_INT(t.second, 42);

    // Detik negatif: floor-div harus memberi 1969-12-31 23:59:59, bukan
    // 1970-01-01 00:00:01 seperti pembagian pemotong.
    kz_civil_from_unix(-1, &t);
    CHECK_INT(t.year, 1969); CHECK_INT(t.month, 12); CHECK_INT(t.day, 31);
    CHECK_INT(t.hour, 23); CHECK_INT(t.minute, 59); CHECK_INT(t.second, 59);
    CHECK_INT(t.weekday, 3);   // Rabu

    kz_civil_from_unix(-86400, &t);
    CHECK_INT(t.year, 1969); CHECK_INT(t.month, 12); CHECK_INT(t.day, 31);
}

// ------------------------------------------------------------
// 3. Offset zona: offset tetap + DST di kedua belahan bumi.
// ------------------------------------------------------------
static void test_offsets(void) {
    // Offset tetap. Tanda POSIX: "WIB-7" = UTC+7.
    CHECK_INT(kz_tz_offset("WIB-7", 0), 7 * 3600);
    CHECK_INT(kz_tz_offset("UTC0", 0), 0);
    CHECK_INT(kz_tz_offset("IST-5:30", 0), 5 * 3600 + 30 * 60);   // India +5:30
    CHECK_INT(kz_tz_offset("EST5", 0), -5 * 3600);                // tanda positif = barat
    CHECK_INT(kz_tz_offset("WITA-8", 0), 8 * 3600);

    // Aturan rusak -> UTC (0), bukan crash.
    CHECK_INT(kz_tz_offset("", 0), 0);
    CHECK_INT(kz_tz_offset("X", 0), 0);
    CHECK_INT(kz_tz_offset("WIB", 0), 0);        // offset hilang
    CHECK_INT(kz_tz_offset(0, 0), 0);

    // --- New York 2026: DST 8 Mar 2026 02:00 EST -> 7 Nov 2026 02:00 EDT ---
    // 2026-01-15 (musim dingin) = UTC-5.
    kz_civil_t jan; memset(&jan, 0, sizeof jan);
    jan.year = 2026; jan.month = 1; jan.day = 15; jan.hour = 12;
    int64_t u_jan = kz_civil_to_unix(&jan);
    CHECK_INT(kz_tz_offset("EST5EDT,M3.2.0,M11.1.0", u_jan), -5 * 3600);
    CHECK_INT(kz_tz_is_dst("EST5EDT,M3.2.0,M11.1.0", u_jan), 0);

    // 2026-07-15 (musim panas) = UTC-4.
    kz_civil_t jul; memset(&jul, 0, sizeof jul);
    jul.year = 2026; jul.month = 7; jul.day = 15; jul.hour = 12;
    int64_t u_jul = kz_civil_to_unix(&jul);
    CHECK_INT(kz_tz_offset("EST5EDT,M3.2.0,M11.1.0", u_jul), -4 * 3600);
    CHECK_INT(kz_tz_is_dst("EST5EDT,M3.2.0,M11.1.0", u_jul), 1);

    // Tepat di kedua sisi transisi musim semi 2026-03-08 02:00 lokal (07:00 UTC).
    int64_t spring = kz_civil_to_unix(&(kz_civil_t){ 2026, 3, 8, 7, 0, 0, 0, 0 });
    CHECK_INT(kz_tz_is_dst("EST5EDT,M3.2.0,M11.1.0", spring - 1), 0);
    CHECK_INT(kz_tz_is_dst("EST5EDT,M3.2.0,M11.1.0", spring), 1);

    // Tepat di kedua sisi transisi musim gugur 2026-11-01 02:00 EDT (06:00 UTC).
    int64_t fall = kz_civil_to_unix(&(kz_civil_t){ 2026, 11, 1, 6, 0, 0, 0, 0 });
    CHECK_INT(kz_tz_is_dst("EST5EDT,M3.2.0,M11.1.0", fall - 1), 1);
    CHECK_INT(kz_tz_is_dst("EST5EDT,M3.2.0,M11.1.0", fall), 0);

    // --- London: GMT0BST, DST terakhir Minggu Maret (M3.5.0/1) ---
    kz_civil_t w; memset(&w, 0, sizeof w);
    w.year = 2026; w.month = 1; w.day = 15; w.hour = 12;
    CHECK_INT(kz_tz_offset("GMT0BST,M3.5.0/1,M10.5.0/2", kz_civil_to_unix(&w)), 0);
    kz_civil_t s; memset(&s, 0, sizeof s);
    s.year = 2026; s.month = 7; s.day = 15; s.hour = 12;
    CHECK_INT(kz_tz_offset("GMT0BST,M3.5.0/1,M10.5.0/2", kz_civil_to_unix(&s)), 3600);

    // --- Belahan SELATAN: Sydney DST (Okt..Apr) membalik urutan start/end. ---
    // 2026-01-15 = DST (UTC+11); 2026-07-15 = standar (UTC+10).
    kz_civil_t sj; memset(&sj, 0, sizeof sj);
    sj.year = 2026; sj.month = 1; sj.day = 15; sj.hour = 12;
    CHECK_INT(kz_tz_is_dst("AEST-10AEDT,M10.1.0,M4.1.0/3", kz_civil_to_unix(&sj)), 1);
    CHECK_INT(kz_tz_offset("AEST-10AEDT,M10.1.0,M4.1.0/3", kz_civil_to_unix(&sj)),
              11 * 3600);
    kz_civil_t sw; memset(&sw, 0, sizeof sw);
    sw.year = 2026; sw.month = 7; sw.day = 15; sw.hour = 12;
    CHECK_INT(kz_tz_is_dst("AEST-10AEDT,M10.1.0,M4.1.0/3", kz_civil_to_unix(&sw)), 0);
    CHECK_INT(kz_tz_offset("AEST-10AEDT,M10.1.0,M4.1.0/3", kz_civil_to_unix(&sw)),
              10 * 3600);

    // Singkatan zona ikut berubah saat DST.
    char ab[KZ_TZ_ABBR_MAX];
    kz_tz_abbr("EST5EDT,M3.2.0,M11.1.0", u_jan, ab, sizeof ab);
    CHECK_STR(ab, "EST");
    kz_tz_abbr("EST5EDT,M3.2.0,M11.1.0", u_jul, ab, sizeof ab);
    CHECK_STR(ab, "EDT");
    kz_tz_abbr("WIB-7", 0, ab, sizeof ab);
    CHECK_STR(ab, "WIB");
}

// ------------------------------------------------------------
// 3b. Batas transisi DST yang PERSIS.
//
// Nilai-nilai ini diverifikasi independen terhadap basis data zona IANA
// (Python zoneinfo) untuk tahun 2026 — bukan diturunkan dari kode yang diuji.
// Kalau evaluator transisi bergeser, test ini gagal di detik yang tepat.
// ------------------------------------------------------------
static void test_dst_boundaries(void) {
    const char* ny = "EST5EDT,M3.2.0,M11.1.0";
    // 2026-03-08 07:00 UTC: 02:00 EST -> 03:00 EDT.
    CHECK_INT(kz_tz_is_dst(ny, 1772953200LL - 1), 0);
    CHECK_INT(kz_tz_is_dst(ny, 1772953200LL), 1);
    CHECK_INT(kz_tz_offset(ny, 1772953200LL - 1), -5 * 3600);
    CHECK_INT(kz_tz_offset(ny, 1772953200LL), -4 * 3600);
    // 2026-11-01 06:00 UTC: 02:00 EDT -> 01:00 EST.
    CHECK_INT(kz_tz_is_dst(ny, 1793512800LL - 1), 1);
    CHECK_INT(kz_tz_is_dst(ny, 1793512800LL), 0);
    CHECK_INT(kz_tz_offset(ny, 1793512800LL - 1), -4 * 3600);
    CHECK_INT(kz_tz_offset(ny, 1793512800LL), -5 * 3600);

    const char* lon = "GMT0BST,M3.5.0/1,M10.5.0/2";
    // 2026-03-29 01:00 UTC: 01:00 GMT -> 02:00 BST (minggu TERAKHIR Maret).
    // Transisi Eropa terjadi pada 01:00 UTC, BUKAN 02:00 — inilah sebabnya
    // aturan tanpa /jam (default 02:00 lokal) meleset satu jam.
    CHECK_INT(kz_tz_is_dst(lon, 1774746000LL - 1), 0);
    CHECK_INT(kz_tz_is_dst(lon, 1774746000LL), 1);
    CHECK_INT(kz_tz_offset(lon, 1774746000LL - 1), 0);
    CHECK_INT(kz_tz_offset(lon, 1774746000LL), 3600);
    // 2026-10-25 01:00 UTC: 02:00 BST -> 01:00 GMT.
    CHECK_INT(kz_tz_is_dst(lon, 1792890000LL - 1), 1);
    CHECK_INT(kz_tz_is_dst(lon, 1792890000LL), 0);
    CHECK_INT(kz_tz_offset(lon, 1792890000LL - 1), 3600);
    CHECK_INT(kz_tz_offset(lon, 1792890000LL), 0);

    // Berlin: transisi MUSIM GUGUR pada 03:00 lokal (01:00 UTC), bukan 02:00.
    const char* ber = "CET-1CEST,M3.5.0,M10.5.0/3";
    CHECK_INT(kz_tz_is_dst(ber, 1792890000LL - 1), 1);   // 2026-10-25 00:59:59 UTC
    CHECK_INT(kz_tz_is_dst(ber, 1792890000LL), 0);       // 01:00 UTC
    CHECK_INT(kz_tz_offset(ber, 1792890000LL - 1), 7200);
    CHECK_INT(kz_tz_offset(ber, 1792890000LL), 3600);

    // Cairo: aturan dengan jam 0 dan 24 (transisi tepat tengah malam).
    const char* cai = "EET-2EEST,M4.5.5/0,M10.5.4/24";
    CHECK_INT(kz_tz_is_dst(cai, 1776981600LL - 1), 0);
    CHECK_INT(kz_tz_is_dst(cai, 1776981600LL), 1);
    CHECK_INT(kz_tz_offset(cai, 1776981600LL), 3 * 3600);
    CHECK_INT(kz_tz_is_dst(cai, 1793307600LL - 1), 1);
    CHECK_INT(kz_tz_is_dst(cai, 1793307600LL), 0);
    CHECK_INT(kz_tz_offset(cai, 1793307600LL), 2 * 3600);

    // Transisi New York 2026: 8 Mar 07:00 UTC -> 1 Nov 06:00 UTC.
    // Rentangnya 238 hari KURANG satu jam, jadi membaginya dengan 86400
    // menghasilkan 237 — dan itu memang benar, bukan pembulatan yang bocor.
    int64_t a = 1772953200LL, b = 1793512800LL;
    CHECK_INT((b - a) / 86400, 237);
    CHECK_INT((b - a) / 3600, 238 * 24 - 1);
}

// ------------------------------------------------------------
// 4. Bug yang memicu libs/time: rollover tanggal saat offset melewati
//    tengah malam. Kernel lama hanya `%24` jam tanpa menggeser hari.
// ------------------------------------------------------------
static void test_midnight_rollover(void) {
    // 2026-09-20 20:00 UTC + 7 jam = 2026-09-21 03:00 WIB (HARI BERIKUTNYA).
    kz_civil_t utc; memset(&utc, 0, sizeof utc);
    utc.year = 2026; utc.month = 9; utc.day = 20; utc.hour = 20;
    int64_t u = kz_civil_to_unix(&utc);

    kz_civil_t local;
    kz_civil_local("WIB-7", u, &local);
    CHECK_INT(local.year, 2026);
    CHECK_INT(local.month, 9);
    CHECK_INT(local.day, 21);      // <- inilah yang dulu salah (tetap 20)
    CHECK_INT(local.hour, 3);
    CHECK_INT(local.weekday, 1);   // Senin, bukan Minggu

    // Dan arah sebaliknya: offset negatif melewati tengah malam ke belakang.
    // 2026-09-20 03:00 UTC - 5 jam = 2026-09-19 22:00 EST.
    kz_civil_t utc2; memset(&utc2, 0, sizeof utc2);
    utc2.year = 2026; utc2.month = 9; utc2.day = 20; utc2.hour = 3;
    kz_civil_local("EST5", kz_civil_to_unix(&utc2), &local);
    CHECK_INT(local.day, 19);
    CHECK_INT(local.hour, 22);
    CHECK_INT(local.weekday, 6);   // Sabtu

    // Rollover bulan dan tahun.
    kz_civil_t ny; memset(&ny, 0, sizeof ny);
    ny.year = 2026; ny.month = 12; ny.day = 31; ny.hour = 20;
    kz_civil_local("WIB-7", kz_civil_to_unix(&ny), &local);
    CHECK_INT(local.year, 2027);
    CHECK_INT(local.month, 1);
    CHECK_INT(local.day, 1);
    CHECK_INT(local.hour, 3);

    // Offset setengah jam juga harus benar (India +5:30).
    kz_civil_t h; memset(&h, 0, sizeof h);
    h.year = 2026; h.month = 9; h.day = 20; h.hour = 20; h.minute = 0;
    kz_civil_local("IST-5:30", kz_civil_to_unix(&h), &local);
    CHECK_INT(local.day, 21);
    CHECK_INT(local.hour, 1);
    CHECK_INT(local.minute, 30);
}

// ------------------------------------------------------------
// 5. Format.
// ------------------------------------------------------------
static void test_format(void) {
    char b[64];

    kz_fmt_hhmm(13, 15, 0, b, sizeof b);
    CHECK_STR(b, "13:15");
    kz_fmt_hhmm(9, 5, 0, b, sizeof b);
    CHECK_STR(b, "09:05");
    kz_fmt_hhmm(0, 0, 0, b, sizeof b);
    CHECK_STR(b, "00:00");

    // 12 jam: tengah malam = 12 AM, tengah hari = 12 PM.
    kz_fmt_hhmm(13, 15, 1, b, sizeof b);
    CHECK_STR(b, "01:15 PM");
    kz_fmt_hhmm(0, 30, 1, b, sizeof b);
    CHECK_STR(b, "12:30 AM");
    kz_fmt_hhmm(12, 0, 1, b, sizeof b);
    CHECK_STR(b, "12:00 PM");
    kz_fmt_hhmm(23, 59, 1, b, sizeof b);
    CHECK_STR(b, "11:59 PM");
    kz_fmt_hhmm(11, 59, 1, b, sizeof b);
    CHECK_STR(b, "11:59 AM");

    kz_fmt_hhmmss(13, 15, 42, 0, b, sizeof b);
    CHECK_STR(b, "13:15:42");
    kz_fmt_hhmmss(13, 15, 42, 1, b, sizeof b);
    CHECK_STR(b, "01:15:42 PM");

    // Tanggal: hari TANPA nol di depan.
    kz_civil_t t; memset(&t, 0, sizeof t);
    t.year = 2026; t.month = 10; t.day = 3; t.weekday = 6;
    kz_fmt_date_long(&t, b, sizeof b);
    CHECK_STR(b, "Saturday, October 3");
    kz_fmt_date_short(&t, b, sizeof b);
    CHECK_STR(b, "3 Oct 2026");
    kz_fmt_date_iso(&t, b, sizeof b);
    CHECK_STR(b, "2026-10-03");

    t.day = 20;
    kz_fmt_date_long(&t, b, sizeof b);
    CHECK_STR(b, "Saturday, October 20");

    // Buffer sempit tidak boleh overflow — potong, tetap NUL.
    char tiny[5];
    kz_fmt_date_long(&t, tiny, sizeof tiny);
    CHECK_INT(tiny[4], 0);
    kz_fmt_hhmm(13, 15, 1, tiny, sizeof tiny);
    CHECK_INT(tiny[4], 0);

    // Nama invalid -> string kosong, bukan out-of-bounds.
    CHECK_STR(kz_weekday_name(-1), "");
    CHECK_STR(kz_weekday_name(7), "");
    CHECK_STR(kz_month_name(0), "");
    CHECK_STR(kz_month_name(13), "");
}

// ------------------------------------------------------------
// 6. Zona tabel + config /time.ui.
// ------------------------------------------------------------
static void test_zones_and_cfg(void) {
    int n = 0;
    const kz_zone_t* t = kz_zone_table(&n);
    CHECK(t != 0);
    CHECK(n > 0);
    CHECK(n <= KZ_TZ_TABLE_MAX);

    // Setiap aturan di tabel harus bisa di-parse (kalau tidak, jam akan
    // diam-diam menampilkan UTC untuk kota itu).
    int unparsable = 0;
    for (int i = 0; i < n; i++) {
        // kz_tz_abbr mengembalikan "" bila aturan tak terbaca.
        char ab[KZ_TZ_ABBR_MAX];
        kz_tz_abbr(t[i].rule, 0, ab, sizeof ab);
        if (ab[0] == '\0') {
            printf("  unparsable zone: %s -> \"%s\"\n", t[i].id, t[i].rule);
            unparsable++;
        }
    }
    CHECK_INT(unparsable, 0);

    // Default = Jakarta (perilaku lama kernel).
    CHECK_INT(kz_zone_default(), 0);
    CHECK_STR(t[kz_zone_default()].id, "Asia/Jakarta");

    CHECK_INT(kz_zone_find("Asia/Tokyo") >= 0, 1);
    CHECK_INT(kz_zone_find("Europe/London") >= 0, 1);
    CHECK_INT(kz_zone_find("Nowhere/Nothing"), -1);
    CHECK_INT(kz_zone_find(0), -1);

    // Config round-trip.
    kz_time_cfg_t c;
    kz_time_cfg_default(&c);
    CHECK_INT(c.zone, 0);
    CHECK_INT(c.hour12, 0);

    char buf[KZ_TIME_CFG_LEN];
    c.zone = kz_zone_find("Europe/London");
    c.hour12 = 1;
    CHECK_INT(kz_time_cfg_encode(&c, buf), KZ_TIME_CFG_LEN);
    kz_time_cfg_t d;
    kz_time_cfg_decode(buf, KZ_TIME_CFG_LEN, &d);
    CHECK_INT(d.zone, c.zone);
    CHECK_INT(d.hour12, 1);

    // File rusak -> default, tidak crash.
    kz_time_cfg_decode("XXXXXX", 6, &d);
    CHECK_INT(d.zone, kz_zone_default());
    CHECK_INT(d.hour12, 0);
    kz_time_cfg_decode(0, 0, &d);
    CHECK_INT(d.zone, kz_zone_default());
    kz_time_cfg_decode(buf, 3, &d);          // panjang salah
    CHECK_INT(d.zone, kz_zone_default());
    // Zona di luar rentang.
    char bad[KZ_TIME_CFG_LEN];
    memcpy(bad, buf, KZ_TIME_CFG_LEN);
    bad[4] = (char)99;
    kz_time_cfg_decode(bad, KZ_TIME_CFG_LEN, &d);
    CHECK_INT(d.zone, kz_zone_default());

    // NULL encode memakai default.
    CHECK_INT(kz_time_cfg_encode(0, buf), KZ_TIME_CFG_LEN);
    kz_time_cfg_decode(buf, KZ_TIME_CFG_LEN, &d);
    CHECK_INT(d.zone, kz_zone_default());
}

// ------------------------------------------------------------
// 7. World Clock: perbedaan tanggal antar kota pada satu instan UTC.
// ------------------------------------------------------------
static void test_world_clock(void) {
    // 2026-09-20 20:00 UTC: Jakarta sudah besok, Los Angeles masih hari ini.
    kz_civil_t utc; memset(&utc, 0, sizeof utc);
    utc.year = 2026; utc.month = 9; utc.day = 20; utc.hour = 20;
    int64_t u = kz_civil_to_unix(&utc);

    int n = 0;
    const kz_zone_t* t = kz_zone_table(&n);

    kz_civil_t jkt;
    kz_civil_local(t[kz_zone_find("Asia/Jakarta")].rule, u, &jkt);
    CHECK_INT(jkt.day, 21);
    CHECK_INT(jkt.hour, 3);

    kz_civil_t lax;
    kz_civil_local(t[kz_zone_find("America/Los_Angeles")].rule, u, &lax);
    CHECK_INT(lax.day, 20);       // PDT = UTC-7 pada September
    CHECK_INT(lax.hour, 13);

    kz_civil_t ldn;
    kz_civil_local(t[kz_zone_find("Europe/London")].rule, u, &ldn);
    CHECK_INT(ldn.day, 20);
    CHECK_INT(ldn.hour, 21);      // BST = UTC+1

    kz_civil_t tky;
    kz_civil_local(t[kz_zone_find("Asia/Tokyo")].rule, u, &tky);
    CHECK_INT(tky.day, 21);
    CHECK_INT(tky.hour, 5);
}

int main(void) {
    printf("libs/time: civil calendar + POSIX timezone rules\n");
    test_calendar();
    test_unix();
    test_offsets();
    test_dst_boundaries();
    test_midnight_rollover();
    test_format();
    test_zones_and_cfg();
    test_world_clock();

    if (g_fail) {
        printf("FAILED: %d/%d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("OK: %d checks passed\n", g_checks);
    return 0;
}
