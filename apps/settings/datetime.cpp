// apps/settings/datetime.cpp - implementasi halaman Date & Time.
#include "datetime.hpp"

namespace settings {

namespace {

void onZone(void* ud) {
    DateTimePage::ZoneBinding* b = static_cast<DateTimePage::ZoneBinding*>(ud);
    if (b && b->page) {
        // Baris yang diklik = zona b->zone. Nilainya dipakai langsung, bukan
        // dibaca ulang dari listview: indeks baris == indeks zona tabel, dan
        // binding menyimpannya supaya tidak perlu konversi di sini.
        b->page->setZone(b->zone);
    }
}

void onHour12(void* ud) {
    DateTimePage* p = static_cast<DateTimePage*>(ud);
    if (p) p->onHour12Toggled();
}

}  // namespace

void DateTimePage::build(ui_window_t* w) {
    win = w;

    // Struktur halaman dibangun NATIVE, bukan lewat XML: isinya adalah tabel
    // runtime (daftar zona dari libs/time), dan jumlah barisnya baru diketahui
    // saat build. Ini pola yang sama dengan sidebar File Manager - XML dipakai
    // untuk struktur yang tetap, bukan untuk daftar yang tumbuh.
    root = ui_vbox_create(w, UI_SPACE_LG);
    if (!root) return;

    ui_layout_add(root, ui_label_create(w, "Date & Time"));
    ui_layout_add(root, ui_label_create(w,
        "Choose the timezone used across the system. "
        "Saved to /time.ui and picked up by the desktop, shell and Clock."));

    // --- Format jam ---
    ui_widget_t* fmt_sec = ui_section_create(w, "Format", UI_SPACE_SM);
    hour12_switch = ui_switch_create(w, "Use 12-hour clock");
    ui_switch_set_change(hour12_switch, onHour12, this);
    ui_layout_add(fmt_sec, hour12_switch);
    ui_layout_add(root, fmt_sec);

    // --- Zona waktu ---
    ui_widget_t* zone_sec = ui_section_create(w, "Timezone", UI_SPACE_SM);

    int n = 0;
    const kz_zone_t* table = kz_zone_table(&n);
    if (n > KZ_TZ_TABLE_MAX) n = KZ_TZ_TABLE_MAX;

    zone_list = ui_listview_create(w, 420, 300);
    for (int i = 0; i < n; i++) {
        // Baris kaya: judul = nama kota, deskripsi = offset contoh.
        // Deskripsi sengaja menampilkan offset NYATA dari aturan zona, bukan
        // teks yang ditulis ulang di sini - kalau tabel zona berubah, yang
        // tampil ikut berubah.
        char desc[48];
        int32_t off = kz_tz_offset(table[i].rule, 0);
        int k = 0;
        const char* p = "UTC";
        if (off < 0) { desc[k++] = '-'; off = -off; } else { desc[k++] = '+'; }
        int oh = (int)(off / 3600);
        int om = (int)((off % 3600) / 60);
        if (oh >= 10) desc[k++] = (char)('0' + (oh / 10) % 10);
        desc[k++] = (char)('0' + oh % 10);
        if (om) {
            desc[k++] = ':';
            desc[k++] = (char)('0' + (om / 10) % 10);
            desc[k++] = (char)('0' + om % 10);
        }
        (void)p;
        desc[k] = '\0';
        ui_listview_add_row(zone_list, table[i].label, desc, 0, 0);
    }

    // Binding per baris: indeks zona disimpan di tabel page, bukan di widget.
    for (int i = 0; i < n; i++) {
        bindings[i].page = this;
        bindings[i].zone = i;
    }
    ui_listview_set_change(zone_list, onZone, &bindings[0]);

    ui_layout_add(zone_sec, zone_list);
    ui_layout_add(root, zone_sec);

    status = ui_label_create(w, "");
    ui_layout_add(root, status);

    load();
    syncSelection();
    syncStatus();
}

void DateTimePage::load() {
    kz_time_cfg_t cfg;
    kz_time_cfg_load(&cfg);
    zone_ = cfg.zone;
    hour12_ = cfg.hour12;
    if (hour12_switch) ui_switch_set_on(hour12_switch, hour12_);
}

void DateTimePage::save() {
    kz_time_cfg_t cfg;
    cfg.zone = zone_;
    cfg.hour12 = hour12_;
    // Gagal menyimpan tetap memberi tahu; nilai yang tampil tetap yang dipilih
    // pengguna, tapi status menyebutkan bahwa penyimpanan gagal (jangan diam).
    if (!kz_time_cfg_save(&cfg) && status) {
        ui_label_set_text(status, "Applied, but /time.ui could not be saved");
        return;
    }
    syncStatus();
}
void DateTimePage::syncStatus() {
    if (!status) return;
    int n = 0;
    const kz_zone_t* table = kz_zone_table(&n);
    const char* name = (zone_ >= 0 && zone_ < n) ? table[zone_].label : "?";

    char msg[96];
    int k = 0;
    const char* p = "Timezone: ";
    while (*p && k < 80) msg[k++] = *p++;
    for (int i = 0; name[i] && k < 80; i++) msg[k++] = name[i];
    if (k < 84) { msg[k++] = ' '; msg[k++] = '-'; msg[k++] = ' '; }
    const char* f = hour12_ ? "12-hour" : "24-hour";
    for (int i = 0; f[i] && k < 92; i++) msg[k++] = f[i];
    msg[k] = '\0';
    ui_label_set_text(status, msg);
}

void DateTimePage::syncSelection() {
    if (zone_list) ui_listview_set_selected(zone_list, zone_);
}

void DateTimePage::setZone(int zone) {
    int n = 0;
    kz_zone_table(&n);
    if (zone < 0 || zone >= n) return;
    zone_ = zone;
    save();
}

void DateTimePage::onHour12Toggled() {
    if (!hour12_switch) return;
    hour12_ = ui_switch_on(hour12_switch) ? 1 : 0;
    save();
}

}  // namespace settings
