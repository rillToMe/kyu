// apps/settings/datetime.hpp - halaman Date & Time (zona waktu + format jam).
//
// MENGAPA HALAMAN INI ADA
// Zona waktu adalah keputusan PENGGUNA, dan sebelum ini tidak ada tempat untuk
// mengubahnya: kernel menambahkan WIB secara hardcoded. Setelah kernel
// diperbaiki agar melaporkan UTC (drivers/rtc.c), zona waktu menjadi pilihan
// di user-space, dan halaman inilah tempat memilihnya.
//
// Preferensinya disimpan di /time.ui (6 byte, tag "KZT1") - format yang SAMA
// dibaca desktop (jam taskbar), shell (`time`), notepad, dan aplikasi Clock.
// Satu file, satu sumber kebenaran; tidak ada salinan preferensi per aplikasi.
//
// Semua zona berasal dari tabel libs/time, jadi halaman ini tidak menyimpan
// daftar kota atau offset sendiri (UI-2.1: nilai visual/logika tidak
// diduplikasi).
#ifndef SETTINGS_DATETIME_HPP
#define SETTINGS_DATETIME_HPP

// kztime.h SEBELUM platform.hpp: KZ_TZ_TABLE_MAX dipakai sebagai ukuran array
// anggota, jadi harus sudah terlihat saat struct didefinisikan. Header ini
// punya guard extern "C" sendiri, jadi aman di-include langsung dari C++.
#include "kztime.h"
#include "platform.hpp"

namespace settings {

struct DateTimePage {
    ui_widget_t* root = nullptr;
    ui_widget_t* status = nullptr;
    ui_window_t* win = nullptr;
    ui_widget_t* zone_list = nullptr;    // daftar zona (baris kaya)
    ui_widget_t* hour12_switch = nullptr;

    // Binding per baris: indeks zona disimpan di sini (bukan di widget), dan
    // alamatnya stabil karena tabel ini milik page.
    struct ZoneBinding {
        DateTimePage* page = nullptr;
        int zone = 0;
    };
    // Ukuran tabel zona libs/time. Nilainya dibaca dari header, bukan disalin:
    // kalau tabel bertambah, array ini ikut - tidak ada angka kedua yang bisa
    // menyimpang.
    ZoneBinding bindings[KZ_TZ_TABLE_MAX];

    void build(ui_window_t* w);
    void setZone(int zone);       // dipanggil binding baris & pilihan program
    void onHour12Toggled();
    void load();
    void save();
private:
    int zone_ = 0;
    int hour12_ = 0;
    void syncStatus();
    void syncSelection();
};

}  // namespace settings

#endif // SETTINGS_DATETIME_HPP
