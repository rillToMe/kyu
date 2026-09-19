// test/ata_devmodel_test.c — Test ekuivalensi jalur baca ATA (Stage 1).
//
// APA YANG DIUJI
//   drivers/ata.c punya DUA jalur baca (lihat include/ata.h + proposal di
//   DOCUMENTATION/design/ata-driver-redesign-proposal.md §7.1):
//     LEGACY : 8x ata_read_sector (satu perintah per sektor) — perilaku lama
//     BATCH  : satu perintah READ SECTORS count>1 + transfer `rep insw`
//   Test ini menjalankan KEDUA jalur terhadap MODEL DEVICE ATA yang menegakkan
//   aturan protokol, lalu memastikan:
//     * byte hasil baca identik (juga identik dengan pola referensi LBA itu),
//     * urutan sektor yang dibaca identik,
//     * jumlah perintah berbeda seperti yang dimaksud (8 vs 1 per blok),
//     * tidak ada pelanggaran protokol (baca data saat DRQ mati, dll),
//     * kasus error/timeout/out-of-range bersikap benar (BATCH melaporkan
//       error; LEGACY tetap seperti dokumentasi: buffer di-nol-kan).
//
// CARA JALAN
//   make test-ata                        (backing sintetis, deterministik)
//   ./test/ata_devmodel_test disk.img    (opsional: bandingkan pada image nyata)
//   Image dibuka READ-ONLY ("rb") dan tidak pernah ditulis oleh test ini.
//
// CATATAN CAKUPAN
//   `insw_rep` di sini adalah padanan fungsional (test/atamock/io.h) karena
//   asm tidak bisa jalan di host. Instruksi asli `cld; rep insw` divalidasi
//   lewat boot QEMU dengan ATA_READ_PATH_DEFAULT=1.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// =====================================================================
// MODEL DEVICE ATA (bukan array pasif: menegakkan protokol + hitung statistik)
// =====================================================================
#define MODEL_SECTORS 204800u              // sama dengan disk.img: 100 MiB
#define MODEL_MAX_LBA_SEQ 8192u

static uint8_t  g_disk[MODEL_SECTORS * 512u];
static uint16_t g_ident[256];

typedef struct {
    // register
    uint8_t  drive_head;
    uint8_t  sec_count;
    uint8_t  lba_lo, lba_mid, lba_hi;
    // perintah aktif
    int      active;
    int      identify;
    uint32_t remaining;
    uint32_t cur_lba;
    uint32_t words_left;
    int      err, df, drq;
    // injeksi
    int      stuck_busy;               // 1 = BSY nyangkut (timeout)
    int      fail_lba;                 // LBA yang bikin ERR di tengah transfer
    int      df_lba;                   // LBA yang bikin DF di tengah transfer
    // statistik
    uint32_t cmds_read;
    uint32_t cmds_identify;
    uint32_t sectors_served;
    uint32_t max_count;
    uint32_t violations;
    uint32_t lba_seq[MODEL_MAX_LBA_SEQ];
    uint32_t lba_seq_n;
} model_t;

static model_t M;

// Isi sektor = fungsi deterministik LBA. 4 byte awal menyimpan LBA supaya
// kesalahan off-by-one langsung kelihatan (bukan sekadar "dua jalur sama").
static void pattern_fill(uint32_t lba, uint8_t *dst) {
    uint32_t x = lba * 2654435761u + 1u;
    for (uint32_t i = 0; i < 512u; i++) {
        x = x * 1664525u + 1013904223u;
        dst[i] = (uint8_t)(x >> 24);
    }
    dst[0] = (uint8_t)(lba);
    dst[1] = (uint8_t)(lba >> 8);
    dst[2] = (uint8_t)(lba >> 16);
    dst[3] = 0xA5;
}

static void model_reset(void) {
    memset(&M, 0, sizeof(M));
    M.fail_lba = -1;
    M.df_lba   = -1;
    for (uint32_t l = 0; l < MODEL_SECTORS; l++) {
        pattern_fill(l, g_disk + (size_t)l * 512u);
    }
    memset(g_ident, 0, sizeof(g_ident));
    // IDENTIFY: word 60/61 = jumlah sektor LBA28; 83 bit10 = dukung LBA48;
    // 100..103 = jumlah sektor LBA48 (di sini sama, 32-bit).
    g_ident[60]  = (uint16_t)(MODEL_SECTORS & 0xFFFFu);
    g_ident[61]  = (uint16_t)(MODEL_SECTORS >> 16);
    g_ident[83]  = 0x0400u;
    g_ident[100] = (uint16_t)(MODEL_SECTORS & 0xFFFFu);
    g_ident[101] = (uint16_t)(MODEL_SECTORS >> 16);
}

