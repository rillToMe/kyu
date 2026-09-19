// kernel/crash_archive.c — crashdump → berkas di FS (lihat include/crash_archive.h)
//
// Alur lengkap fitur crash safety:
//   1. Panic        : kernel/crashdump.c menulis snapshot 4KB ke 8 sektor ekor
//                     disk (polling ATA, tanpa lock) + kernel/panic_log.c
//                     menyimpan record ringkas di RAM reserved.
//   2. Boot berikut : kernel ini membaca kembali area itu, menyusun laporan
//                     teks, lalu menulisnya sebagai CRASH_ARCHIVE_PATH.
//   3. Pengguna     : buka berkas itu di fileman/viewer/notepad, atau
//                     `baca /crash-report.txt` di shell. Tidak perlu QEMU
//                     serial dan tidak perlu terminal.
//
// Prinsip:
//   * Zero-dynamic-allocation (buffer statis) — aman dipanggil kapan saja.
//   * Tidak menyentuh apa pun kalau dump tidak valid/korup.
//   * Idempoten: berkas ditulis ulang hanya kalau isinya berbeda.

#include <stdint.h>
#include <stddef.h>   // NULL
#include "crash_archive.h"
#include "crashdump.h"
#include "kyuzenfs.h"

extern void serial_print(const char* s);
extern void serial_dec(uint64_t v);

static char g_report[8192];   // laporan teks (header + payload)
static char g_prev[8192];     // isi berkas lama, untuk membandingkan

// Data pemberitahuan (lihat include/crash_notice.h). g_new_report hanya 1 pada
// boot yang BENAR-BENAR menerbitkan isi baru — supaya notifikasi desktop tidak
// muncul berulang di setiap startup.
static int            g_new_report = 0;
static crash_notice_t g_notice;

static uint32_t str_len(const char* s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int str_same(const char* a, const char* b) {
    uint32_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

// Buang berkas lama (create tidak menimpa berkas yang sudah ada).
static void remove_if_present(const char* path) {
    if (kfs_exists((char*)path)) kfs_delete_file((char*)path);
}

void crash_archive_publish(void) {
    if (!crashdump_ready()) return;

    uint32_t len = crashdump_format_report(g_report, sizeof(g_report));
    if (len == 0) {
        // Normal: boot tanpa crash sebelumnya.
        serial_print("[CRASH] tidak ada dump valid di area ekor disk\n");
        return;
    }

    // Simpan ringkasan untuk notifikasi (dipakai syscall SYS_CRASH_NOTICE).
    {
        crashdump_hdr_t h;
        if (crashdump_read_last(&h, NULL, 0, NULL)) {
            for (uint32_t i = 0; i < sizeof(g_notice); i++) ((char*)&g_notice)[i] = 0;
            g_notice.crash_count  = h.dump_count;
            g_notice.uptime_ms    = h.timestamp_ms;
            g_notice.vector       = h.exception_vector;
            g_notice.rip          = h.rip;
            g_notice.cr2          = h.cr2;
            g_notice.task_id      = (h.task_id == 0xFFFFFFFFu) ? 0u : (uint32_t)h.task_id;
            for (uint32_t i = 0; i + 1u < sizeof(g_notice.task_name) && h.task_name[i]; i++)
                g_notice.task_name[i] = h.task_name[i];
            for (uint32_t i = 0; i + 1u < sizeof(g_notice.path) && CRASH_ARCHIVE_PATH[i]; i++)
                g_notice.path[i] = CRASH_ARCHIVE_PATH[i];
        }
    }

    const char* path = CRASH_ARCHIVE_PATH;

    // Sudah terbit dengan isi yang sama? Maka tidak perlu menulis disk.
    if (kfs_exists((char*)path)) {
        uint32_t sz = kfs_get_file_size((char*)path);
        if (sz == len && sz + 1u < sizeof(g_prev) &&
            kfs_read_to_buffer((char*)path, g_prev, sizeof(g_prev)) &&
            str_same(g_prev, g_report)) {
            serial_print("[CRASH] arsip sudah terbaru di ");
            serial_print(path);
            serial_print(" (");
            serial_dec(len);
            serial_print(" byte)\n");
            return;
        }
    }

    remove_if_present(path);
    if (!kfs_create_file((char*)path, g_report, len)) {
        serial_print("[CRASH] GAGAL menulis ");
        serial_print(path);
        serial_print("\n");
        return;
    }

    g_new_report = 1;   // laporan BARU -> desktop boleh memberi notifikasi
    serial_print("[CRASH] crashdump diterbitkan sebagai ");
    serial_print(path);
    serial_print(" (");
    serial_dec(str_len(g_report));
    serial_print(" byte) - bisa dibuka dari fileman/viewer\n");
}

int crash_archive_notice(crash_notice_t* out) {
    if (!out) return 0;
    *out = g_notice;            // salinan struct; syscall yang men-copy ke user
    out->pending = (uint32_t)(g_new_report ? 1 : 0);
    return (int)out->pending;
}
