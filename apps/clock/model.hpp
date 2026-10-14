// apps/clock/model.hpp — logika waktu aplikasi Clock (murni, tanpa UI).
//
// KENAPA DIPISAH DARI UI: stopwatch dan timer adalah STATE MACHINE, bukan
// gambar. Memisahkannya membuat keduanya bisa diuji di host tanpa window,
// tanpa QEMU, dan tanpa jam sistem — dan yang paling penting, membuat
// kebenaran "tidak melenceng" (no drift) bisa DIBUKTIKAN, bukan diklaim.
//
// ATURAN WAKTU (satu-satunya yang benar di sini):
//   * Selalu dari TIMESTAMP, bukan penjumlahan per frame.
//     elapsed = akumulasi + (sekarang - timestamp_mulai)
//     Bukan: elapsed++ tiap frame — itu melenceng begitu satu frame terlewat.
//   * Semua nilai milidetik, integer 64-bit. Tidak ada float di sini karena
//     durasi adalah bilangan bulat — bukan karena larangan build. (Dulu
//     memang larangan: -mno-sse -msoft-float; sejak Phase 3 userspace boleh
//     memakai FP, dan file ini tetap integer karena itu yang benar untuk
//     waktu.)
//   * Sumber waktu monotonic (sys_uptime) diberikan PEMANGGIL, tidak dibaca di
//     sini — itu yang membuat kelas ini bisa diuji dengan waktu palsu.
//
// Waktu dinding (RTC) sengaja TIDAK ada di sini: stopwatch/timer tidak boleh
// memakai jam dinding, karena jam itu bisa diubah pengguna/NTP dan durasi akan
// ikut melompat. Pemisahan itu ada di services/.
#ifndef CLOCK_MODEL_HPP
#define CLOCK_MODEL_HPP

#include <stdint.h>

namespace clock_app {

// ------------------------------------------------------------
// Stopwatch. Keadaan: berhenti -> berjalan <-> jeda, dengan akumulasi.
// ------------------------------------------------------------
class Stopwatch {
public:
    Stopwatch();

    void start(uint64_t now_ms);    // berhenti/jeda -> berjalan
    void pause(uint64_t now_ms);    // berjalan -> jeda
    void toggle(uint64_t now_ms);   // start bila diam, jeda bila berjalan
    void reset();                   // kembali ke nol, hapus putaran
    void lap(uint64_t now_ms);      // catat putaran (hanya saat berjalan)

    bool     running() const { return running_; }
    uint64_t elapsed(uint64_t now_ms) const;

    // Putaran terakhir yang tercatat (0 bila belum ada).
    uint64_t last_lap_total() const;

    // Jumlah putaran tersimpan (dibatasi MAX_LAPS; yang tertua dibuang).
    int lap_count() const { return lap_n_; }
    // Putaran ke-`i` dari YANG TERBARU (i=0 terbaru). Return 0 bila di luar.
    uint64_t lap_total(int i) const;
    // Selisih putaran ke-`i` dari yang terbaru (durasi putaran itu sendiri).
    uint64_t lap_delta(int i) const;

    enum { MAX_LAPS = 64 };

private:
    bool     running_;
    uint64_t accum_ms_;   // total saat terakhir dijeda
    uint64_t start_ms_;   // timestamp saat mulai berjalan
    // Putaran disimpan sebagai TOTAL pada saat tombol putaran ditekan.
    // Delta dihitung saat ditampilkan, bukan disimpan: itu satu sumber
    // kebenaran, dan tidak bisa menjadi tidak konsisten satu sama lain.
    uint64_t laps_[MAX_LAPS];
    int      lap_n_;      // jumlah terpakai
    int      lap_head_;   // indeks entri TERBARU
};

// ------------------------------------------------------------
// Timer hitung mundur.
//
// Sisa SELALU dihitung dari target, bukan dikurangi tiap frame:
//   sisa = durasi - (akumulasi + (sekarang - timestamp_mulai))
// ------------------------------------------------------------
class Timer {
public:
    enum State { IDLE, RUNNING, PAUSED, DONE };

    Timer();

    // Setel durasi (dijepit 1 detik..24 jam). Mereset ke IDLE.
    void set_duration_ms(uint64_t ms);
    uint64_t duration_ms() const { return duration_ms_; }

    // Penyesuaian cepat (tombol -/+). Langkah 1 menit, minimal 1 menit.
    void adjust_minutes(int delta_min);

    void start(uint64_t now_ms);
    void pause(uint64_t now_ms);
    void resume(uint64_t now_ms);
    void reset();
    // Dipanggil tiap tick: memindahkan RUNNING -> DONE saat sisa habis.
    // Return true tepat SEKALI saat transisi itu terjadi (dipakai pemanggil
    // untuk memberi tahu pengguna, bukan tiap frame setelahnya).
    bool tick(uint64_t now_ms);

    State    state() const { return state_; }
    bool     running() const { return state_ == RUNNING; }
    uint64_t remaining(uint64_t now_ms) const;
    // Progres 0..1000 (per-mil). 1000 = selesai. Dipakai busur melingkar;
    // per-mil karena persen bulat membuat busur tersendat pada durasi panjang.
    int progress_permil(uint64_t now_ms) const;

    enum {
        MIN_MS = 1000,              // 1 detik
        MAX_MS = 24ULL * 3600 * 1000,  // 24 jam
        STEP_MS = 60 * 1000         // langkah tombol -/+
    };

private:
    State    state_;
    uint64_t duration_ms_;
    uint64_t accum_ms_;   // waktu berjalan terakumulasi saat dijeda
    uint64_t start_ms_;   // timestamp saat mulai berjalan
    bool     done_reported_;
};

// ------------------------------------------------------------
// Format (buffer tetap, tanpa alokasi — aman dipanggil tiap frame).
// ------------------------------------------------------------

// "MM:SS.mmm" (atau "H:MM:SS.mmm" bila >= 1 jam). Cocok untuk stopwatch.
void fmt_elapsed(uint64_t ms, char* out, int cap);
// "MM:SS" — untuk sisa waktu timer dan daftar putaran.
void fmt_mmss(uint64_t ms, char* out, int cap);
// "H:MM:SS" atau "MM:SS" — durasi timer tanpa milidetik.
void fmt_duration(uint64_t ms, char* out, int cap);

}  // namespace clock_app

#endif // CLOCK_MODEL_HPP
