// kernel/debug/crashdump.c — snapshot panic ke sektor mentah disk.
//
// KENAPA FILE TERPISAH DARI panic.c
//   panic.c menggambar layar + menyusun teks snapshot (tahu soal register dan
//   penyebab fault); file ini hanya urusan I/O disk + integritas data. Batas
//   itu bikin writer-nya bisa diuji terpisah (test/panic_test.c) dengan ATA
//   yang di-mock.
//
// PRINSIP (semuanya wajib untuk konteks panic)
//   * POLLING ATA PIO (drivers/ata.c) — tidak ada interrupt, tidak ada
//     scheduler/thread, tidak menunggu lock apa pun.
//   * ZERO-DYNAMIC-ALLOCATION: satu buffer staging statis 4KB di BSS.
//   * AREA KHUSUS: CRASHDUMP_SECTORS (8 = 4KB) sektor di EKOR disk. KyuzenFS
//     berhenti sebelum area ini (KZFS_CRASHDUMP_SECTORS di
//     include/kyuzenfs_v4.h), jadi snapshot TIDAK menimpa data file.
//   * RECURSION GUARD: kalau panic baru terjadi saat dump berjalan, panic.c
//     memanggil crashdump_abort() → dump berhenti di antara sektor dan sistem
//     jatuh ke freeze/halt. Panggilan dump kedua saat masih aktif gagal cepat
//     (-2) alih-alih menulis dua snapshot bertumpuk.
//   * VERIFIKASI: sektor pertama dibaca ulang; kalau tidak cocok, dump
//     dilaporkan gagal (jangan mengklaim punya artefak yang tidak ada).

#include <stdint.h>
#include "crashdump.h"
#include "ata.h"

// Staging 4KB di BSS (bukan stack: konteks panic bisa saja sudah kehabisan
// stack, dan 4KB terlalu besar untuk frame ISR).
static uint8_t  g_buf[CRASHDUMP_MAX_BYTES];

static uint64_t g_lba       = 0;
static uint32_t g_sectors   = 0;
static int      g_ready     = 0;
static uint32_t g_dump_count = 0;

static volatile int g_active = 0;   // 1 = dump sedang berjalan
static volatile int g_abort  = 0;   // 1 = minta dump berhenti (panic bersarang)

// =====================================================================
// Helper
// =====================================================================
static void mem_zero(uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}
static void mem_copy(uint8_t* dst, const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

// Checksum payload: FNV-1a 32-bit. Sektor yang rusak/terpotong hampir pasti
// menghasilkan nilai berbeda.
static uint32_t sum32(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint32_t s = 0x811C9DC5u;
    for (uint32_t i = 0; i < len; i++) {
        s ^= (uint32_t)b[i];
        s *= 16777619u;
    }
    return s;
}

// =====================================================================
// API
// =====================================================================
void crashdump_init(uint64_t crash_start_lba, uint32_t crash_sectors) {
    g_ready = 0;
    g_lba = crash_start_lba;
    g_sectors = crash_sectors;

    if (!crash_start_lba || crash_sectors == 0) return;
    if (g_sectors > CRASHDUMP_SECTORS) g_sectors = CRASHDUMP_SECTORS;   // maks 4KB
    g_ready = 1;

    // Baca header dump sebelumnya (kalau ada) supaya dump_count berlanjut.
    uint8_t sec[512];
    ata_read_sector((uint32_t)g_lba, sec);
    const crashdump_hdr_t* h = (const crashdump_hdr_t*)sec;
    if (h->magic == CRASHDUMP_MAGIC && h->version == CRASHDUMP_VERSION &&
        h->dump_count != 0xFFFFFFFFu) {
        g_dump_count = h->dump_count;
    }
}

int crashdump_ready(void) { return g_ready; }
int crashdump_is_active(void) { return g_active; }
uint64_t crashdump_start_lba(void) { return g_lba; }

// =====================================================================
// PEMBACAAN KEMBALI + LAPORAN TEKS
//
// Bagian ini dipakai dari konteks NORMAL (boot/shell), bukan jalur panic:
// tujuannya memindahkan isi area ekor disk menjadi berkas yang bisa dibuka
// aplikasi (viewer/notepad), bukan cuma dibaca di terminal.
// =====================================================================
static uint32_t rep_str(char* b, uint32_t cap, uint32_t at, const char* s) {
    while (s && *s && at + 1u < cap) b[at++] = *s++;
    if (at < cap) b[at] = '\0';
    return at;
}
static uint32_t rep_dec(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    char t[24];
    uint32_t n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && at + 1u < cap) b[at++] = t[--n];
    if (at < cap) b[at] = '\0';
    return at;
}
static uint32_t rep_hex(char* b, uint32_t cap, uint32_t at, uint64_t v, uint32_t digits) {
    static const char* d = "0123456789ABCDEF";
    at = rep_str(b, cap, at, "0x");
    for (int i = (int)digits - 1; i >= 0 && at + 1u < cap; i--)
        b[at++] = d[(v >> (i * 4)) & 0xF];
    if (at < cap) b[at] = '\0';
    return at;
}

