// apps/clock/model.cpp — implementasi state machine waktu (lihat model.hpp).
#include "model.hpp"

namespace clock_app {

// ------------------------------------------------------------
// Stopwatch
// ------------------------------------------------------------
Stopwatch::Stopwatch()
    : running_(false), accum_ms_(0), start_ms_(0), lap_n_(0), lap_head_(0) {}

void Stopwatch::start(uint64_t now_ms) {
    if (running_) return;
    running_ = true;
    start_ms_ = now_ms;
}

void Stopwatch::pause(uint64_t now_ms) {
    if (!running_) return;
    // Akumulasi SELISIH, bukan menyimpan `now` sebagai titik mulai baru:
    // dengan begitu rumus elapsed() tetap satu bentuk.
    accum_ms_ += now_ms - start_ms_;
    running_ = false;
}

void Stopwatch::toggle(uint64_t now_ms) {
    if (running_) pause(now_ms);
    else start(now_ms);
}

void Stopwatch::reset() {
    running_ = false;
    accum_ms_ = 0;
    start_ms_ = 0;
    lap_n_ = 0;
    lap_head_ = 0;
}

uint64_t Stopwatch::elapsed(uint64_t now_ms) const {
    if (!running_) return accum_ms_;
    return accum_ms_ + (now_ms - start_ms_);
}

void Stopwatch::lap(uint64_t now_ms) {
    // Putaran hanya bermakna saat berjalan: mencatat saat jeda akan
    // menghasilkan putaran bernilai nol yang membingungkan.
    if (!running_) return;
    lap_head_ = (lap_head_ + 1) % MAX_LAPS;
    laps_[lap_head_] = elapsed(now_ms);
    if (lap_n_ < MAX_LAPS) lap_n_++;
}

uint64_t Stopwatch::last_lap_total() const {
    if (lap_n_ == 0) return 0;
    return laps_[lap_head_];
}

uint64_t Stopwatch::lap_total(int i) const {
    if (i < 0 || i >= lap_n_) return 0;
    int idx = lap_head_ - i;
    while (idx < 0) idx += MAX_LAPS;
    return laps_[idx];
}

uint64_t Stopwatch::lap_delta(int i) const {
    if (i < 0 || i >= lap_n_) return 0;
    uint64_t total = lap_total(i);
    // Putaran terbaru: selisihnya terhadap putaran sebelumnya.
    // Putaran tertua yang tersimpan: selisihnya terhadap nol.
    if (i == lap_n_ - 1) return total;
    return total - lap_total(i + 1);
}

// ------------------------------------------------------------
// Timer
// ------------------------------------------------------------
Timer::Timer()
    : state_(IDLE), duration_ms_(5 * 60 * 1000), accum_ms_(0), start_ms_(0),
      done_reported_(false) {}

void Timer::set_duration_ms(uint64_t ms) {
    if (ms < MIN_MS) ms = MIN_MS;
    if (ms > MAX_MS) ms = MAX_MS;
    duration_ms_ = ms;
    // Mengubah durasi membatalkan hitungan yang sedang berjalan: melanjutkan
    // dengan target lama sambil menampilkan durasi baru akan menyesatkan.
    reset();
}

void Timer::adjust_minutes(int delta_min) {
    if (delta_min == 0) return;
    int64_t ms = (int64_t)duration_ms_ + (int64_t)delta_min * (int64_t)STEP_MS;
    if (ms < (int64_t)MIN_MS) ms = (int64_t)MIN_MS;
    if (ms > (int64_t)MAX_MS) ms = (int64_t)MAX_MS;
    set_duration_ms((uint64_t)ms);
}

void Timer::start(uint64_t now_ms) {
    state_ = RUNNING;
    accum_ms_ = 0;
    start_ms_ = now_ms;
    done_reported_ = false;
}

void Timer::pause(uint64_t now_ms) {
    if (state_ != RUNNING) return;
    accum_ms_ += now_ms - start_ms_;
    state_ = PAUSED;
}

void Timer::resume(uint64_t now_ms) {
    if (state_ != PAUSED) return;
    start_ms_ = now_ms;
    state_ = RUNNING;
}

void Timer::reset() {
    state_ = IDLE;
    accum_ms_ = 0;
    start_ms_ = 0;
    done_reported_ = false;
}

uint64_t Timer::remaining(uint64_t now_ms) const {
    if (state_ == IDLE) return duration_ms_;
    uint64_t used = accum_ms_;
    if (state_ == RUNNING) used += now_ms - start_ms_;
    if (used >= duration_ms_) return 0;
    return duration_ms_ - used;
}

bool Timer::tick(uint64_t now_ms) {
    if (state_ != RUNNING) return false;
    if (remaining(now_ms) > 0) return false;
    // Habis: berhenti di DONE dan lapor SEKALI. Melaporkan tiap tick akan
    // memicu notifikasi berulang tiap frame.
    state_ = DONE;
    accum_ms_ = duration_ms_;
    if (done_reported_) return false;
    done_reported_ = true;
    return true;
}

int Timer::progress_permil(uint64_t now_ms) const {
    if (duration_ms_ == 0) return 1000;
    if (state_ == DONE) return 1000;
    uint64_t rem = remaining(now_ms);
    uint64_t done = duration_ms_ - rem;
    // Per-mil, dibulatkan ke bawah: busur tidak pernah mendahului waktu.
    uint64_t p = done * 1000ULL / duration_ms_;
    if (p > 1000) p = 1000;
    return (int)p;
}

// ------------------------------------------------------------
// Format
// ------------------------------------------------------------
namespace {

void put_ch(char* out, int cap, int& k, char c) {
    if (k < cap - 1) out[k++] = c;
}
void put_n(char* out, int cap, int& k, uint64_t v, int digits) {
    for (int d = digits - 1; d >= 0; d--) {
        uint64_t div = 1;
        for (int i = 0; i < d; i++) div *= 10;
        put_ch(out, cap, k, (char)('0' + (int)((v / div) % 10)));
    }
}
void finish(char* out, int cap, int k) {
    if (cap > 0) out[k < cap ? k : cap - 1] = '\0';
}

}  // namespace

void fmt_elapsed(uint64_t ms, char* out, int cap) {
    if (!out || cap <= 0) return;
    uint64_t total_s = ms / 1000;
    uint64_t h = total_s / 3600;
    uint64_t m = (total_s / 60) % 60;
    uint64_t s = total_s % 60;
    uint64_t msec = ms % 1000;
    int k = 0;
    if (h > 0) {
        put_n(out, cap, k, h, (h >= 10) ? 2 : 1);
        put_ch(out, cap, k, ':');
    }
    put_n(out, cap, k, m, 2);
    put_ch(out, cap, k, ':');
    put_n(out, cap, k, s, 2);
    put_ch(out, cap, k, '.');
    put_n(out, cap, k, msec, 3);
    finish(out, cap, k);
}

void fmt_mmss(uint64_t ms, char* out, int cap) {
    if (!out || cap <= 0) return;
    uint64_t total_s = (ms + 999) / 1000;   // dibulatkan ke atas untuk sisa
    uint64_t h = total_s / 3600;
    uint64_t m = (total_s / 60) % 60;
    uint64_t s = total_s % 60;
    int k = 0;
    if (h > 0) {
        put_n(out, cap, k, h, (h >= 10) ? 2 : 1);
        put_ch(out, cap, k, ':');
    }
    put_n(out, cap, k, m, 2);
    put_ch(out, cap, k, ':');
    put_n(out, cap, k, s, 2);
    finish(out, cap, k);
}

void fmt_duration(uint64_t ms, char* out, int cap) {
    if (!out || cap <= 0) return;
    uint64_t total_s = (ms + 500) / 1000;   // pembulatan terdekat
    uint64_t h = total_s / 3600;
    uint64_t m = (total_s / 60) % 60;
    uint64_t s = total_s % 60;
    int k = 0;
    if (h > 0) {
        put_n(out, cap, k, h, (h >= 10) ? 2 : 1);
        put_ch(out, cap, k, ':');
    }
    put_n(out, cap, k, m, 2);
    put_ch(out, cap, k, ':');
    put_n(out, cap, k, s, 2);
    finish(out, cap, k);
}

}  // namespace clock_app
