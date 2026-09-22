#ifndef RTC_H
#define RTC_H

#include <stdint.h>

// Array time_buf akan diisi dengan: [Tahun, Bulan, Hari, Jam, Menit, Detik]
void read_rtc(uint32_t* time_buf);

// Varian yang dipakai syscall 20 (sys_get_time) — wrapper di drivers/rtc.c
// di atas read_rtc. Deklarasi kanonis di sini (bukan `extern` lokal).
void rtc_read_time(uint32_t* time_buf);

#endif