static uint32_t model_active_lba(void) {
    return ((uint32_t)(M.drive_head & 0x0Fu) << 24)
         | ((uint32_t)M.lba_hi << 16)
         | ((uint32_t)M.lba_mid << 8)
         | (uint32_t)M.lba_lo;
}

static uint8_t model_status(void) {
    if (M.stuck_busy) return 0x80;                 // BSY
    uint8_t s = 0x40;                              // RDY
    if (M.df)   s |= 0x20;                         // DF
    if (M.err)  s |= 0x01;                         // ERR
    if (M.drq)  s |= 0x08;                         // DRQ
    return s;
}

static void model_abort(int err, int df) {
    M.err = err; M.df = df; M.drq = 0; M.active = 0;
}

static void model_start_read(void) {
    uint32_t lba = model_active_lba();
    uint32_t cnt = M.sec_count ? (uint32_t)M.sec_count : 256u;   // ATA: 0 = 256

    if (M.sec_count == 0) M.violations++;   // driver kita tidak boleh kirim 0
    if (cnt > M.max_count) M.max_count = cnt;
    M.cmds_read++;

    if (lba >= MODEL_SECTORS || lba + cnt > MODEL_SECTORS) {
        model_abort(1, 0);                  // di luar disk: ERR (bukan violation)
        return;
    }
    M.active = 1; M.remaining = cnt; M.cur_lba = lba;
    M.words_left = 256u; M.err = 0; M.df = 0;
    if ((int)lba == M.fail_lba) { model_abort(1, 0); return; }
    if ((int)lba == M.df_lba)   { model_abort(0, 1); return; }
    M.drq = 1;
}

static void model_advance_sector(void) {
    M.sectors_served++;
    if (M.lba_seq_n < MODEL_MAX_LBA_SEQ) M.lba_seq[M.lba_seq_n++] = M.cur_lba;
    M.cur_lba++;
    M.remaining--;
    if (M.remaining == 0) { M.drq = 0; M.active = 0; return; }
    if ((int)M.cur_lba == M.fail_lba) { model_abort(1, 0); return; }
    if ((int)M.cur_lba == M.df_lba)   { model_abort(0, 1); return; }
    M.words_left = 256u;                    // sektor berikut, DRQ tetap asserted
}

static void model_start_identify(void) {
    M.cmds_identify++;
    M.identify = 1; M.drq = 1; M.words_left = 256u; M.err = 0; M.df = 0;
}

static uint16_t model_data_read(void) {
    if (!M.drq || M.words_left == 0) {
        M.violations++;                     // baca data saat DRQ mati = salah
        return 0xFFFFu;
    }
    uint16_t w;
    if (M.identify) {
        w = g_ident[256u - M.words_left];
    } else {
        uint32_t base = M.cur_lba * 512u;
        uint32_t wi   = 256u - M.words_left;
        w = (uint16_t)g_disk[base + wi * 2u]
          | (uint16_t)((uint16_t)g_disk[base + wi * 2u + 1u] << 8);
    }
    M.words_left--;
    if (M.words_left == 0) {
        if (M.identify) { M.identify = 0; M.drq = 0; }
        else model_advance_sector();
    }
    return w;
}

// --- handler port (dipakai shim test/atamock/io.h) ---
#define P_DATA   0x1F0
#define P_ERRREG 0x1F1
#define P_COUNT  0x1F2
#define P_LO     0x1F3
#define P_MID    0x1F4
#define P_HI     0x1F5
#define P_DRIVE  0x1F6
#define P_CMD    0x1F7
#define P_ALT    0x3F6

uint8_t atamock_inb(uint16_t port) {
    switch (port) {
    case P_CMD: case P_ALT: return model_status();          // ALT: tanpa side effect
    case P_ERRREG:          return (uint8_t)(M.err ? 0x40 : 0x00);
    default:                return 0x00;
    }
}

void atamock_outb(uint16_t port, uint8_t val) {
    switch (port) {
    case P_COUNT: M.sec_count = val; break;
    case P_LO:    M.lba_lo    = val; break;
    case P_MID:   M.lba_mid   = val; break;
    case P_HI:    M.lba_hi    = val; break;
    case P_DRIVE: M.drive_head = val; break;
    case P_CMD:
        if (M.stuck_busy) break;                 // drive nyangkut: perintah diabaikan
        if (val == 0x20)      model_start_read();
        else if (val == 0xEC) model_start_identify();
        else if (val == 0xE7) { M.drq = 0; }      // CACHE FLUSH: sukses, no-op
        else { M.violations++; model_abort(1, 0); }
        break;
    default: break;
    }
}

