// apps/clock/app.cpp — implementasi aplikasi Clock.
//
// Alur: XML -> struktur; build*() menyuntikkan widget kustom ke slot ber-id;
// tick() adalah SATU-SATUNYA tempat nilai diperbarui, dan hanya yang berubah
// ditandai kotor.
#include "app.hpp"

#include "ui_xml_data.h"   // generated: ui_xml_clock[+_len]

namespace clock_app {

namespace {

// ------------------------------------------------------------
// Kota World Clock. Offset diambil dari tabel zona libs/time, jadi tidak ada
// daftar offset kedua yang bisa menyimpang dari aturan waktu.
// ------------------------------------------------------------
struct CityDef { const char* zone; const char* name; };

const CityDef kCities[] = {
    { "Asia/Jakarta",      "Jakarta" },
    { "Asia/Makassar",     "Makassar" },
    { "Asia/Jayapura",     "Jayapura" },
    { "Asia/Tokyo",        "Tokyo" },
    { "Asia/Shanghai",     "Shanghai" },
    { "Asia/Kolkata",      "New Delhi" },
    { "Asia/Dubai",        "Dubai" },
    { "Europe/Moscow",     "Moscow" },
    { "Europe/Berlin",     "Berlin" },
    { "Europe/London",     "London" },
    { "UTC",               "UTC" },
    { "America/New_York",  "New York" },
    { "America/Los_Angeles", "Los Angeles" },
    { "America/Sao_Paulo", "Sao Paulo" },
    { "Africa/Cairo",      "Cairo" },
    { "Australia/Sydney",  "Sydney" },
    { "Pacific/Auckland",  "Auckland" },
};
constexpr int kCityCount = (int)(sizeof(kCities) / sizeof(kCities[0]));

// Ukuran viewport yang di-set native setelah inflasi (bergantung ukuran
// window). Nilai ini bukan token visual: ia mengisi RUANG yang tersedia,
// bukan memilih jarak/warna.
//
// Jendela 520x680, bukan 940x640: toolkit menata anak dari kiri-atas dan tidak
// meregangkan (VBox/HBox hanya menempatkan), jadi komposisi compact terbaca
// sebagai satu kolom. Jendela yang jauh lebih lebar dari isinya menyisakan
// ruang mati di kanan dan membuat waktu tidak lagi mendominasi. 520 px cukup
// untuk teks 64 px + muka jam 176 px, dan tetap nyaman di layar 1280x720
// (UI-12.4).
constexpr int kWindowW = 520;
constexpr int kWindowH = 700;
constexpr int kAnalogSize = 168;
constexpr int kRingSize = 128;

// Nama hari/bulan dari libs/time, tapi kapitalisasi judul ("Saturday") sudah
// benar di sana; helper di sini hanya menyusun kalimatnya.

// Copy string dengan batas.
void scopy(char* dst, int cap, const char* src) {
    if (!dst || cap <= 0) return;
    int i = 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Bandingkan; 0 = sama. Dipakai tick() supaya teks yang tidak berubah tidak
// di-set ulang (set_text selalu menandai kotor).
bool same(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return false;
    return a[i] == b[i];
}

// Sapaan menurut jam lokal.
const char* greeting_for(int hour) {
    if (hour < 5)  return "Good night";
    if (hour < 12) return "Good morning";
    if (hour < 17) return "Good afternoon";
    if (hour < 21) return "Good evening";
    return "Good night";
}

// Thunk C -> method. userdata = ClockApp*.
void tPage(void* ud)      { static_cast<ClockApp*>(ud)->onPageChanged(); }
void tSwToggle(void* ud)  { static_cast<ClockApp*>(ud)->onStopwatchToggle(); }
void tSwLap(void* ud)     { static_cast<ClockApp*>(ud)->onStopwatchLap(); }
void tSwReset(void* ud)   { static_cast<ClockApp*>(ud)->onStopwatchReset(); }
void tTmToggle(void* ud)  { static_cast<ClockApp*>(ud)->onTimerToggle(); }
void tTmReset(void* ud)   { static_cast<ClockApp*>(ud)->onTimerReset(); }
void tTmPlus(void* ud)    { static_cast<ClockApp*>(ud)->onTimerPlus(); }
void tTmMinus(void* ud)   { static_cast<ClockApp*>(ud)->onTimerMinus(); }
void tWorld(void* ud)     { static_cast<ClockApp*>(ud)->onWorldChanged(); }
void tSeconds(void* ud)   { static_cast<ClockApp*>(ud)->onToggleSeconds(); }
void tQuit(void* ud)      { static_cast<ClockApp*>(ud)->onQuit(); }

}  // namespace

// ------------------------------------------------------------
ClockApp::ClockApp()
    : win_(0), xml_(0), tab_(0),
      greeting_(0), time_main_(0), time_sec_(0), date_line_(0), zone_line_(0),
      analog_(0), analog_caption_(0), sec_switch_(0),
      world_list_(0), world_hint_(0),
      sw_display_(0), sw_state_(0), sw_start_(0), sw_lap_(0), sw_reset_(0),
      sw_laps_(0),
      tm_display_(0), tm_state_(0), tm_ring_(0), tm_start_(0), tm_reset_(0),
      tm_plus_(0), tm_minus_(0),
      status_(0),
      page_(Page::Home), zone_(kz_zone_default()), hour12_(0), show_seconds_(1),
      hero_w_(0), hero_h_(0), sub_w_(0), sub_h_(0) {
    for (int i = 0; i < kPageCount; i++) pages_[i] = 0;
    drawn_.valid = false;
    drawn_.sw_laps = -1;
    drawn_.tm_permil = -1;
    drawn_.sw_ms = ~0ULL;
    drawn_.tm_rem_s = ~0ULL;
    drawn_.sw_running = false;
    drawn_.year = drawn_.month = drawn_.day = drawn_.weekday = -1;
    drawn_.hour = drawn_.minute = drawn_.second = -1;
    drawn_.hour12 = -1;
    drawn_.tm_state = -1;   // mustahil -> selalu sinkron
    drawn_.time_main[0] = drawn_.time_sec[0] = '\0';
    drawn_.date_line[0] = drawn_.zone_line[0] = '\0';
    drawn_.sw_display[0] = '\0';
    drawn_.tm_display[0] = '\0';
}

ClockApp::~ClockApp() {
    // Urutan: window dulu (menghancurkan pohon widget), baru konteks XML.
    // Konteks memegang ID map yang menunjuk widget; menghancurkannya lebih
    // dulu akan meninggalkan pointer ke widget yang masih hidup, dan sebaliknya
    // aman karena destroy konteks tidak menyentuh widget.
    if (win_) ui_window_destroy(win_);
    if (xml_) ui_xml_ctx_destroy(xml_);
}

// ------------------------------------------------------------
// Konfigurasi: zona + format 12/24 jam dari /time.ui.
// ------------------------------------------------------------
void ClockApp::loadConfig() {
    kz_time_cfg_t cfg;
    kz_time_cfg_load(&cfg);
    zone_ = cfg.zone;
    hour12_ = cfg.hour12;
}

const char* ClockApp::zoneRule() const {
    int n = 0;
    const kz_zone_t* t = kz_zone_table(&n);
    if (zone_ < 0 || zone_ >= n) return 0;
    return t[zone_].rule;
}

void ClockApp::nowLocal(kz_civil_t* out, uint64_t* ms_frac) const {
    uint32_t t[6];   // [year, month, day, hour, min, sec] UTC
    sys_get_time(t);
    kz_civil_t utc;
    utc.year = (int32_t)t[0]; utc.month = (int32_t)t[1];
    utc.day = (int32_t)t[2];  utc.hour = (int32_t)t[3];
    utc.minute = (int32_t)t[4]; utc.second = (int32_t)t[5];
    utc.weekday = 0; utc.yday = 0;
    int64_t unix_sec = kz_civil_to_unix(&utc);
    kz_civil_local(zoneRule(), unix_sec, out);
    // Pecahan detik dari jam MONOTONIC (sys_uptime), bukan dari RTC: RTC hanya
    // punya resolusi detik, dan jarum detik yang halus butuh sub-detik.
    if (ms_frac) *ms_frac = sys_uptime() % 1000ULL;
}

// ------------------------------------------------------------
int ClockApp::run() {
    // Font UI sistem untuk widget toolkit. WAJIB sebelum widget pertama
    // (UI-7.1): lebar/tinggi kontrol diukur dari metrik font saat dibuat.
    ui::uifont_install();

    int ww = kWindowW, hh = kWindowH;
    uint32_t sw = 0, sh = 0;
    if (sys_get_screen_size(&sw, &sh) == 0 && sw > 0 && sh > 0) {
        // Layar lebih kecil: susutkan, jangan melampaui. UI-12.4 menetapkan
        // 1280x720 sebagai target minimum; ini hanya jaring pengaman.
        if ((int)sw - 40 < ww) ww = (int)sw - 40;
        if ((int)sh - 60 < hh) hh = (int)sh - 60;
    }
    if (ww < 480) ww = 480;
    if (hh < 360) hh = 360;

    win_ = ui_window_create((uint32_t)ww, (uint32_t)hh);
    if (!win_) return 1;
    ui_window_set_title(win_, "Clock");
    // Hormati tema tersimpan (settings.ui) - jangan paksa tema sendiri.
    ui_settings_load(win_);

    loadConfig();
    // Font hero milik aplikasi. Gagal -> tampilan jatuh ke bitmap toolkit
    // (aplikasi tetap jalan; lihat catatan fallback di draw_*).
    font_.load();
    if (font_.ready()) {
        font_.measure_hero("00:00", &hero_w_, &hero_h_);
        font_.measure_sub("00", &sub_w_, &sub_h_);
    }

    if (!build()) {
        // Dokumen rusak: jangan diam. Beri tahu lewat judul + keluar rapi,
        // bukan window kosong tanpa penjelasan.
        ui_window_set_title(win_, "Clock - UI failed to load");
        ui_window_notify(win_, "Clock: UI definition failed to load", 6000);
    }

    // Satu sumber pembaruan. Tick ~60x/detik; tick() sendiri yang memutuskan
    // apakah ada yang perlu digambar.
    ui_window_set_tick(win_, [](void* ud) -> int {
        static_cast<ClockApp*>(ud)->tick();
        return 1;   // mark_dirty yang menentukan render; lihat tick()
    }, this);

    ui_window_run(win_);
    return 0;
}

// ------------------------------------------------------------
// Bangun UI: XML -> struktur, lalu suntikkan widget kustom.
// ------------------------------------------------------------
bool ClockApp::build() {
    ui_xml_error_t err;
    ui_xml_doc_t* doc = ui_xml_parse(ui_xml_clock, ui_xml_clock_len, &err);
    if (!doc) return false;
    xml_ = ui_xml_ctx_create(win_);
    if (!xml_) { ui_xml_doc_destroy(doc); return false; }
    if (!ui_xml_inflate(xml_, doc, &err)) {
        ui_xml_doc_destroy(doc);
        return false;
    }
    ui_xml_doc_destroy(doc);   // UI hidup tanpa dokumen

    tab_ = ui_xml_find(xml_, "pages");
    status_ = ui_xml_find(xml_, "status");
    if (!tab_ || !status_) return false;

    // Tab harus MENGISI area konten. VBox/HBox di toolkit hanya menempatkan
    // anak, tidak meregangkannya (HBox menetapkan w = lebar isi), jadi ukuran
    // di XML hanyalah placeholder dan harus di-set di sini dari ukuran window.
    // Tanpa ini panel halaman hanya selebar nilai XML dan menyisakan ruang
    // mati di kanan.
    {
        ui::Window* w = reinterpret_cast<ui::Window*>(win_);
        int cw = (int)w->gw->width - 2 * 8;         // margin root VBox (Window::add)
        int ch = (int)w->gw->height - 8 - w->bar_h - w->bbar_h - 8;
        if (cw < 200) cw = 200;
        if (ch < 160) ch = 160;
        ui_widget_set_size(tab_, cw, ch);
    }

    // Panel per halaman = anak Tab (dimiliki Tab, pointer non-owning).
    ui::Tab* t = reinterpret_cast<ui::Tab*>(tab_);
    if (t->n != kPageCount) return false;
    for (int i = 0; i < kPageCount; i++)
        pages_[i] = reinterpret_cast<ui_widget_t*>(t->panels[i]);

    // Root tiap halaman = anak pertama panel (vbox ber-id).
    ui_widget_t* home = ui_xml_find(xml_, "home_root");
    ui_widget_t* world = ui_xml_find(xml_, "world_root");
    ui_widget_t* sw = ui_xml_find(xml_, "stopwatch_root");
    ui_widget_t* tm = ui_xml_find(xml_, "timer_root");
    if (!home || !world || !sw || !tm) return false;

    if (!buildHome(home)) return false;
    if (!buildWorld(world)) return false;
    if (!buildStopwatch(sw)) return false;
    if (!buildTimer(tm)) return false;

    // Binding callback by NAME (UI-18.8). Null-check tidak perlu: ui_xml_bind
    // mengembalikan 0 untuk id yang tidak ada, dan test XML memastikan semua
    // id yang dipakai di sini memang ada di dokumen (test J04).
    ui_xml_bind(xml_, "sw_start", UI_XML_ON_CLICK, tSwToggle, this);
    ui_xml_bind(xml_, "sw_lap", UI_XML_ON_CLICK, tSwLap, this);
    ui_xml_bind(xml_, "sw_reset", UI_XML_ON_CLICK, tSwReset, this);
    ui_xml_bind(xml_, "tm_start", UI_XML_ON_CLICK, tTmToggle, this);
    ui_xml_bind(xml_, "tm_reset", UI_XML_ON_CLICK, tTmReset, this);
    ui_xml_bind(xml_, "tm_plus", UI_XML_ON_CLICK, tTmPlus, this);
    ui_xml_bind(xml_, "tm_minus", UI_XML_ON_CLICK, tTmMinus, this);
    ui_xml_bind(xml_, "sec_switch", UI_XML_ON_CHANGE, tSeconds, this);

    // Tab aktif berubah -> perbarui status + hentikan pembaruan halaman
    // tersembunyi.
    ui_tab_set_change(tab_, tPage, this);

    // Fokuskan strip tab sejak awal. Tanpa ini TIDAK ADA widget yang memegang
    // fokus saat startup, sehingga panah kiri/kanan dibuang sebelum sampai ke
    // Tab (Window hanya mengirim on_key ke widget fokus; tanpa fokus, tombol
    // jatuh ke hook aplikasi yang tidak ada). Akibatnya navigasi tab lewat
    // keyboard tidak berfungsi sampai pengguna mengklik sesuatu lebih dulu —
    // melanggar UI-9.8 (setiap kontrol harus terjangkau keyboard).
    ui_window_focus(win_, tab_);

    // Ctrl+Q keluar; konsisten dengan aplikasi lain yang memakai accelerator.
    ui_window_add_shortcut(win_, KEY_MOD_CTRL, 'q', tQuit, this);

    page_ = Page::Home;
    syncVisible();
    return true;
}

// ------------------------------------------------------------
// Home: waktu besar (FtText) + muka jam analog.
// ------------------------------------------------------------
bool ClockApp::buildHome(ui_widget_t* root) {
    greeting_ = ui_xml_find(xml_, "greeting");
    date_line_ = ui_xml_find(xml_, "date_line");
    zone_line_ = ui_xml_find(xml_, "zone_line");
    analog_caption_ = ui_xml_find(xml_, "analog_caption");
    sec_switch_ = ui_xml_find(xml_, "sec_switch");
    ui_widget_t* time_slot = ui_xml_find(xml_, "time_slot");
    ui_widget_t* analog_slot = ui_xml_find(xml_, "analog_slot");
    if (!greeting_ || !time_slot || !analog_slot || !sec_switch_) return false;

    // Waktu utama: FtText dengan font hero milik aplikasi.
    // Ukuran widget diambil dari metrik font; kalau font gagal dimuat,
    // fallback ke ukuran bitmap supaya tetap ada ruang untuk teks.
    // Lebar sengaja dilebihkan dari lebar teks: ruang ini milik teks yang
    // bisa berubah ("9:05" -> "12:05"), dan FtText tidak menghitung ulang.
    int w = hero_w_ > 0 ? hero_w_ + 40 : 240;
    int h = hero_h_ > 0 ? hero_h_ + 8 : 20;
    time_main_ = ui_fttext_create(win_, w, h);
    ui_fttext_set_draw(time_main_, drawMainTime, this);
    ui_layout_add(time_slot, time_main_);

    // Detik: teks pendukung, sengaja lebih kecil dan di bawah waktu utama
    // (hierarki: jam mendominasi, detik sekunder).
    int sw2 = sub_w_ > 0 ? sub_w_ * 4 : 64;
    int sh2 = sub_h_ > 0 ? sub_h_ + 6 : 20;
    time_sec_ = ui_fttext_create(win_, sw2, sh2);
    ui_fttext_set_draw(time_sec_, drawSubTime, this);
    ui_layout_add(time_slot, time_sec_);

    // Muka jam analog: widget toolkit (butuh Painter + token tema).
    analog_ = ui_analogclock_create(win_, kAnalogSize);
    ui_analogclock_show_seconds(analog_, show_seconds_);
    ui_layout_add(analog_slot, analog_);

    if (sec_switch_) ui_switch_set_on(sec_switch_, show_seconds_);
    return true;
}

// ------------------------------------------------------------
// World Clock: satu baris per kota, diperbarui di tick().
// ------------------------------------------------------------
bool ClockApp::buildWorld(ui_widget_t* root) {
    world_hint_ = ui_xml_find(xml_, "world_hint");
    ui_widget_t* slot = ui_xml_find(xml_, "world_slot");
    if (!slot) return false;

    world_list_ = ui_listview_create(win_, 560, kCityCount * 24 + 8);
    for (int i = 0; i < kCityCount; i++)
        ui_listview_add_row(world_list_, kCities[i].name, "", 0, 0);
    ui_layout_add(slot, world_list_);
    if (world_hint_)
        ui_label_set_text(world_hint_,
                          "Select a city to use its timezone.");
    return true;
}

// ------------------------------------------------------------
// Stopwatch
// ------------------------------------------------------------
bool ClockApp::buildStopwatch(ui_widget_t* root) {
    sw_state_ = ui_xml_find(xml_, "sw_state");
    sw_laps_ = ui_xml_find(xml_, "sw_laps");
    ui_widget_t* slot = ui_xml_find(xml_, "sw_display_slot");
    if (!slot || !sw_laps_) return false;

    // Waktu besar: lebar cukup untuk "00:00.000" pada font hero.
    int w = hero_w_ > 0 ? hero_w_ * 5 : 300;
    int h = hero_h_ > 0 ? hero_h_ + 6 : 20;
    sw_display_ = ui_fttext_create(win_, w, h);
    ui_fttext_set_draw(sw_display_, drawStopwatch, this);
    ui_layout_add(slot, sw_display_);

    // Daftar putaran: diperbarui hanya saat putaran berubah.
    ui_listview_set_change(sw_laps_, 0, 0);
    return true;
}

// ------------------------------------------------------------
// Timer
// ------------------------------------------------------------
bool ClockApp::buildTimer(ui_widget_t* root) {
    tm_state_ = ui_xml_find(xml_, "tm_state");
    ui_widget_t* ring_slot = ui_xml_find(xml_, "tm_ring_slot");
    ui_widget_t* disp_slot = ui_xml_find(xml_, "tm_display_slot");
    if (!ring_slot || !disp_slot) return false;

    tm_ring_ = ui_circularprogress_create(win_, kRingSize);
    ui_layout_add(ring_slot, tm_ring_);

    int w = hero_w_ > 0 ? hero_w_ * 3 : 200;
    int h = hero_h_ > 0 ? hero_h_ + 6 : 20;
    tm_display_ = ui_fttext_create(win_, w, h);
    ui_fttext_set_draw(tm_display_, drawTimer, this);
    ui_layout_add(disp_slot, tm_display_);
    return true;
}

// ------------------------------------------------------------
// Callback penggambaran FtText.
//
// Semuanya memakai kz_text_draw_on (IDEMPOTEN, UI-8.3) dan menggambar DI ATAS
// warna permukaan halaman yang diberikan toolkit - jadi mengulang frame tidak
// menebalkan tepi glyph.
// ------------------------------------------------------------
void ClockApp::drawMainTime(void* ud, uint32_t* canvas, int cw, int ch,
                            int x, int y) {
    ClockApp* a = static_cast<ClockApp*>(ud);
    if (!a->font_.ready()) return;
    color_t bg = a->bg_for_fttext();
    // Baseline: font diukur dari atas; turunkan sedikit agar tidak menempel
    // ke tepi atas kotak.
    int base = y + (a->hero_h_ > 0 ? a->hero_h_ - a->hero_h_ / 5 : 14);
    a->font_.draw_hero(canvas, cw, ch, x, base, a->fg_primary(), bg,
                       a->drawn_.time_main);
}

void ClockApp::drawSubTime(void* ud, uint32_t* canvas, int cw, int ch,
                           int x, int y) {
    ClockApp* a = static_cast<ClockApp*>(ud);
    if (!a->font_.ready()) return;
    color_t bg = a->bg_for_fttext();
    int base = y + (a->sub_h_ > 0 ? a->sub_h_ - a->sub_h_ / 5 : 14);
    a->font_.draw_sub(canvas, cw, ch, x, base, a->fg_secondary(), bg,
                      a->drawn_.time_sec);
}

void ClockApp::drawStopwatch(void* ud, uint32_t* canvas, int cw, int ch,
                             int x, int y) {
    ClockApp* a = static_cast<ClockApp*>(ud);
    if (!a->font_.ready()) return;
    color_t bg = a->bg_for_fttext();
    int base = y + (a->hero_h_ > 0 ? a->hero_h_ - a->hero_h_ / 5 : 14);
    a->font_.draw_hero(canvas, cw, ch, x, base, a->fg_primary(), bg,
                       a->drawn_.sw_display);
}

void ClockApp::drawTimer(void* ud, uint32_t* canvas, int cw, int ch,
                         int x, int y) {
    ClockApp* a = static_cast<ClockApp*>(ud);
    if (!a->font_.ready()) return;
    color_t bg = a->bg_for_fttext();
    int base = y + (a->hero_h_ > 0 ? a->hero_h_ - a->hero_h_ / 5 : 14);
    a->font_.draw_hero(canvas, cw, ch, x, base, a->fg_primary(), bg,
                       a->drawn_.tm_display);
}

// ------------------------------------------------------------
// Warna dari token tema.
//
// Theme dibaca lewat Window, bukan disimpan sendiri: tema bisa berubah saat
// aplikasi berjalan (Settings), dan menyalin warnanya di awal akan membuat
// aplikasi tetap memakai palet lama.
// ------------------------------------------------------------
color_t ClockApp::bg_for_fttext() const {
    const ui::Theme& t = reinterpret_cast<ui::Window*>(win_)->theme;
    // Latar halaman = theme.bg (ui::ELEV_BASE). FtText menggambar di alur halaman,
    // bukan di atas kartu, jadi inilah permukaan yang benar.
    return t.surface_for(ui::ELEV_BASE);
}

color_t ClockApp::fg_primary() const {
    const ui::Theme& t = reinterpret_cast<ui::Window*>(win_)->theme;
    return t.tone(ui::TONE_PRIMARY);
}

color_t ClockApp::fg_secondary() const {
    const ui::Theme& t = reinterpret_cast<ui::Window*>(win_)->theme;
    return t.tone(ui::TONE_SECONDARY);
}

void ClockApp::zoneLabel(int64_t unix_sec, char* out, int cap) const {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    char ab[KZ_TZ_ABBR_MAX];
    kz_tz_abbr(zoneRule(), unix_sec, ab, (int)sizeof(ab));
    int n = 0;
    const kz_zone_t* zt = 0;
    int zn = 0;
    const kz_zone_t* tab = kz_zone_table(&zn);
    if (zone_ >= 0 && zone_ < zn) zt = &tab[zone_];
    // "WIB - Jakarta" (singkatan dulu: itu yang membedakan dua kota ber-offset
    // sama), atau hanya nama kota bila aturannya tidak punya singkatan.
    if (zt) {
        for (int i = 0; zt->label[i] && n < cap - 1; i++) out[n++] = zt->label[i];
        if (ab[0] && n < cap - 4) {
            // Sisipkan singkatan di depan: susun ulang tanpa alokasi.
            char tmp[64];
            int k = 0;
            for (int i = 0; ab[i] && k < 60; i++) tmp[k++] = ab[i];
            if (k < 58) { tmp[k++] = ' '; tmp[k++] = '-'; tmp[k++] = ' '; }
            for (int i = 0; zt->label[i] && k < 62; i++) tmp[k++] = zt->label[i];
            tmp[k] = '\0';
            scopy(out, cap, tmp);
            return;
        }
    }
    if (ab[0]) scopy(out, cap, ab);
}

// ------------------------------------------------------------
// Tick: SATU-SATUNYA tempat nilai diperbarui.
//
// Aturan yang dipegang di sini (UI-20.1):
//   * Halaman yang TIDAK terlihat tidak diperbarui sama sekali. Tanpa ini,
//     muka jam di halaman Home akan menandai dirinya kotor 60x/detik walau
//     pengguna sedang membuka Timer.
//   * Teks hanya di-set ulang bila ISIYNA berubah; set_text selalu menandai
//     kotor, jadi memanggilnya tiap frame = render tiap frame.
// ------------------------------------------------------------
void ClockApp::tick() {
    if (!win_) return;
    const uint64_t uptime = sys_uptime();
    kz_civil_t loc;
    uint64_t ms_frac = 0;
    nowLocal(&loc, &ms_frac);
    int64_t unix_sec = kz_civil_to_unix(&loc);

    if (page_ == Page::Home) {
        // --- Waktu utama: hanya format ulang bila menit/jam berubah. ---
        if (loc.hour != drawn_.hour || loc.minute != drawn_.minute ||
            hour12_ != drawn_.hour12) {
            kz_fmt_hhmm(loc.hour, loc.minute, hour12_, drawn_.time_main,
                        (int)sizeof(drawn_.time_main));
            if (time_main_) ui_fttext_refresh(time_main_);
            drawn_.hour = loc.hour;
            drawn_.minute = loc.minute;
            drawn_.hour12 = hour12_;
        }
        // --- Detik: berubah tiap detik, bukan tiap frame. ---
        if (loc.second != drawn_.second) {
            char b[8];
            kz_fmt_hhmmss(0, 0, loc.second, 0, b, (int)sizeof(b));
            // Hanya "SS" yang ditampilkan; ambil dua digit terakhir.
            int n = 0;
            while (b[n]) n++;
            const char* ss = (n >= 2) ? b + n - 2 : b;
            scopy(drawn_.time_sec, (int)sizeof(drawn_.time_sec), ss);
            if (time_sec_) ui_fttext_refresh(time_sec_);
            drawn_.second = loc.second;
        }
        // --- Tanggal & sapaan: sekali sehari. ---
        if (loc.day != drawn_.day || loc.month != drawn_.month ||
            loc.year != drawn_.year) {
            kz_fmt_date_long(&loc, drawn_.date_line,
                             (int)sizeof(drawn_.date_line));
            if (date_line_) ui_label_set_text(date_line_, drawn_.date_line);
            if (greeting_) ui_label_set_text(greeting_, greeting_for(loc.hour));
            drawn_.day = loc.day;
            drawn_.month = loc.month;
            drawn_.year = loc.year;
        }
        // --- Label zona: hanya bila berubah (mis. DST mulai/berakhir). ---
        char zl[32];
        zoneLabel(unix_sec, zl, (int)sizeof(zl));
        if (!same(zl, drawn_.zone_line)) {
            scopy(drawn_.zone_line, (int)sizeof(drawn_.zone_line), zl);
            if (zone_line_) ui_label_set_text(zone_line_, drawn_.zone_line);
        }
        // --- Muka jam analog: sudut dari detik + pecahan milidetik. ---
        // set_time mengembalikan 0 bila sudut yang terkuantisasi belum
        // berubah, jadi tidak ada render untuk perubahan yang tak terlihat.
        if (analog_ && show_seconds_) {
            if (ui_analogclock_set_time(analog_, loc.hour, loc.minute,
                                        loc.second, (int)ms_frac))
                ui_fttext_refresh(time_sec_);   // jaga area detik ikut segar
        } else if (analog_) {
            ui_analogclock_set_time(analog_, loc.hour, loc.minute, 0, 0);
        }
    } else if (page_ == Page::World) {
        // Kota hanya berubah tiap MENIT, jadi pembaruan ditahan sampai menit
        // berganti - bukan tiap tick.
        if (loc.minute != drawn_.minute || !drawn_.valid) {
            for (int i = 0; i < kCityCount; i++) {
                int zi = kz_zone_find(kCities[i].zone);
                int zn = 0;
                const kz_zone_t* tab = kz_zone_table(&zn);
                if (zi < 0 || zi >= zn) continue;
                kz_civil_t c;
                kz_civil_local(tab[zi].rule, unix_sec, &c);
                char tm[16];
                kz_fmt_hhmm(c.hour, c.minute, hour12_, tm, (int)sizeof(tm));
                char ab[KZ_TZ_ABBR_MAX];
                kz_tz_abbr(tab[zi].rule, unix_sec, ab, (int)sizeof(ab));
                // Baris kaya: judul = kota, deskripsi = waktu + zona.
                // Perbedaan hari ditandai eksplisit (Yesterday/Tomorrow) supaya
                // tidak hanya bergantung pada warna/posisi.
                char desc[48];
                int k = 0;
                for (int j = 0; tm[j] && k < 40; j++) desc[k++] = tm[j];
                if (ab[0] && k < 40) {
                    desc[k++] = ' ';
                    desc[k++] = 0xE2;   // UTF-8 untuk titik tengah (·)
                    desc[k++] = 0x80;
                    desc[k++] = 0xA2;
                    desc[k++] = ' ';
                    for (int j = 0; ab[j] && k < 44; j++) desc[k++] = ab[j];
                }
                if (c.day != loc.day && k < 40) {
                    const char* rel = (c.day > loc.day) ? " - tomorrow"
                                                        : " - yesterday";
                    for (int j = 0; rel[j] && k < 46; j++) desc[k++] = rel[j];
                }
                desc[k] = '\0';
                // Baris sudah ada (dibuat sekali di buildWorld); perbarui
                // deskripsinya. Ini bukan alokasi per frame.
                ui_listview_set_row_description(world_list_, i, desc);
            }
            drawn_.minute = loc.minute;
            drawn_.valid = true;
        }
    } else if (page_ == Page::Stopwatch) {
        uint64_t ms = sw_.elapsed(uptime);
        if (ms != drawn_.sw_ms) {
            fmt_elapsed(ms, drawn_.sw_display, (int)sizeof(drawn_.sw_display));
            if (sw_display_) ui_fttext_refresh(sw_display_);
            drawn_.sw_ms = ms;
        }
        // Tombol berubah label saat berjalan/jeda (Start <-> Pause) supaya
        // keadaannya jelas tanpa harus membaca teks status.
        if (sw_.running() != drawn_.sw_running) {
            if (sw_start_)
                ui_button_set_text(sw_start_, sw_.running() ? "Pause" : "Start");
            if (sw_lap_) ui_widget_set_enabled(sw_lap_, sw_.running() ? 1 : 0);
            drawn_.sw_running = sw_.running();
        }
        if (sw_.lap_count() != drawn_.sw_laps) {
            ui_listview_clear(sw_laps_);
            for (int i = 0; i < sw_.lap_count(); i++) {
                char n[16], t[16], d[16];
                // Nomor putaran: yang terbaru di atas.
                int no = sw_.lap_count() - i;
                int k = 0;
                if (no >= 10) n[k++] = (char)('0' + (no / 10) % 10);
                n[k++] = (char)('0' + no % 10);
                n[k] = '\0';
                fmt_mmss(sw_.lap_total(i), t, (int)sizeof(t));
                fmt_mmss(sw_.lap_delta(i), d, (int)sizeof(d));
                char desc[40];
                int j = 0;
                for (int q = 0; d[q] && j < 30; q++) desc[j++] = d[q];
                desc[j] = '\0';
                ui_listview_add_row(sw_laps_, t, desc, 0, 0);
            }
            drawn_.sw_laps = sw_.lap_count();
        }
    } else if (page_ == Page::Timer) {
        // Transisi selesai dilaporkan SEKALI oleh model, jadi notifikasi tidak
        // berulang tiap frame.
        if (tm_.tick(uptime)) {
            ui_window_notify(win_, "Timer finished", 5000);
            if (tm_state_) ui_label_set_text(tm_state_, "Finished");
        }
        int permil = tm_.progress_permil(uptime);
        if (tm_ring_ && permil != drawn_.tm_permil) {
            ui_circularprogress_set_value(tm_ring_, permil);
            drawn_.tm_permil = permil;
        }
        uint64_t rem = tm_.remaining(uptime);
        // Tampilan timer berubah tiap detik (bukan tiap milidetik): menghindari
        // 60 render/detik untuk digit yang tidak berubah.
        uint64_t rem_s = (rem + 999) / 1000;
        if (rem_s != drawn_.tm_rem_s) {
            fmt_duration(rem, drawn_.tm_display,
                         (int)sizeof(drawn_.tm_display));
            if (tm_display_) ui_fttext_refresh(tm_display_);
            drawn_.tm_rem_s = rem_s;
        }
        if (tm_.state() != drawn_.tm_state) {
            const char* s = "Ready";
            if (tm_.state() == Timer::RUNNING) s = "Running";
            else if (tm_.state() == Timer::PAUSED) s = "Paused";
            else if (tm_.state() == Timer::DONE) s = "Finished";
            if (tm_state_) ui_label_set_text(tm_state_, s);
            if (tm_start_)
                ui_button_set_text(tm_start_,
                                   tm_.running() ? "Pause" : "Start");
            // Durasi tidak boleh diubah saat berjalan: mengubahnya membatalkan
            // hitungan, dan tombol yang terlihat aktif padahal tidak akan
            // membingungkan.
            if (tm_plus_) ui_widget_set_enabled(tm_plus_, tm_.running() ? 0 : 1);
            if (tm_minus_) ui_widget_set_enabled(tm_minus_, tm_.running() ? 0 : 1);
            drawn_.tm_state = tm_.state();
        }
    }
}

// ------------------------------------------------------------
// Aksi (dipanggil callback widget)
// ------------------------------------------------------------
void ClockApp::onPageChanged() {
    if (!tab_) return;
    int a = ui_tab_active(tab_);
    if (a < 0 || a >= kPageCount) return;
    page_ = (Page)a;
    // Halaman baru harus menggambar dirinya penuh pada tick berikutnya: reset
    // nilai "terakhir digambar" yang relevan supaya tick menganggap semuanya
    // berubah. Tanpa ini, halaman yang baru dibuka bisa tampil dengan nilai
    // basi sampai kebetulan ada field yang berubah.
    drawn_.valid = false;
    drawn_.hour = drawn_.minute = drawn_.second = -1;
    drawn_.day = drawn_.month = drawn_.year = -1;
    drawn_.sw_ms = ~0ULL;
    drawn_.sw_laps = -1;
    drawn_.tm_rem_s = ~0ULL;
    drawn_.tm_permil = -1;
    drawn_.tm_state = -1;   // mustahil -> selalu sinkron
    drawn_.sw_running = !sw_.running();
    if (status_) {
        static const char* names[kPageCount] = {
            "Clock", "World Clock", "Stopwatch", "Timer"
        };
        ui_statusbar_set_text(status_, names[a],
                              "KyuzenOS  |  Ctrl+Q to quit");
    }
    // Segarkan halaman yang baru terlihat segera, bukan menunggu tick pertama.
    tick();
}

void ClockApp::onStopwatchToggle() {
    sw_.toggle(sys_uptime());
    if (status_)
        ui_statusbar_set_text(status_,
                              sw_.running() ? "Stopwatch running"
                                            : "Stopwatch paused",
                              "Ctrl+Q to quit");
    tick();
}

void ClockApp::onStopwatchLap() {
    if (!sw_.running()) return;
    sw_.lap(sys_uptime());
    tick();
}

void ClockApp::onStopwatchReset() {
    sw_.reset();
    // Tampilan harus langsung kembali ke nol, bukan menunggu tick berikutnya.
    drawn_.sw_ms = ~0ULL;
    drawn_.sw_laps = -1;
    drawn_.sw_running = !sw_.running();
    if (status_) ui_statusbar_set_text(status_, "Stopwatch reset", "Ctrl+Q to quit");
    tick();
}

void ClockApp::onTimerToggle() {
    uint64_t now = sys_uptime();
    if (tm_.running()) tm_.pause(now);
    else if (tm_.state() == Timer::PAUSED) tm_.resume(now);
    else tm_.start(now);
    drawn_.tm_state = -1;
    if (status_) {
        const char* s = tm_.running() ? "Timer running"
                     : (tm_.state() == Timer::PAUSED ? "Timer paused"
                                                     : "Timer started");
        ui_statusbar_set_text(status_, s, "Ctrl+Q to quit");
    }
    tick();
}

void ClockApp::onTimerReset() {
    tm_.reset();
    drawn_.tm_state = -1;
    drawn_.tm_rem_s = ~0ULL;
    drawn_.tm_permil = -1;
    if (status_) ui_statusbar_set_text(status_, "Timer reset", "Ctrl+Q to quit");
    tick();
}

void ClockApp::onTimerPlus() {
    tm_.adjust_minutes(1);
    drawn_.tm_state = -1;
    drawn_.tm_rem_s = ~0ULL;
    drawn_.tm_permil = -1;
    tick();
}

void ClockApp::onTimerMinus() {
    tm_.adjust_minutes(-1);
    drawn_.tm_state = -1;
    drawn_.tm_rem_s = ~0ULL;
    drawn_.tm_permil = -1;
    tick();
}

void ClockApp::onWorldChanged() {
    if (!world_list_) return;
    int sel = ui_listview_selected(world_list_);
    if (sel < 0 || sel >= kCityCount) return;
    // Memilih kota menjadikannya zona aktif sistem (tersimpan), jadi jam
    // halaman Home ikut mengikuti. Itu yang membuat World Clock berguna
    // alih-alih hanya daftar bacaan.
    int zi = kz_zone_find(kCities[sel].zone);
    if (zi < 0) return;
    kz_time_cfg_t cfg;
    cfg.zone = zi;
    cfg.hour12 = hour12_;
    if (kz_time_cfg_save(&cfg)) {
        zone_ = zi;
        drawn_.valid = false;
        drawn_.hour = drawn_.minute = drawn_.second = -1;
        drawn_.day = drawn_.month = drawn_.year = -1;
        drawn_.zone_line[0] = '\0';
        if (status_) {
            char b[64];
            scopy(b, (int)sizeof(b), "Timezone: ");
            int k = 0;
            while (b[k]) k++;
            for (int i = 0; kCities[sel].name[i] && k < 60; i++)
                b[k++] = kCities[sel].name[i];
            b[k] = '\0';
            ui_statusbar_set_text(status_, b, "Saved to /time.ui");
        }
        tick();
    } else if (status_) {
        ui_statusbar_set_text(status_, "Could not save timezone",
                              "/time.ui is not writable");
    }
}

void ClockApp::onToggleSeconds() {
    if (!sec_switch_) return;
    show_seconds_ = ui_switch_on(sec_switch_) ? 1 : 0;
    if (analog_) {
        ui_analogclock_show_seconds(analog_, show_seconds_);
        // Saat jarum detik disembunyikan, jangan biarkan tick memicu render
        // tiap frame: setel sudutnya ke detik utuh.
        if (!show_seconds_) ui_analogclock_set_time(analog_, 0, 0, 0, 0);
    }
    if (status_)
        ui_statusbar_set_text(status_,
                              show_seconds_ ? "Second hand on"
                                            : "Second hand off",
                              "Ctrl+Q to quit");
}

void ClockApp::onQuit() {
    if (win_) ui_window_request_close(win_);
}

void ClockApp::syncVisible() {    // Tab menangani visibilitas panel sendiri; tidak ada yang perlu dilakukan
    // di sini selain menyelaraskan status awal.
    if (status_)
        ui_statusbar_set_text(status_, "Clock",
                              "KyuzenOS  |  Ctrl+Q to quit");
}

}  // namespace clock_app
