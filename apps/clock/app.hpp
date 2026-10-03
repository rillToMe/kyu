// apps/clock/app.hpp — aplikasi Clock.
//
// ARSITEKTUR (UI-13.1, dan aturan pemisahan di RULES.md):
//
//   ui/xml/clock.xml   struktur + semantik (halaman, kontrol, id)
//        v
//   ClockApp           satu pemilik window; menghubungkan callback by id
//        v
//   model.hpp          state machine stopwatch/timer (murni, teruji host)
//        v
//   libs/time          kalender + zona waktu (murni)
//        v
//   libui              widget, tema, layout, event
//
// Widget TIDAK pernah memanggil syscall (UI-13.3): waktu masuk lewat tick(),
// dan muka jam / busur progres menerima nilainya dari sini.
//
// TICK = SATU-SATUNYA SUMBER PEMBARUAN. Setiap nilai yang ditampilkan dihitung
// ulang di tick(), lalu HANYA yang benar-benar berubah yang ditandai kotor.
// Ini yang membuat jam tidak menggambar ulang seluruh window tiap detik.
#ifndef CLOCK_APP_HPP
#define CLOCK_APP_HPP

#include <stdint.h>

#include "platform.hpp"   // header C + ABI (extern "C" benar)

// Header toolkit: aplikasi ini menggambar teksnya sendiri lewat FtText, jadi
// ia perlu membaca TOKEN TEMA (warna teks/latar) — bukan memilih warnanya.
// Itu justru yang diwajibkan UI-2.1: nilai visual datang dari token, dan satu-
// satunya cara sah untuk membacanya adalah lewat ui::Theme.
#include "core/theme.hpp"
#include "window/window.hpp"
#include "containers/tab.hpp"   // ui::Tab: panel per halaman
#include "services/uifont.hpp"

#include "model.hpp"
#include "bigtime.hpp"

namespace clock_app {

// Halaman. Urutan = urutan tab.
enum class Page : uint8_t {
    Home = 0,
    World,
    Stopwatch,
    Timer,
    Count
};
constexpr int kPageCount = static_cast<int>(Page::Count);

// Kota di World Clock. Offset TIDAK disimpan di sini: diambil dari tabel zona
// libs/time supaya hanya ada satu sumber aturan waktu (UI-2.1).
struct City {
    const char* zone_id;   // kunci ke kz_zone_table()
    int         row;       // baris di listview (untuk binding)
};

class ClockApp {
public:
    ClockApp();
    ~ClockApp();

    int run();

    // --- dipanggil thunk C / callback widget ---
    void onPageChanged();
    void onStopwatchToggle();
    void onStopwatchLap();
    void onStopwatchReset();
    void onTimerToggle();
    void onTimerReset();
    void onTimerPlus();
    void onTimerMinus();
    void onWorldChanged();
    void onToggleSeconds();
    void onQuit();
    void tick();

private:
    ui_window_t* win_;
    ui_xml_ctx_t* xml_;

    // Halaman (panel Tab dimiliki Tab; pointer di sini non-owning).
    ui_widget_t* tab_;
    ui_widget_t* pages_[kPageCount];

    // --- Home ---
    ui_widget_t* greeting_;
    ui_widget_t* time_main_;    // FtText (font hero milik aplikasi)
    ui_widget_t* time_sec_;     // FtText (teks pendukung)
    ui_widget_t* date_line_;
    ui_widget_t* zone_line_;
    ui_widget_t* analog_;       // AnalogClock
    ui_widget_t* analog_caption_;
    ui_widget_t* sec_switch_;   // Switch: tampilkan jarum detik

    // --- World ---
    ui_widget_t* world_list_;
    ui_widget_t* world_hint_;

    // --- Stopwatch ---
    ui_widget_t* sw_display_;   // FtText (waktu besar)
    ui_widget_t* sw_state_;
    ui_widget_t* sw_start_;
    ui_widget_t* sw_lap_;
    ui_widget_t* sw_reset_;
    ui_widget_t* sw_laps_;

    // --- Timer ---
    ui_widget_t* tm_display_;   // FtText
    ui_widget_t* tm_state_;
    ui_widget_t* tm_ring_;      // CircularProgress
    ui_widget_t* tm_start_;
    ui_widget_t* tm_reset_;
    ui_widget_t* tm_plus_;
    ui_widget_t* tm_minus_;

    ui_widget_t* status_;

    // --- state ---
    Page     page_;
    int      zone_;         // index zona aktif (kz_zone_table)
    int      hour12_;       // 0 = 24 jam
    int      show_seconds_;

    Stopwatch sw_;
    Timer     tm_;
    BigFont   font_;

    // Nilai terakhir yang DIGAMBAR, per elemen. Tick membandingkan terhadap
    // ini supaya hanya elemen yang berubah yang ditandai kotor (UI-20.1).
    struct Drawn {
        int year, month, day, weekday;
        int hour, minute, second, hour12;
        char time_main[16];
        char time_sec[8];
        char date_line[48];
        char zone_line[32];
        char sw_display[24];
        uint64_t sw_ms;
        bool sw_running;
        char tm_display[16];
        // Keadaan timer DISIMPAN SEBAGAI ENUM, bukan teks: yang perlu
        // dibandingkan adalah keadaan state machine-nya, dan membandingkan
        // teks akan membuat "Paused" -> "Paused" tetap dianggap berubah.
        int tm_state;
        int tm_permil;
        uint64_t tm_rem_s;
        int sw_laps;
        bool valid;
    } drawn_;

    // Ukuran hero (dihitung saat font dimuat).
    int hero_w_, hero_h_, sub_w_, sub_h_;

    bool build();
    bool buildHome(ui_widget_t* panel);
    bool buildWorld(ui_widget_t* panel);
    bool buildStopwatch(ui_widget_t* panel);
    bool buildTimer(ui_widget_t* panel);
    void loadConfig();
    void syncVisible();

    // Warna untuk FtText, dibaca dari TOKEN TEMA (UI-2.1/UI-6.4), bukan
    // dipilih sendiri. FtText menggambar di atas latar halaman, jadi bg =
    // surface halaman (theme.bg) — itulah yang membuat komposit idempoten.
    color_t bg_for_fttext() const;
    color_t fg_primary() const;
    color_t fg_secondary() const;

    // Ambil waktu lokal sekarang (UTC dari RTC + aturan zona).
    void nowLocal(kz_civil_t* out, uint64_t* ms_frac) const;
    const char* zoneRule() const;
    void zoneLabel(int64_t unix_sec, char* out, int cap) const;

    // Callback penggambaran FtText.
    static void drawMainTime(void* ud, uint32_t* canvas, int cw, int ch,
                             int x, int y);
    static void drawSubTime(void* ud, uint32_t* canvas, int cw, int ch,
                            int x, int y);
    static void drawStopwatch(void* ud, uint32_t* canvas, int cw, int ch,
                              int x, int y);
    static void drawTimer(void* ud, uint32_t* canvas, int cw, int ch,
                          int x, int y);
};

}  // namespace clock_app

#endif // CLOCK_APP_HPP