int crashdump_read_last(crashdump_hdr_t* out_hdr, void* out_payload,
                        uint32_t cap, uint32_t* out_len) {
    if (!g_ready) return 0;

    uint8_t sec[512];
    ata_read_sector((uint32_t)g_lba, sec);
    crashdump_hdr_t h;
    mem_copy((uint8_t*)&h, sec, (uint32_t)sizeof(h));
    if (h.magic != CRASHDUMP_MAGIC || h.version != CRASHDUMP_VERSION) return 0;
    if (h.sectors == 0 || h.sectors > g_sectors) return 0;
    if (h.length > (CRASHDUMP_MAX_BYTES - (uint32_t)sizeof(crashdump_hdr_t))) return 0;

    // Validasi isi: checksum payload harus cocok (dump terpotong/rusak ditolak).
    static uint8_t pay[CRASHDUMP_MAX_BYTES - sizeof(crashdump_hdr_t)];
    uint32_t off = (uint32_t)sizeof(crashdump_hdr_t);
    for (uint32_t i = 0; i < h.length; i++) {
        uint32_t idx = off + i;
        if ((idx % 512u) == 0u) ata_read_sector((uint32_t)(g_lba + (idx / 512u)), sec);
        pay[i] = sec[idx % 512u];
    }
    if (sum32(pay, h.length) != h.checksum) return 0;

    if (out_hdr) *out_hdr = h;
    if (out_len) *out_len = h.length;
    if (out_payload && cap) {
        uint32_t n = h.length < cap - 1u ? h.length : cap - 1u;
        mem_copy((uint8_t*)out_payload, pay, n);
        ((char*)out_payload)[n] = '\0';
    }
    return 1;
}