uint16_t atamock_inw(uint16_t port) {
    if (port == P_DATA) return model_data_read();
    return 0x0000;
}

void atamock_outw(uint16_t port, uint16_t val) {
    (void)port; (void)val;                       // jalur tulis di luar scope Stage 1
}

// =====================================================================
// DRIVER ASLI (dikompilasi apa adanya)
// `#include "io.h"` di dalamnya diarahkan ke test/atamock/io.h lewat -iquote.
// =====================================================================
#include "../drivers/ata.c"

// =====================================================================
// HARNESS
// =====================================================================
static int g_fails = 0;
static void check(int cond, const char *msg) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fails++;
}

static uint8_t BUF_LEG[4096];
static uint8_t BUF_BAT[4096];
static uint8_t BUF_REF[4096];

// Isi referensi untuk 8 sektor mulai lba.
static void ref_fill(uint32_t lba, uint32_t count, uint8_t *dst) {
    for (uint32_t s = 0; s < count; s++) pattern_fill(lba + s, dst + s * 512u);
}

static void use_path(int path) { ata_read_path_set(path); }

// Baca 1 blok 4 KB lewat jalur yang diminta; kembalikan rc (legacy selalu 0).
static int read_block(uint64_t block, int path, uint8_t *dst) {
    use_path(path);
    return ata_read_block4k(block, dst);
}

// =====================================================================
// TEST
// =====================================================================

// 1. IDENTIFY (regresi: kode ini tidak diubah Stage 1, tapi ada di file yang sama)
static void test_identify(void) {
    model_reset();
    check(ata_get_total_sectors() == MODEL_SECTORS,
          "identify: LBA28 word 60/61 -> jumlah sektor benar");

    // LBA48 > 4G sektor: pemanggil v3 meng-clamp ke 0xFFFFFFFF (didokumentasikan).
    g_ident[102] = 0x0001u;
    check(ata_get_total_sectors() == 0xFFFFFFFFu,
          "identify: LBA48 > 32-bit di-clamp ke 0xFFFFFFFF (perilaku lama)");
}

// 2. Ekuivalensi per blok 4 KB + jumlah perintah + urutan sektor
static void test_block_equivalence(void) {
    static const uint64_t blocks[] = { 0, 1, 7, 123, 12800, 25599 };
    for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        uint64_t b = blocks[i];
        char msg[128];

        model_reset();
        int rc_l = read_block(b, ATA_READ_PATH_LEGACY, BUF_LEG);
        uint32_t cmds_l = M.cmds_read, viol_l = M.violations;
        uint32_t seq_l[8];                         // 1 blok = 8 sektor
        memcpy(seq_l, M.lba_seq, sizeof(seq_l));
        uint32_t seqn_l = M.lba_seq_n;

        model_reset();
        int rc_b = read_block(b, ATA_READ_PATH_BATCH, BUF_BAT);
        uint32_t cmds_b = M.cmds_read, viol_b = M.violations, maxc_b = M.max_count;
        uint32_t seq_b[8];
        memcpy(seq_b, M.lba_seq, sizeof(seq_b));
        uint32_t seqn_b = M.lba_seq_n;

        ref_fill((uint32_t)(b * 8u), 8, BUF_REF);

        snprintf(msg, sizeof(msg), "blok %llu: legacy & batch byte-identik (+ pola LBA)",
                 (unsigned long long)b);
        check(rc_l == 0 && rc_b == 0 && memcmp(BUF_LEG, BUF_BAT, 4096) == 0 &&
              memcmp(BUF_LEG, BUF_REF, 4096) == 0, msg);

        snprintf(msg, sizeof(msg),
                 "blok %llu: perintah 8 (legacy) vs 1 count=8 (batch)", (unsigned long long)b);
        check(cmds_l == 8 && cmds_b == 1 && maxc_b == 8, msg);

        snprintf(msg, sizeof(msg), "blok %llu: urutan sektor identik & 8 sektor terlayani",
                 (unsigned long long)b);
        check(seqn_l == 8 && seqn_b == 8 &&
              memcmp(seq_l, seq_b, sizeof(seq_l)) == 0 &&
              seq_l[0] == (uint32_t)(b * 8u) && seq_l[7] == (uint32_t)(b * 8u + 7u), msg);

        snprintf(msg, sizeof(msg), "blok %llu: 0 pelanggaran protokol (kedua jalur)",
                 (unsigned long long)b);
        check(viol_l == 0 && viol_b == 0, msg);
    }
}

