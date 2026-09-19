#ifndef CRASHDUMP_H
#define CRASHDUMP_H

#include <stdint.h>

// =====================================================================
// crashdump — snapshot panic ke sektor mentah disk (kernel/crashdump.c)
//
// Prinsip:
//   * POLLING ATA PIO murni (drivers/ata.c): tidak memakai interrupt,
//     scheduler/thread, heap, atau lock apa pun — aman di konteks panic
//     dengan semua IRQ dimatikan.
//   * Area = CRASHDUMP area yang DISISIHKAN di ekor disk; KyuzenFS berhenti
//     KZFS_CRASHDUMP_SECTORS sebelum akhir disk (include/kyuzenfs_v4.h),
//     jadi snapshot tidak pernah menimpa data file.
//   * Zero-dynamic-allocation: satu buffer staging statis.
// =====================================================================

#define CRASHDUMP_MAGIC   0x44435A4Bu   // "KZCD" (Kyuzen CrashDump)
#define CRASHDUMP_VERSION 1u
#define CRASHDUMP_MAX_BYTES 4096u       // 8 sektor = 4KB (batas spec fitur)
#define CRASHDUMP_SECTORS   (CRASHDUMP_MAX_BYTES / 512u)

// Header dump (packed, little-endian, ditulis di sektor pertama area).
#define CRASHDUMP_FLAG_TRUNCATED 1u     // payload dipotong karena > batas

typedef struct __attribute__((packed)) crashdump_hdr {
    uint32_t magic;                 // CRASHDUMP_MAGIC
    uint16_t version;               // CRASHDUMP_VERSION
    uint16_t flags;                 // CRASHDUMP_FLAG_*
    uint32_t length;                // panjang payload (byte)
    uint32_t checksum;              // sum32 payload — deteksi sektor rusak
    uint32_t sectors;               // total sektor yang ditulis (termasuk header)
    uint32_t dump_count;            // urutan dump (dibaca ulang saat boot)
    uint64_t timestamp_ms;          // timer_get_ms() saat panic
    uint64_t exception_vector;      // int_num; 0xFFFF = kernel_panic() manual
    uint64_t error_code;
    uint64_t rip;
    uint64_t cr2;
    uint64_t rsp;
    uint64_t cs;
    uint64_t cr3;
    uint64_t task_id;
    char     task_name[16];
} crashdump_hdr_t;

// Siapkan area. crash_start_lba + crash_sectors = area di ekor disk.
// Membaca header dump lama (kalau ada) untuk melanjutkan dump_count.
void crashdump_init(uint64_t crash_start_lba, uint32_t crash_sectors);

int  crashdump_ready(void);
int  crashdump_is_active(void);

// Tulis snapshot: header + payload teks (keduanya sudah dibangun pemanggil,
// tanpa alokasi). Return 0 sukses, <0 gagal:
//   -1 belum di-init / argumen tidak valid
//   -2 dipanggil saat dump lain masih berjalan (recursion guard)
//   -3 dibatalkan setelah crashdump_abort() (panic bersarang)
//   -4 verifikasi baca-balik gagal (isi disk tidak sama)
int  crashdump_write_snapshot(const crashdump_hdr_t* hdr, const void* payload, uint32_t len);

// RECURSION GUARD: dipanggil panic.c saat panic baru terjadi sementara dump
// berjalan — dump berhenti di antara sektor dan sistem jatuh ke freeze/halt
// (jangan menulis apa pun sambil menyentuh state yang belum tentu sehat).
void crashdump_abort(void);

// Baca kembali dump TERAKHIR dari area ekor disk (polling ATA, aman dipanggil
// dari konteks normal). Return 1 kalau header valid, 0 kalau area belum pernah
// diisi / isinya rusak.
//   out_hdr     : header (boleh NULL)
//   out_payload : buffer teks payload (boleh NULL)
//   cap         : kapasitas out_payload
//   out_len     : panjang payload yang dibaca (boleh NULL)
int  crashdump_read_last(crashdump_hdr_t* out_hdr, void* out_payload,
                         uint32_t cap, uint32_t* out_len);

// Susun laporan crashdump yang bisa dibaca manusia (header + payload) tanpa
// alokasi dinamis. Return panjang teks (tanpa NUL); 0 kalau tidak ada dump
// valid. Dipakai kernel/crash_archive.c untuk menerbitkannya sebagai berkas.
uint32_t crashdump_format_report(char* out, uint32_t cap);

// LBA area crashdump (untuk mencantumkan lokasi raw di laporan).
uint64_t crashdump_start_lba(void);

#endif