uint32_t crashdump_format_report(char* out, uint32_t cap) {
    if (!out || cap < 64u) return 0;
    out[0] = '\0';

    crashdump_hdr_t h;
    static char payload[CRASHDUMP_MAX_BYTES];
    uint32_t len = 0;
    if (!crashdump_read_last(&h, payload, sizeof(payload), &len)) return 0;

    uint32_t at = 0;
    at = rep_str(out, cap, at, "KyuzenOS - laporan crashdump\n");
    at = rep_str(out, cap, at, "===========================\n");
    at = rep_str(out, cap, at, "waktu      : " );
    at = rep_dec(out, cap, at, h.timestamp_ms / 1000u);
    at = rep_str(out, cap, at, " s uptime (");
    at = rep_dec(out, cap, at, h.timestamp_ms);
    at = rep_str(out, cap, at, " ms)\n");
    at = rep_str(out, cap, at, "jenis      : vector ");
    at = rep_dec(out, cap, at, h.exception_vector);
    at = rep_str(out, cap, at, h.exception_vector == 0xFFFFu
                              ? "  (kernel_panic() manual)\n" : "  (exception CPU)\n");
    at = rep_str(out, cap, at, "error code : ");
    at = rep_hex(out, cap, at, h.error_code, 16u);
    at = rep_str(out, cap, at, "\nrip        : ");
    at = rep_hex(out, cap, at, h.rip, 16u);
    at = rep_str(out, cap, at, "\ncr2        : ");
    at = rep_hex(out, cap, at, h.cr2, 16u);
    at = rep_str(out, cap, at, "\nrsp        : ");
    at = rep_hex(out, cap, at, h.rsp, 16u);
    at = rep_str(out, cap, at, "\ncs         : ");
    at = rep_hex(out, cap, at, h.cs, 4u);
    at = rep_str(out, cap, at, "\ncr3        : ");
    at = rep_hex(out, cap, at, h.cr3, 16u);
    at = rep_str(out, cap, at, "\ntask       : #");
    at = rep_dec(out, cap, at, h.task_id);
    at = rep_str(out, cap, at, " \"");
    at = rep_str(out, cap, at, h.task_name);
    at = rep_str(out, cap, at, "\"\ndump ke-   : ");
    at = rep_dec(out, cap, at, h.dump_count);
    at = rep_str(out, cap, at, "\nchecksum   : ");
    at = rep_hex(out, cap, at, h.checksum, 8u);
    at = rep_str(out, cap, at, "\nraw        : LBA ");
    at = rep_dec(out, cap, at, g_lba);
    at = rep_str(out, cap, at, " + ");
    at = rep_dec(out, cap, at, h.sectors);
    at = rep_str(out, cap, at, " sektor (mentah, di ekor disk)\n\n");
    at = rep_str(out, cap, at, "--- isi laporan lengkap dari jalur panic ---\n");
    at = rep_str(out, cap, at, payload);
    at = rep_str(out, cap, at, "\n");
    return at;
}

void crashdump_abort(void) {
    if (g_active) g_abort = 1;
}

int crashdump_write_snapshot(const crashdump_hdr_t* hdr, const void* payload, uint32_t len) {
    if (!g_ready || !hdr) return -1;
    if (g_active) return -2;              // recursion guard: satu dump saja
    g_active = 1;
    g_abort  = 0;

    const uint8_t* pay = (const uint8_t*)payload;
    uint32_t pay_max = CRASHDUMP_MAX_BYTES - (uint32_t)sizeof(crashdump_hdr_t);

    uint16_t flags = 0;
    if (!pay) len = 0;
    if (len > pay_max) { len = pay_max; flags |= CRASHDUMP_FLAG_TRUNCATED; }

    // Susun header final di staging.
    crashdump_hdr_t h = *hdr;
    h.magic       = CRASHDUMP_MAGIC;
    h.version     = CRASHDUMP_VERSION;
    h.flags       = flags;
    h.length      = len;
    h.checksum    = sum32(pay, len);
    h.dump_count  = (++g_dump_count);
    h.sectors     = (uint32_t)((sizeof(h) + len + 511u) / 512u);
    if (h.sectors == 0) h.sectors = 1;
    if (h.sectors > g_sectors) h.sectors = g_sectors;

    mem_zero(g_buf, CRASHDUMP_MAX_BYTES);
    mem_copy(g_buf, (const uint8_t*)&h, (uint32_t)sizeof(h));
    if (len) mem_copy(&g_buf[sizeof(h)], pay, len);

    // Tulis sektor demi sektor; cek abort di antara sektor.
    for (uint32_t s = 0; s < h.sectors; s++) {
        if (g_abort) { g_active = 0; return -3; }
        ata_write_sector((uint32_t)(g_lba + s), &g_buf[s * 512u]);
    }

    // Verifikasi baca-balik (bukti dump benar-benar ada di disk).
    if (g_abort) { g_active = 0; return -3; }
    uint8_t sec[512];
    ata_read_sector((uint32_t)g_lba, sec);
    const crashdump_hdr_t* v = (const crashdump_hdr_t*)sec;
    if (v->magic != CRASHDUMP_MAGIC || v->dump_count != h.dump_count ||
        v->checksum != h.checksum) {
        g_active = 0;
        return -4;
    }

    g_active = 0;
    return 0;
}
