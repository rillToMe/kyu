#ifndef CRASH_NOTICE_H
#define CRASH_NOTICE_H

#include <stdint.h>

// =====================================================================
// ABI: pemberitahuan crash untuk aplikasi (dipakai kernel + user app)
//
// Isi: 1 kalau BOOT INI menerbitkan laporan crash BARU (yaitu boot tepat
// setelah sistem panic). Boot-boot berikutnya tanpa crash mengembalikan 0 —
// jadi desktop hanya memunculkan notifikasi sekali, bukan tiap startup.
// =====================================================================

#define SYS_CRASH_NOTICE 80   // RBX = crash_notice_t* (user) -> 1 ada / 0 tidak

typedef struct __attribute__((packed)) crash_notice {
    uint32_t pending;       // 1 = laporan baru diterbitkan pada boot ini
    uint32_t crash_count;   // dump ke-N (bertambah tiap panic)
    uint64_t uptime_ms;     // waktu panic (ms sejak boot yang crash)
    uint64_t vector;        // vektor exception; 0xFFFF = kernel_panic() manual
    uint64_t rip;
    uint64_t cr2;
    uint32_t task_id;
    char     task_name[16];
    char     path[32];      // lokasi berkas laporan (mis. /crash-report.txt)
} crash_notice_t;

#endif
