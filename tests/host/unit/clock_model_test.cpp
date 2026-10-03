// tests/host/unit/clock_model_test.cpp — logika waktu aplikasi Clock.
//
// KENAPA TEST INI PENTING: klaim "stopwatch tidak melenceng" adalah klaim
// tentang ARITMETIKA, bukan tentang tampilan. Test ini membuktikannya dengan
// jam palsu: waktu dimajukan dengan langkah tidak beraturan (meniru frame yang
// terlewat / sistem yang sibuk) dan hasilnya harus tetap persis.
//
// Kalau implementasinya menambah elapsed tiap frame (cara yang salah), test
// ini gagal — dan itulah gunanya.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "model.hpp"

using namespace clock_app;

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_U64(got, want) do { \
    g_checks++; \
    unsigned long long _g = (unsigned long long)(got); \
    unsigned long long _w = (unsigned long long)(want); \
    if (_g != _w) { \
        printf("FAIL %s:%d  %s = %llu, want %llu\n", \
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
// 1. Stopwatch: transisi keadaan.
// ------------------------------------------------------------
static void test_stopwatch_basic(void) {
    Stopwatch sw;
    CHECK(!sw.running());
    CHECK_U64(sw.elapsed(1000), 0);

    sw.start(1000);
    CHECK(sw.running());
    CHECK_U64(sw.elapsed(1000), 0);
    CHECK_U64(sw.elapsed(1500), 500);

    sw.pause(2000);
    CHECK(!sw.running());
    CHECK_U64(sw.elapsed(2000), 1000);
    // Waktu terus berjalan di luar, tapi stopwatch TIDAK boleh ikut jalan.
    CHECK_U64(sw.elapsed(99999), 1000);

    sw.start(10000);
    CHECK_U64(sw.elapsed(10500), 1500);   // 1000 tersimpan + 500 baru

    sw.reset();
    CHECK(!sw.running());
    CHECK_U64(sw.elapsed(20000), 0);
}

// ------------------------------------------------------------
// 2. Stopwatch: idempoten dan tahan panggilan berulang.
// ------------------------------------------------------------
static void test_stopwatch_idempotent(void) {
    Stopwatch sw;
    sw.start(100);
    sw.start(200);        // sudah berjalan -> tidak menggeser titik mulai
    CHECK_U64(sw.elapsed(300), 200);

    sw.pause(300);
    sw.pause(400);        // sudah jeda -> tidak menambah akumulasi
    CHECK_U64(sw.elapsed(500), 200);

    sw.reset();
    sw.reset();
    CHECK_U64(sw.elapsed(600), 0);
}

// ------------------------------------------------------------
// 3. INTI: tidak melenceng saat langkah waktu tidak beraturan.
//
// Inilah alasan model memakai timestamp, bukan penambahan per frame.
// ------------------------------------------------------------
static void test_stopwatch_no_drift(void) {
    Stopwatch sw;
    const uint64_t t0 = 5000;
    sw.start(t0);

    // 10.000 "frame" dengan langkah yang sengaja tidak rata: 1..7 ms.
    uint64_t now = t0;
    for (int i = 0; i < 10000; i++) {
        now += (uint64_t)(i % 7) + 1;
        (void)sw.elapsed(now);   // pembacaan tidak boleh mengubah apa pun
    }
    // Total langkah = jumlah (i%7)+1 untuk i=0..9999.
    // Tiap siklus 7 nilai berjumlah 1+2+3+4+5+6+7 = 28; 10000/7 = 1428 siklus
    // (9996 langkah) + 4 sisa (1+2+3+4 = 10) -> 1428*28 + 10 = 39994.
    CHECK_U64(now - t0, 39994);
    CHECK_U64(sw.elapsed(now), 39994);

    // Melompati waktu yang sangat besar (mis. mesin tersleep) tetap eksak.
    CHECK_U64(sw.elapsed(t0 + 3600ULL * 1000), 3600ULL * 1000);

    // Dan tidak pernah mundur walau waktu dibaca dengan urutan aneh.
    CHECK_U64(sw.elapsed(t0 + 10), 10);
    CHECK_U64(sw.elapsed(t0 + 20), 20);
}

// ------------------------------------------------------------
// 4. Putaran: total dan selisih.
// ------------------------------------------------------------
static void test_stopwatch_laps(void) {
    Stopwatch sw;
    sw.start(0);

    sw.lap(1000);   // putaran 1 = 1000
    sw.lap(2500);   // putaran 2 = 2500 (delta 1500)
    sw.lap(4000);   // putaran 3 = 4000 (delta 1500)

    CHECK_U64(sw.lap_count(), 3);
    CHECK_U64(sw.last_lap_total(), 4000);
    // i=0 terbaru.
    CHECK_U64(sw.lap_total(0), 4000);
    CHECK_U64(sw.lap_total(1), 2500);
    CHECK_U64(sw.lap_total(2), 1000);
    CHECK_U64(sw.lap_delta(0), 1500);
    CHECK_U64(sw.lap_delta(1), 1500);
    CHECK_U64(sw.lap_delta(2), 1000);   // putaran pertama = dari nol
    // Di luar rentang: 0, bukan sampah memori.
    CHECK_U64(sw.lap_total(3), 0);
    CHECK_U64(sw.lap_total(-1), 0);

    // Putaran saat jeda diabaikan.
    sw.pause(5000);
    sw.lap(6000);
    CHECK_U64(sw.lap_count(), 3);

    sw.reset();
    CHECK_U64(sw.lap_count(), 0);
    CHECK_U64(sw.last_lap_total(), 0);
}

// ------------------------------------------------------------
// 5. Putaran: melebihi kapasitas -> yang tertua dibuang, bukan korupsi.
// ------------------------------------------------------------
static void test_stopwatch_lap_ring(void) {
    Stopwatch sw;
    sw.start(0);
    for (int i = 1; i <= Stopwatch::MAX_LAPS + 10; i++)
        sw.lap((uint64_t)i * 100);

    CHECK_U64(sw.lap_count(), Stopwatch::MAX_LAPS);
    // Yang terbaru = putaran ke-74 pada t=7400.
    CHECK_U64(sw.lap_total(0), (uint64_t)(Stopwatch::MAX_LAPS + 10) * 100);
    // Yang tertua yang TERSIMPAN = putaran ke-11 pada t=1100.
    CHECK_U64(sw.lap_total(Stopwatch::MAX_LAPS - 1), 1100);
}

// ------------------------------------------------------------
// 6. Timer: keadaan dan sisa.
// ------------------------------------------------------------
static void test_timer_basic(void) {
    Timer t;
    t.set_duration_ms(60000);
    CHECK(t.state() == Timer::IDLE);
    CHECK_U64(t.remaining(0), 60000);

    t.start(1000);
    CHECK(t.state() == Timer::RUNNING);
    CHECK_U64(t.remaining(1000), 60000);
    CHECK_U64(t.remaining(11000), 50000);

    t.pause(11000);
    CHECK(t.state() == Timer::PAUSED);
    CHECK_U64(t.remaining(99000), 50000);   // jeda membekukan sisa

    t.resume(20000);
    CHECK(t.state() == Timer::RUNNING);
    CHECK_U64(t.remaining(20000), 50000);
    CHECK_U64(t.remaining(30000), 40000);

    t.reset();
    CHECK(t.state() == Timer::IDLE);
    CHECK_U64(t.remaining(40000), 60000);
}

// ------------------------------------------------------------
// 7. Timer: selesai dilaporkan TEPAT SEKALI.
// ------------------------------------------------------------
static void test_timer_completion(void) {
    Timer t;
    t.set_duration_ms(1000);
    t.start(0);

    CHECK(!t.tick(500));        // belum habis
    CHECK(t.state() == Timer::RUNNING);
    CHECK_U64(t.remaining(500), 500);

    CHECK(t.tick(1000));        // tepat habis -> lapor
    CHECK(t.state() == Timer::DONE);
    CHECK_U64(t.remaining(1000), 0);

    // Sudah DONE: tidak lapor lagi walau tick terus dipanggil.
    CHECK(!t.tick(1100));
    CHECK(!t.tick(2000));
    CHECK(!t.tick(99999));

    // Lewat jauh melewati target tetap berhenti di nol (tidak negatif).
    Timer t2;
    t2.set_duration_ms(1000);
    t2.start(0);
    CHECK(t2.tick(500000));
    CHECK_U64(t2.remaining(500000), 0);
    CHECK(t2.state() == Timer::DONE);
}

// ------------------------------------------------------------
// 8. Timer: TIDAK melenceng (inti yang sama dengan stopwatch).
// ------------------------------------------------------------
static void test_timer_no_drift(void) {
    Timer t;
    t.set_duration_ms(600000);   // 10 menit
    t.start(0);
    uint64_t now = 0;
    for (int i = 0; i < 20000; i++) {
        now += (uint64_t)(i % 5) + 1;
        (void)t.remaining(now);
        (void)t.progress_permil(now);
    }
    // Setelah semua langkah itu, sisa harus persis durasi - now.
    CHECK_U64(t.remaining(now), 600000 - now);

    // Jeda lalu lanjut: total waktu berjalan tetap terjaga.
    Timer t2;
    t2.set_duration_ms(10000);
    t2.start(0);
    t2.pause(3000);          // 3000 terpakai
    CHECK_U64(t2.remaining(99999), 7000);
    t2.resume(50000);
    CHECK_U64(t2.remaining(50000), 7000);
    CHECK_U64(t2.remaining(52000), 5000);
    CHECK(t2.tick(57000));
    CHECK_U64(t2.remaining(57000), 0);
}

// ------------------------------------------------------------
// 9. Timer: batas durasi + tombol -/+.
// ------------------------------------------------------------
static void test_timer_duration(void) {
    Timer t;

    t.set_duration_ms(0);
    CHECK_U64(t.duration_ms(), Timer::MIN_MS);      // dijepit ke 1 detik

    t.set_duration_ms(999999999ULL);
    CHECK_U64(t.duration_ms(), Timer::MAX_MS);      // dijepit ke 24 jam

    t.set_duration_ms(5 * 60 * 1000);               // 5 menit
    t.adjust_minutes(1);
    CHECK_U64(t.duration_ms(), 6 * 60 * 1000);
    t.adjust_minutes(-1);
    CHECK_U64(t.duration_ms(), 5 * 60 * 1000);

    // Tidak bisa turun di bawah minimum.
    for (int i = 0; i < 20; i++) t.adjust_minutes(-1);
    CHECK_U64(t.duration_ms(), Timer::MIN_MS);

    // Tidak bisa naik di atas maksimum.
    for (int i = 0; i < 2000; i++) t.adjust_minutes(1);
    CHECK_U64(t.duration_ms(), Timer::MAX_MS);

    // Mengubah durasi saat berjalan membatalkan hitungan.
    t.set_duration_ms(60000);
    t.start(0);
    CHECK(t.state() == Timer::RUNNING);
    t.adjust_minutes(1);
    CHECK(t.state() == Timer::IDLE);
    CHECK_U64(t.remaining(0), 120000);
}

// ------------------------------------------------------------
// 10. Timer: progres per-mil monoton dan berakhir di 1000.
// ------------------------------------------------------------
static void test_timer_progress(void) {
    Timer t;
    t.set_duration_ms(10000);
    t.start(0);

    CHECK_U64(t.progress_permil(0), 0);
    CHECK_U64(t.progress_permil(1000), 100);
    CHECK_U64(t.progress_permil(5000), 500);
    CHECK_U64(t.progress_permil(9000), 900);

    // Monoton naik sepanjang waktu.
    int prev = -1;
    int nonmono = 0;
    for (uint64_t ms = 0; ms <= 10000; ms += 37) {
        int p = t.progress_permil(ms);
        if (p < prev) nonmono++;
        prev = p;
    }
    CHECK_U64(nonmono, 0);

    // Saat DONE selalu 1000.
    t.tick(10000);
    CHECK(t.state() == Timer::DONE);
    CHECK_U64(t.progress_permil(10000), 1000);
    CHECK_U64(t.progress_permil(20000), 1000);
}

// ------------------------------------------------------------
// 11. Format.
// ------------------------------------------------------------
static void test_format(void) {
    char b[32];

    fmt_elapsed(0, b, sizeof b);
    CHECK_STR(b, "00:00.000");
    fmt_elapsed(381, b, sizeof b);
    CHECK_STR(b, "00:00.381");
    fmt_elapsed(42381, b, sizeof b);
    CHECK_STR(b, "00:42.381");
    fmt_elapsed(60000, b, sizeof b);
    CHECK_STR(b, "01:00.000");
    fmt_elapsed(3599999, b, sizeof b);
    CHECK_STR(b, "59:59.999");
    // Di atas satu jam: jam ikut ditampilkan.
    fmt_elapsed(3600000, b, sizeof b);
    CHECK_STR(b, "1:00:00.000");
    fmt_elapsed(3661234, b, sizeof b);
    CHECK_STR(b, "1:01:01.234");

    fmt_mmss(0, b, sizeof b);
    CHECK_STR(b, "00:00");
    fmt_mmss(1000, b, sizeof b);
    CHECK_STR(b, "00:01");
    fmt_mmss(300000, b, sizeof b);
    CHECK_STR(b, "05:00");
    fmt_mmss(3661000, b, sizeof b);
    CHECK_STR(b, "1:01:01");

    fmt_duration(300000, b, sizeof b);
    CHECK_STR(b, "05:00");
    fmt_duration(5000, b, sizeof b);
    CHECK_STR(b, "00:05");
    fmt_duration(3600000, b, sizeof b);
    CHECK_STR(b, "1:00:00");

    // Buffer sempit: terpotong, tetap NUL, tidak menulis lewat batas.
    char tiny[6];
    memset(tiny, 'X', sizeof tiny);
    fmt_elapsed(42381, tiny, sizeof tiny);
    CHECK_U64(tiny[5], 0);
    fmt_mmss(300000, tiny, sizeof tiny);
    CHECK_U64(tiny[5], 0);

    // NULL tidak crash.
    fmt_elapsed(1000, 0, 10);
    fmt_mmss(1000, 0, 10);
    fmt_duration(1000, 0, 10);
}

int main(void) {
    printf("clock model: stopwatch/timer state machines\n");
    test_stopwatch_basic();
    test_stopwatch_idempotent();
    test_stopwatch_no_drift();
    test_stopwatch_laps();
    test_stopwatch_lap_ring();
    test_timer_basic();
    test_timer_completion();
    test_timer_no_drift();
    test_timer_duration();
    test_timer_progress();
    test_format();

    if (g_fail) {
        printf("FAILED: %d/%d checks failed\n", g_fail, g_checks);
        return 1;
    }
    printf("OK: %d checks passed\n", g_checks);
    return 0;
}
