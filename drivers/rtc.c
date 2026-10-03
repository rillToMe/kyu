#include "rtc.h"

// Fungsi untuk membaca dan menulis ke Port Hardware
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// Membaca register CMOS
static uint8_t get_rtc_register(int reg) {
    outb(0x70, reg);
    return inb(0x71);
}

// Konversi BCD ke Angka Biasa
static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd / 16) * 10);
}

// ---------------------------------------------------------------------------
// Baca RTC sebagai UTC.
//
// PERUBAHAN PENTING (dulu salah):
//   Versi lama menambahkan +7 jam (WIB) di sini TANPA menggeser TANGGAL, lalu
//   membungkus jam dengan `% 24`. Akibatnya tanggal salah selama 7 jam setiap
//   hari: pukul 20:00 UTC tanggal 20 dilaporkan sebagai 03:00 tanggal 20,
//   padahal WIB-nya sudah tanggal 21.
//
//   Selain itu, zona waktu jadi keputusan KERNEL: aplikasi tidak punya cara
//   memilih zona lain, dan TLS di browser harus mengurangi 7 jam itu lagi
//   untuk mendapatkan UTC — dua tempat yang harus selalu sinkron.
//
//   Sekarang kernel melaporkan UTC APA ADANYA. Penerjemahan ke waktu lokal
//   adalah tugas user-space (libs/time, kz_civil_local), sehingga:
//     * zona waktu bisa diubah tanpa menyentuh kernel,
//     * tanggal ikut bergeser dengan benar saat offset melewati tengah malam,
//     * hanya ada SATU tempat yang tahu zona waktu.
//
// Register status B juga dibaca karena dua hal yang sebelumnya diabaikan:
//   * bit 1 (DM)  — apakah jam disimpan BCD atau biner
//   * bit 2 (24/12) — apakah jam dalam mode 12 jam (butuh bit PM di 0x04)
// Tanpa keduanya, RTC yang dikonfigurasi 12 jam akan dilaporkan salah 12 jam.
// ---------------------------------------------------------------------------
void read_rtc(uint32_t* time_buf) {
    // Tunggu update selesai SEBELUM membaca apa pun. Membaca register saat UIP
    // menyala bisa memberi campuran nilai lama/baru (mis. menit baru dengan jam
    // lama). Batas iterasi agar tidak pernah menggantung kalau RTC mati.
    for (int i = 0; i < 1000000; i++) {
        if ((get_rtc_register(0x0A) & 0x80) == 0) break;
    }

    uint8_t sec   = get_rtc_register(0x00);
    uint8_t min   = get_rtc_register(0x02);
    uint8_t hour  = get_rtc_register(0x04);
    uint8_t day   = get_rtc_register(0x07);
    uint8_t month = get_rtc_register(0x08);
    uint8_t year  = get_rtc_register(0x09);
    uint8_t statb = get_rtc_register(0x0B);

    int binary = (statb & 0x04) != 0;          // DM = 1 -> nilai biner
    int h12    = (statb & 0x02) == 0;          // 24/12 = 0 -> mode 12 jam

    // Bit 7 jam = flag PM (hanya berarti di mode 12 jam). Harus dibuang
    // SEBELUM konversi BCD: bcd_to_bin(0x92) = 92, dan 92 & 0x7F = 28 — salah.
    int pm = 0;
    if (h12) {
        pm = (hour & 0x80) != 0;
        hour &= 0x7F;
    }

    uint32_t h = binary ? hour : bcd_to_bin(hour);
    uint32_t m = binary ? min  : bcd_to_bin(min);
    uint32_t s = binary ? sec  : bcd_to_bin(sec);

    if (h12) {
        // 12 AM (tengah malam) -> 0, 12 PM (tengah hari) -> 12.
        if (h == 12) h = 0;
        if (pm) h += 12;
    }

    time_buf[0] = (binary ? year  : bcd_to_bin(year))  + 2000;
    time_buf[1] = (binary ? month : bcd_to_bin(month));
    time_buf[2] = (binary ? day   : bcd_to_bin(day));
    // UTC apa adanya: tidak ada penambahan offset di kernel.
    time_buf[3] = h;
    time_buf[4] = m;
    time_buf[5] = s;
}

// Alias publik yang dipanggil oleh syscall.c (sys_get_time / syscall 20)
void rtc_read_time(uint32_t* time_buf) {
    read_rtc(time_buf);
}