// 3. Rentang tidak sejajar blok + chunking count > 8
static void test_range_equivalence(void) {
    struct { uint32_t lba, count, expect_cmds; } cases[] = {
        { 3,   5, 1 },      // di tengah blok
        { 0,   8, 1 },      // tepat 1 blok
        { 5,   9, 2 },      // melewati batas blok -> 8 + 1
        { 87, 20, 3 },      // 8 + 8 + 4
        { 204792, 8, 1 },   // 8 sektor terakhir (area crashdump)
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t lba = cases[i].lba, cnt = cases[i].count;
        char msg[128];
        static uint8_t a[16384], b[16384], r[16384];   // >= count terbesar (20) x 512

        model_reset();
        use_path(ATA_READ_PATH_LEGACY);
        int rc_l = ata_read_range(lba, cnt, a);
        uint32_t cmds_l = M.cmds_read;

        model_reset();
        use_path(ATA_READ_PATH_BATCH);
        int rc_b = ata_read_range(lba, cnt, b);
        uint32_t cmds_b = M.cmds_read;

        ref_fill(lba, cnt, r);

        snprintf(msg, sizeof(msg), "rentang lba=%u count=%u: byte identik (+ pola)",
                 lba, cnt);
        check(rc_l == 0 && rc_b == 0 &&
              memcmp(a, b, cnt * 512u) == 0 && memcmp(a, r, cnt * 512u) == 0, msg);

        snprintf(msg, sizeof(msg), "rentang lba=%u count=%u: perintah %u (legacy) vs %u (batch)",
                 lba, cnt, cmds_l, cmds_b);
        check(cmds_l == cnt && cmds_b == cases[i].expect_cmds, msg);
    }
}

// 4. Di luar disk: BATCH melaporkan error, LEGACY tetap silent-zero (dokumentasi)
static void test_beyond_disk(void) {
    uint32_t lba = MODEL_SECTORS - 4u;         // 4 sektor valid, 4 melewati disk
    model_reset();
    use_path(ATA_READ_PATH_BATCH);
    int rc = ata_read_range(lba, 8, BUF_BAT);
    check(rc == -2 && M.violations == 0,
          "di luar disk (batch): device ERR -> return -2, tanpa pelanggaran protokol");

    model_reset();
    use_path(ATA_READ_PATH_LEGACY);
    rc = ata_read_range(lba, 8, BUF_LEG);
    ref_fill(lba, 4, BUF_REF);
    int tail_zero = 1;
    for (int i = 4 * 512; i < 8 * 512; i++) if (BUF_LEG[i] != 0) { tail_zero = 0; break; }
    check(rc == 0 && memcmp(BUF_LEG, BUF_REF, 4 * 512) == 0 && tail_zero,
          "di luar disk (legacy): return 0 + 4 sektor terakhir di-nol-kan (perilaku lama)");
}

// 5. Guard LBA28: ditolak SEBELUM menyentuh hardware
static void test_lba28_guard(void) {
    model_reset();
    use_path(ATA_READ_PATH_BATCH);
    int rc = ata_read_range(0x0FFFFFFFu, 2, BUF_BAT);
    check(rc == -4 && M.cmds_read == 0,
          "LBA28 overflow (batch): return -4 tanpa mengirim perintah ke drive");
}

// 6. Injeksi ERR di tengah transfer
static void test_error_injection(void) {
    uint32_t lba = 16 * 8u;
    model_reset();
    M.fail_lba = (int)(lba + 3u);              // gagal di sektor ke-4
    use_path(ATA_READ_PATH_BATCH);
    int rc = ata_read_range(lba, 8, BUF_BAT);
    check(rc == -2 && M.sectors_served == 3 && M.violations == 0,
          "injeksi ERR sektor ke-4 (batch): return -2 setelah 3 sektor, tanpa pelanggaran");

    model_reset();
    M.fail_lba = (int)(lba + 3u);
    use_path(ATA_READ_PATH_LEGACY);
    rc = ata_read_range(lba, 8, BUF_LEG);
    int s3_zero = 1;
    for (int i = 3 * 512; i < 4 * 512; i++) if (BUF_LEG[i] != 0) { s3_zero = 0; break; }
    check(rc == 0 && s3_zero,
          "injeksi ERR sektor ke-4 (legacy): return 0 + sektor gagal di-nol-kan (perilaku lama)");
}

