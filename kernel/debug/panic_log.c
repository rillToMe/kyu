// kernel/debug/panic_log.c — crash log persisten di RAM reserved.
//
// MENGAPA BERTAHAN LINTAS WARM-REBOOT
//   * Area = SATU halaman fisik yang direservasi via pmm_alloc_page() sebagai
//     alokasi PERTAMA saat boot (kernel.c, tepat setelah init_paging).
//     pmm_init_dynamic() mengunci 72MB pertama sebagai kernel zone dan menaruh
//     next_free_hint di sana, jadi alokasi pertama SELALU halaman fisik yang
//     sama di setiap boot — log dari boot sebelumnya ada di tempat yang sama.
//   * Warm reset (8042 0xFE / triple fault) tidak menghapus DRAM.
//   * Halaman itu ditandai "used" di bitmap PMM, jadi tidak ada subsistem lain
//     yang memakainya selama boot berjalan.
//   * Cold boot (power cycle) memang menghapus RAM — untuk itu ada crashdump
//     ke disk (kernel/debug/crashdump.c). Keduanya saling melengkapi.
//
// KEAMANAN / BATASAN
//   * Zero-dynamic-allocation dan TANPA lock: hanya menulis field struct di
//     area yang sudah diketahui (memcpy biasa).
//   * Checksum: isi RAM yang korup / bukan log kita ditolak, dan flag-nya
//     dibersihkan supaya tidak dilaporkan berulang tiap boot.
//   * Hanya crash TERAKHIR yang disimpan (satu record, "ringkas"); jumlah
//     crash total dilacak di crash_count.

#include <stdint.h>
#include "panic.h"
#include "serial.h"

extern void kprint(const char* s);

static uint64_t g_log_phys  = 0;
static uint32_t g_log_len   = 0;
static int      g_log_ready = 0;

// =====================================================================
// Akses area
// =====================================================================
static panic_log_t* log_area(void) {
#ifdef PANIC_HOST_TEST
    // Host test: argumen reserved_phys adalah pointer biasa.
    return (panic_log_t*)(uintptr_t)g_log_phys;
#else
    extern uint64_t hhdm_offset;
    return (panic_log_t*)(uintptr_t)(hhdm_offset + g_log_phys);
#endif
}

// Sum sederhana seluruh byte struct KECUALI field checksum itu sendiri.
static uint32_t log_checksum(const panic_log_t* l) {
    const uint8_t* p = (const uint8_t*)l;
    uint32_t skip_lo = (uint32_t)((const uint8_t*)&l->checksum - p);
    uint32_t skip_hi = skip_lo + 4u;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < sizeof(panic_log_t); i++) {
        if (i >= skip_lo && i < skip_hi) continue;
        sum += (uint32_t)p[i] * (i + 1u);      // bobot per posisi
    }
    return sum;
}

static void log_store(panic_log_t* l) { l->checksum = log_checksum(l); }

// =====================================================================
// Formatter kecil (tanpa printf, tanpa alokasi)
// =====================================================================
static uint32_t put_str(char* b, uint32_t cap, uint32_t at, const char* s) {
    while (s && *s && at + 1u < cap) b[at++] = *s++;
    if (at < cap) b[at] = '\0';
    return at;
}
static uint32_t put_dec(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    char t[24];
    uint32_t n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && at + 1u < cap) b[at++] = t[--n];
    if (at < cap) b[at] = '\0';
    return at;
}
static uint32_t put_hex(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    static const char* d = "0123456789ABCDEF";
    char t[18];
    for (int i = 15; i >= 0; i--) { t[i] = d[v & 0xF]; v >>= 4; }
    at = put_str(b, cap, at, "0x");
    for (int i = 0; i < 16 && at + 1u < cap; i++) b[at++] = t[i];
    if (at < cap) b[at] = '\0';
    return at;
}

// Label vector yang bisa dibaca manusia (tanpa duplikasi tabel panic.c).
static const char* vec_name(uint64_t v) {
    switch (v) {
        case 0:      return "DIVIDE BY ZERO";
        case 6:      return "INVALID OPCODE";
        case 8:      return "DOUBLE FAULT";
        case 13:     return "GENERAL PROTECTION";
        case 14:     return "PAGE FAULT";
        case 0xFFFF: return "kernel_panic() manual";
        default:     return "exception";
    }
}

