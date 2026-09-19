#ifndef CRASH_ARCHIVE_H
#define CRASH_ARCHIVE_H

#include <stdint.h>
#include "crash_notice.h"

// =====================================================================
// crash_archive — terbitkan crashdump sebagai BERKAS di KyuzenFS
// (implementasi: kernel/crash_archive.c)
//
// KENAPA ADA
//   Snapshot panic ditulis ke sektor mentah di ekor disk
//   (kernel/crashdump.c) dan hanya bisa dibaca lewat serial/terminal.
//   Modul ini memindahkannya menjadi berkas biasa supaya bisa dibuka
//   aplikasi grafis (fileman/viewer/notepad) — bukan cuma di terminal.
//
// KAPAN JALAN
//   SEKALI saat boot, setelah kfs_init() (FS siap) dan crashdump_init().
//   SENGAJA bukan di jalur panic: menulis berkas butuh lock FS + block cache,
//   dan lock itu justru sering dipegang CPU yang fault. Jalur panic tetap
//   hanya menulis sektor mentah (zero-lock); boot berikutnya yang
//   menerbitkannya ke FS.
//
// IDEMPOTEN
//   Kalau isi berkas sudah sama dengan dump terakhir (konten dibandingkan
//   apa adanya), tidak ada penulisan ulang — boot berulang tidak terus
//   menulis disk.
// =====================================================================

// Berkas laporan (teks, ASCII) di root FS. Root dipilih supaya langsung
// terlihat oleh fileman, yang hanya menampilkan direktori akar.
#define CRASH_ARCHIVE_PATH  "/crash-report.txt"

// Terbitkan dump terakhir ke CRASH_ARCHIVE_PATH. Diam-diam no-op kalau tidak
// ada dump valid (boot normal). Hasilnya dilaporkan ke serial COM1.
void crash_archive_publish(void);

// Isi data pemberitahuan untuk aplikasi (syscall SYS_CRASH_NOTICE).
// Return 1 kalau boot ini menerbitkan laporan BARU, 0 kalau tidak. `out` adalah
// struct milik KERNEL (copy ke user-space dilakukan pemanggil syscall).
int crash_archive_notice(crash_notice_t* out);

#endif