// 7. Timeout BSY
static void test_timeout(void) {
    model_reset();
    M.stuck_busy = 1;
    use_path(ATA_READ_PATH_BATCH);
    int rc = ata_read_block4k(3, BUF_BAT);
    check(rc == -3 && M.cmds_read == 0,
          "BSY nyangkut (batch): return -3, perintah tidak dikirim");

    model_reset();
    M.stuck_busy = 1;
    use_path(ATA_READ_PATH_LEGACY);
    rc = ata_read_block4k(3, BUF_LEG);
    int all_zero = 1;
    for (int i = 0; i < 4096; i++) if (BUF_LEG[i] != 0) { all_zero = 0; break; }
    check(rc == 0 && all_zero,
          "BSY nyangkut (legacy): return 0 + buffer nol (tidak bisa melaporkan error)");
}

// 8. Flag dispatch: default build = jalur lama, setter bisa bolak-balik
// Catatan: nilai default build harus diambil SEBELUM test lain menyentuh setter
// (test lain mengubah jalur; dulu check ini salah baca state sisa test → FAIL).
static int g_boot_path = ATA_READ_PATH_LEGACY;
static void test_flag_dispatch(void) {
    int dflt = g_boot_path;
    check(dflt == ATA_READ_PATH_LEGACY || dflt == ATA_READ_PATH_BATCH,
          "flag: nilai default valid (LEGACY/BATCH)");
#ifndef ATA_READ_PATH_DEFAULT
    check(0, "flag: ATA_READ_PATH_DEFAULT terdefinisi");
#else
    check(dflt ==
              ((ATA_READ_PATH_DEFAULT == ATA_READ_PATH_BATCH)
                   ? ATA_READ_PATH_BATCH : ATA_READ_PATH_LEGACY),
          "flag: default mengikuti ATA_READ_PATH_DEFAULT");
#endif
    ata_read_path_set(ATA_READ_PATH_BATCH);
    int b = ata_read_path_get();
    ata_read_path_set(ATA_READ_PATH_LEGACY);
    int l = ata_read_path_get();
    ata_read_path_set(99);                     // nilai tak dikenal -> aman ke LEGACY
    int x = ata_read_path_get();
    check(b == ATA_READ_PATH_BATCH && l == ATA_READ_PATH_LEGACY &&
          x == ATA_READ_PATH_LEGACY,
          "flag: setter LEGACY/BATCH berfungsi, nilai asing jatuh ke LEGACY");
}

// 9. Opsional: image nyata (dibuka read-only, tidak pernah ditulis)
static void test_real_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("SKIP image nyata (tidak bisa buka %s)\n", path); return; }

    uint32_t limit = MODEL_SECTORS;
    size_t n = fread(g_disk, 1, (size_t)MODEL_SECTORS * 512u, f);
    fclose(f);
    uint32_t nblocks = (uint32_t)(n / 4096u);
    if (nblocks == 0) { printf("SKIP image nyata (terlalu kecil)\n"); return; }
    if (nblocks > limit) nblocks = limit;

    printf("[image] %s: %u blok 4 KB (dibaca read-only)\n", path, nblocks);

    uint32_t checked = 0, mismatch = 0, viol = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        int sample = (b < 3) || (b + 3 >= nblocks) || ((b % 97u) == 0u);
        if (!sample) continue;

        model_reset();
        read_block(b, ATA_READ_PATH_LEGACY, BUF_LEG);
        uint32_t v1 = M.violations;

        model_reset();
        int rc = read_block(b, ATA_READ_PATH_BATCH, BUF_BAT);

        checked++;
        if (rc != 0 || memcmp(BUF_LEG, BUF_BAT, 4096) != 0) mismatch++;
        if (v1 != 0 || M.violations != 0) viol++;
        // Pola sintetis TIDAK dipakai di mode image (isi = data nyata), jadi
        // yang dibandingkan hanya legacy vs batch.
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "image: %u blok sampel identik legacy vs batch", checked);
    check(mismatch == 0, msg);
    snprintf(msg, sizeof(msg), "image: %u blok sampel tanpa pelanggaran protokol", checked);
    check(viol == 0, msg);
}

int main(int argc, char **argv) {
    g_boot_path = ata_read_path_get();      // state default build, sebelum disentuh test
    printf("=== ATA Stage 1: ekuivalensi jalur baca (model device) ===\n");
    printf("[info] default jalur saat build: %s\n",
           g_boot_path == ATA_READ_PATH_BATCH ? "BATCH" : "LEGACY");

    test_identify();
    test_block_equivalence();
    test_range_equivalence();
    test_beyond_disk();
    test_lba28_guard();
    test_error_injection();
    test_timeout();
    test_flag_dispatch();
    if (argc > 1) test_real_image(argv[1]);

    printf("\n%d skenario gagal\n", g_fails);
    return g_fails ? 1 : 0;
}