// =====================================================================
// API
// =====================================================================
void panic_log_init(uint64_t reserved_phys, uint64_t reserved_len) {
    if (!reserved_phys || reserved_len < (uint64_t)sizeof(panic_log_t)) {
        g_log_ready = 0;
        return;
    }
    g_log_phys  = reserved_phys;
    g_log_len   = (uint32_t)reserved_len;
    g_log_ready = 1;
    // SENGAJA tidak di-zero: isi log dari boot sebelumnya harus tetap terbaca
    // oleh panic_check_previous_log(). Record asing akan ditolak checksum.
}

void panic_log_write(uint64_t exception_vector, uint64_t error_code, uint64_t rip,
                     uint64_t cr2, uint64_t timestamp_ms, uint64_t task_id,
                     const char* task_name, int kind) {
    if (!g_log_ready) return;
    panic_log_t* l = log_area();

    // Lanjutkan hitungan kalau record sebelumnya valid (jangan percaya isi RAM
    // yang tidak lolos checksum).
    uint32_t count = 0;
    if (l->signature == PANIC_LOG_SIGNATURE && l->checksum == log_checksum(l))
        count = l->crash_count;

    l->version          = PANIC_LOG_VERSION;
    l->crash_count      = count + 1u;
    l->timestamp        = timestamp_ms;
    l->exception_vector = exception_vector;
    l->error_code       = error_code;
    l->rip              = rip;
    l->cr2              = cr2;
    l->task_id          = task_id;
    l->kind             = (uint32_t)kind;

    uint32_t n = 0;
    while (task_name && task_name[n] && n + 1u < sizeof(l->task_name)) {
        l->task_name[n] = task_name[n];
        n++;
    }
    l->task_name[n] = '\0';

    l->signature = PANIC_LOG_SIGNATURE;   // valid paling akhir
    log_store(l);
}

int panic_check_previous_log(void) {
    if (!g_log_ready) return 0;
    panic_log_t* l = log_area();

    if (l->signature != PANIC_LOG_SIGNATURE) return 0;
    if (l->version != PANIC_LOG_VERSION || l->checksum != log_checksum(l)) {
        l->signature = PANIC_LOG_CONSUMED;   // format asing / isi korup
        return 0;
    }

    char b[512];
    uint32_t at = 0;
    at = put_str(b, sizeof(b), at, "[PANIC_LOG] crash pada boot sebelumnya terdeteksi\n");
    at = put_str(b, sizeof(b), at, "            jenis   : ");
    at = put_str(b, sizeof(b), at, vec_name(l->exception_vector));
    at = put_str(b, sizeof(b), at, " (vector ");
    at = put_dec(b, sizeof(b), at, l->exception_vector);
    at = put_str(b, sizeof(b), at, "), err ");
    at = put_hex(b, sizeof(b), at, l->error_code);
    at = put_str(b, sizeof(b), at, "\n            rip     : ");
    at = put_hex(b, sizeof(b), at, l->rip);
    at = put_str(b, sizeof(b), at, "   cr2 : ");
    at = put_hex(b, sizeof(b), at, l->cr2);
    at = put_str(b, sizeof(b), at, "\n            task    : #");
    at = put_dec(b, sizeof(b), at, l->task_id);
    at = put_str(b, sizeof(b), at, " \"");
    at = put_str(b, sizeof(b), at, l->task_name[0] ? l->task_name : "(tanpa nama)");
    at = put_str(b, sizeof(b), at, "\"\n            uptime  : ");
    at = put_dec(b, sizeof(b), at, l->timestamp / 1000u);
    at = put_str(b, sizeof(b), at, " s   (panic ke-");
    at = put_dec(b, sizeof(b), at, l->crash_count);
    at = put_str(b, sizeof(b), at, ")\n");

    serial_print(b);
    kprint(b);

    // Bersihkan flag supaya tidak dilaporkan berulang di boot berikutnya.
    l->signature = PANIC_LOG_CONSUMED;
    log_store(l);
    return 1;
}
