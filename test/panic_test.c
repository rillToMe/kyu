// test/panic_test.c — host test BSOD (kernel/panic.c).
//
// Jalankan:            make test-panic
// Periksa tata letak:  ./test/panic_test --dump
//
// Cara kerja: kernel/panic.c di-include langsung dengan -DPANIC_HOST_TEST,
// jadi instruksi privileged (cli/sti/hlt/lidt/CR2/CR3/outb) menjadi no-op dan
// jam diganti g_panic_test_ms (tiap pembacaan countdown maju 1 detik). Semua
// dependensi kernel di-mock di sini. Yang diverifikasi:
//   * verdict/petunjuk untuk beragam fault (PENYEBAB harus menjawab, bukan
//     hanya dump angka),
//   * lockdown -> panic_is_locked() (compositor/mouse/timer memakai ini),
//   * countdown 10 detik yang TIDAK menggambar ulang baris penuh (anti-flicker),
//   * urutan flush FS -> reboot, dan hook reboot host.
//
// Layar BSOD diperiksa dengan men-decode framebuffer kembali menjadi teks
// memakai font8x16 asli — jadi hierarki/alignment ikut teruji, bukan hanya
// "fungsi ini dipanggil".

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Font asli: dipakai panic.c untuk menggambar dan test untuk men-decode balik.
// (header memakai nama makro FONT8x16_IMPLEMENTATION — huruf x kecil.)
#define FONT8x16_IMPLEMENTATION
#include "font8x16.h"

#include "display.h"
#include "task.h"
#include "panic.h"

// =====================================================================
// MOCK
// =====================================================================
#define FB_W 1024
#define FB_H 768
#define FB_PITCH (FB_W * 4)

static uint32_t FB[FB_W * FB_H];
uint32_t* fb_ptr = FB;

static display_mode_t g_mode = { FB_W, FB_H, FB_PITCH, 32, 0 };
const display_mode_t* display_get_mode(void) { return &g_mode; }

// --- serial: ditampung untuk diperiksa ---
#define SER_MAX 65536
static char SER[SER_MAX];
static size_t ser_len = 0;

void serial_print(const char* s) {
    while (s && *s && ser_len + 1 < SER_MAX) SER[ser_len++] = *s++;
    SER[ser_len] = '\0';
}

// --- paging: hanya area yang benar-benar kita alokasikan yang "ter-map",
//     supaya backtrace aman dibaca oleh proses host ---
static uint64_t g_fake_stack[1024];

int paging_is_mapped(uint64_t a) {
    uint64_t s = (uint64_t)(uintptr_t)g_fake_stack;
    uint64_t e = s + sizeof(g_fake_stack);
    return a >= s && a < e;
}

// --- SMP / task / PMM ---
static int g_test_task = 2;
static uint64_t g_cpu_index = 1;

uint32_t smp_current_cpu_index(void) { return (uint32_t)g_cpu_index; }
uint32_t smp_online_cpu_count(void)  { return 8; }
int      smp_current_task_id(void)   { return g_test_task; }

uint64_t pmm_get_used_pages(void)  { return 25216; }
uint64_t pmm_get_total_pages(void) { return 259893; }

int task_count = 3;
task_t tasks[MAX_TASKS];

volatile uint64_t g_last_syscall_num  = 4;
volatile int32_t  g_last_syscall_task = 1;

// --- FS flush: urutan flush -> reboot diperiksa ---
static int g_flushes = 0;
void kfs_sync_all(void) { g_flushes++; }

uint64_t timer_get_ms(void) { return 0; }

// --- reboot hook (dipasang lewat panic_set_reboot_hook) ---
static int g_reboot_hook_calls = 0;
static int g_flushes_at_reboot = -1;

static void test_reboot_hook(void) {
    g_reboot_hook_calls++;
    g_flushes_at_reboot = g_flushes;   // harus >= 1 (FS sudah diflush)
}

// =====================================================================
// panic.c (function/variabel static-nya juga jadi milik TU ini)
// =====================================================================
#include "../kernel/panic.c"

// =====================================================================
// DECODE FRAMEBUFFER -> TEKS (untuk memeriksa tata letak)
// =====================================================================
// Tabel glyph: prefix 2 baris pertama -> char. Ambigu (2 glyph dengan prefix
// sama) ditandai -2 dan diselesaikan dengan pencarian linear.
static int glyph_by_prefix[256][256];
static int glyph_table_ready = 0;

static void glyph_table_build(void) {
    if (glyph_table_ready) return;
    glyph_table_ready = 1;
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++) glyph_by_prefix[a][b] = -1;
    for (int ch = 0; ch < 256; ch++) {
        int k0 = font8x16[ch][0], k1 = font8x16[ch][1];
        if (glyph_by_prefix[k0][k1] == -1) glyph_by_prefix[k0][k1] = ch;
        else glyph_by_prefix[k0][k1] = -2;
    }
}

static int glyph_match(const uint8_t bits[16]) {
    glyph_table_build();
    int cand = glyph_by_prefix[bits[0]][bits[1]];
    if (cand >= 0) {
        int ok = 1;
        for (int r = 0; r < 16; r++) if (font8x16[cand][r] != bits[r]) { ok = 0; break; }
        if (ok) return cand;
    }
    for (int ch = 0; ch < 256; ch++) {
        int ok = 1;
        for (int r = 0; r < 16; r++) if (font8x16[ch][r] != bits[r]) { ok = 0; break; }
        if (ok) return ch;
    }
    return -1;
}

static int cell_glyph(uint32_t x, uint32_t y, int* ink) {
    uint8_t bits[16];
    int any = 0;
    for (int r = 0; r < 16; r++) {
        uint8_t row = 0;
        uint32_t base = (y + (uint32_t)r) * FB_W + x;
        for (int c = 0; c < 8; c++)
            if (FB[base + (uint32_t)c] != C_BG) { row |= (uint8_t)(0x80 >> c); any = 1; }
        bits[r] = row;
    }
    *ink = any;
    if (!any) return ' ';

    // Garis aturan (p_rule/p_hline): 1-3 baris piksel penuh selebar sel.
    // Sel seperti ini bukan glyph — kembalikan '#' supaya tidak dihitung
    // sebagai glyph maupun sebagai kegagalan (kalau tidak, baris label akan
    // "kalah" dari baris garis saat pemilihan alignment).
    int full_rows = 0, partial = 0;
    for (int r = 0; r < 16; r++) {
        if (bits[r] == 0xFF) full_rows++;
        else if (bits[r] != 0) partial = 1;
    }
    if (!partial && full_rows >= 1 && full_rows <= 3) return '#';

    int g = glyph_match(bits);
    return g < 0 ? '?' : g;
}

typedef struct { int matched; int bad; int ink; } row_stat_t;

static void row_decode(uint32_t xoff, uint32_t y, char* out, uint32_t cap, row_stat_t* st) {
    uint32_t n = FB_W / 8u;
    if (n + 1u > cap) n = cap - 1u;
    if ((n - 1u) * 8u + xoff + 8u > FB_W) n--;
    st->matched = st->bad = st->ink = 0;
    for (uint32_t cx = 0; cx < n; cx++) {
        int ink = 0;
        int ch = cell_glyph(xoff + cx * 8u, y, &ink);
        out[cx] = (char)ch;
        if (ink) st->ink++;
        if (ch == '#') continue;          // garis aturan: netral
        if (ch == '?') st->bad++;
        else if (ch != ' ') st->matched++;
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
}

// Teks BSOD digambar mulai PANIC_X=50 px: 50 % 8 = 2, jadi sel font TIDAK
// jatuh pada kelipatan 8. Cari offset x (0..7) yang paling banyak cocok.
// Catatan: sampel TIDAK boleh dibatasi pada baris yang "ber-tinta" saja —
// baris atas glyph umumnya kosong, jadi offset yang benar justru muncul pada
// baris yang belum ber-tinta (jendela 16 px menggeser glyph ke bawah).
static int best_xoff(void) {
    int best = 0;
    int best_score = -1000000;
    char tmp[FB_W / 8u + 1];
    for (int xo = 0; xo < 8; xo++) {
        int score = 0;
        for (uint32_t y = 0; y + 16u <= FB_H; y += 2u) {
            row_stat_t st;
            row_decode((uint32_t)xo, y, tmp, sizeof(tmp), &st);
            score += st.matched * 2 - st.bad;
        }
        if (score > best_score) { best_score = score; best = xo; }
    }
    return best;
}

// Kumpulkan baris teks pada layar: cari offset y yang paling cocok dengan
// glyph (teks digambar per 18px, jadi offset tetangga hampir tidak cocok).
static void fb_text(char* out, size_t cap, int verbose) {
    static int matched[FB_H];
    char tmp[FB_W / 8u + 1];

    if (out) out[0] = '\0';
    uint32_t xoff = (uint32_t)best_xoff();
    for (uint32_t y = 0; y + 16u <= FB_H; y++) {
        row_stat_t st;
        row_decode(xoff, y, tmp, sizeof(tmp), &st);
        matched[y] = st.matched;
    }

    size_t used = 0;
    char last[FB_W / 8u + 1];
    last[0] = '\0';

    for (uint32_t y = 0; y + 16u <= FB_H; y++) {
        if (matched[y] < 4) continue;
        int is_max = 1;
        for (int d = -8; d <= 8; d++) {
            int yy = (int)y + d;
            if (yy < 0 || yy + 16 > FB_H) continue;
            if (yy != (int)y && matched[yy] > matched[y]) { is_max = 0; break; }
        }
        if (!is_max) continue;

        row_stat_t st;
        row_decode(xoff, y, tmp, sizeof(tmp), &st);
        if (tmp[0] == '\0') continue;
        if (strcmp(tmp, last) == 0) continue;         // baris kembar
        strncpy(last, tmp, sizeof(last) - 1);
        last[sizeof(last) - 1] = '\0';

        size_t len = strlen(tmp);
        if (verbose) printf("|%s|\n", tmp);
        if (out && used + len + 2u < cap) {
            memcpy(out + used, tmp, len);
            used += len;
            out[used++] = '\n';
            out[used] = '\0';
        }
    }
}

static int g_fb_cached = 0;
static char g_fb_text[16384];

static const char* fb_all(void) {
    if (!g_fb_cached) { fb_text(g_fb_text, sizeof(g_fb_text), 0); g_fb_cached = 1; }
    return g_fb_text;
}

static int fb_find(const char* needle) { return strstr(fb_all(), needle) != NULL; }
static int ser_find(const char* needle) { return strstr(SER, needle) != NULL; }

// =====================================================================
// HARNESS
// =====================================================================
static int g_fails = 0;
static void check(int cond, const char* msg) {
    printf("%s %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fails++;
}

static void scenario_begin(int task_id, const char* task_name, uint8_t kind) {
    panic_host_test_reset();
    panic_set_reboot_hook(test_reboot_hook);
    ser_len = 0; SER[0] = '\0';
    g_flushes = 0; g_reboot_hook_calls = 0; g_flushes_at_reboot = -1;
    g_fb_cached = 0;
    g_test_task = task_id;
    g_cpu_index = 1;
    memset(FB, 0, sizeof(FB));
    memset(tasks, 0, sizeof(tasks));
    if (task_id >= 0 && task_id < MAX_TASKS) {
        strncpy(tasks[task_id].name, task_name, sizeof(tasks[task_id].name) - 1);
        tasks[task_id].kind = kind;
        tasks[task_id].parent_id = 1;
    }
}

static registers_t make_regs(uint64_t int_num, uint64_t err, uint64_t rip, uint64_t cs) {
    registers_t r;
    memset(&r, 0, sizeof(r));
    r.int_num = int_num;
    r.error_code = err;
    r.rip = rip;
    r.cs = cs;
    r.rflags = 0x202;
    r.rax = 0x4241500;
    r.rbx = 0x1000;
    r.rsp = (uint64_t)(uintptr_t)g_fake_stack;
    r.rbp = 0;
    return r;
}

// =====================================================================
// SKENARIO
// =====================================================================
static void test_pf_user_unmapped_write(void) {
    scenario_begin(2, "control-center", TASK_KIND_SPAWNED);
    g_panic_test_cr2 = 0x00000000BFBFFCB8ull;               // kanonik, belum termap
    registers_t r = make_regs(14, 0x6, 0x41E6B24, 0x23);    // P=0 W=1 U=1

    exception_handler(&r);

    check(panic_is_locked() == 1, "PF user: lockdown aktif (compositor/mouse berhenti)");
    check(fb_find("PENYEBAB") && fb_find("APLIKASI user mengakses alamat BELUM TERMAP"),
          "PF user: PENYEBAB menyebut alamat user belum termap");
    check(fb_find("RIP di LUAR kernel text saat CPL=0"),
          "PF user: PENYEBAB menyebut RIP di luar kernel text");
    check(fb_find("DI MANA") && fb_find("control-center"),
          "PF user: DI MANA memuat nama task & CPU");
    check(fb_find("CR2") && fb_find("OP: WRITE") && fb_find("REGISTERS"),
          "PF user: DETAIL memuat CR2, arah akses, register");
    check(g_panic_test_reboots == 1 && g_reboot_hook_calls == 1,
          "PF user: auto-reboot terjadi sekali");
    check(g_flushes >= 1 && g_flushes_at_reboot >= 1,
          "PF user: FS diflush SEBELUM reboot");
    check(g_panic_test_redraws >= 8 && g_panic_test_redraws <= 12,
          "PF user: countdown menimpa sel digit saja (anti-flicker)");
    check(ser_find("KERNEL PANIC (serial dump)") && ser_find("CR2:"),
          "PF user: mirror serial memuat dump lengkap");
}

static void test_pf_kernel_write_ro(void) {
    scenario_begin(3, "kcopy", TASK_KIND_KERNEL);
    g_panic_test_cr2 = (uint64_t)(uintptr_t)g_fake_stack;   // bit P=1 (ter-map)
    registers_t r = make_regs(14, 0x3, 0xFFFFFFFF8000BB4Bull, 0x08);  // P=1 W=1 U=0

    exception_handler(&r);

    check(fb_find("KERNEL MENULIS ke halaman READ-ONLY"),
          "PF kernel: verdict penulisan ke halaman read-only");
    check(fb_find("KERNEL TEXT"), "PF kernel: RIP diklasifikasikan KERNEL TEXT");
    check(!fb_find("catatan: paging_is_mapped()"),
          "PF kernel: tanpa catatan TLB/CR3 saat bit P cocok");
    check(g_panic_test_reboots == 1, "PF kernel: auto-reboot terjadi");
}

static void test_pf_kernel_wild_pointer(void) {
    scenario_begin(-1, "", TASK_KIND_KERNEL);      // idle/ISR: tanpa task
    g_panic_test_cr2 = 0x00000DEADBEEF000ull;
    registers_t r = make_regs(14, 0x2, 0x0000000000001000ull, 0x08);

    exception_handler(&r);

    check(fb_find("KERNEL mengakses alamat yang BELUM TERMAP"),
          "PF kernel liar: verdict pointer NULL/liar");
    check(fb_find("(tidak ada task aktif - konteks idle/ISR)"),
          "PF kernel liar: konteks idle/ISR ditandai jelas");
    check(fb_find("DI LUAR KERNEL TEXT - lompat ke pointer tak sah"),
          "PF kernel liar: kelas RIP menunjukkan lompatan tak sah");
    check(ser_find("TASK: idle/ISR"), "PF kernel liar: serial juga menandai idle");
}

static void test_pf_non_canonical(void) {
    scenario_begin(2, "fileman", TASK_KIND_SPAWNED);
    g_panic_test_cr2 = 0x0000DEADBEEF0000ull;               // non-kanonik
    registers_t r = make_regs(14, 0x0, 0x0000000000401000ull, 0x23);

    exception_handler(&r);

    check(fb_find("ALAMAT NON-KANONIKAL"), "PF non-kanonik: verdict alamat mustahil");
    check(fb_find("NON-KANONIKAL (alamat mustahil)"),
          "PF non-kanonik: baris TARGET menandai CR2 non-kanonik");
    check(g_panic_test_reboots == 1, "PF non-kanonik: auto-reboot terjadi");
}

static void test_repeat_panic(void) {
    scenario_begin(2, "desktop", TASK_KIND_SPAWNED);
    g_panic_test_cr2 = 0x00000000BFBFFCB8ull;
    registers_t r = make_regs(14, 0x6, 0x41E6B24, 0x23);

    exception_handler(&r);                     // panic pertama
    g_fb_cached = 0;
    check(!fb_find("PANIC BERULANG"), "panic pertama: banner tanpa penanda berulang");

    exception_handler(&r);                     // panic kedua (lockdown masih aktif)
    g_fb_cached = 0;
    check(fb_find("PANIC BERULANG"), "panic kedua: banner menandai PANIC BERULANG");
    check(g_panic_test_reboots == 2, "panic kedua: reboot tetap dipicu");
}

static void test_kernel_panic_manual(void) {
    scenario_begin(1, "shell", TASK_KIND_SPAWNED);
    kernel_panic("HEAP CORRUPTION", "double free terdeteksi di kmalloc", 0xDEADull);

    check(fb_find("HEAP CORRUPTION") && fb_find("double free terdeteksi di kmalloc"),
          "kernel_panic: judul & detail jadi PENYEBAB");
    check(fb_find("CODE") && fb_find("pages terpakai"),
          "kernel_panic: DETAIL memuat CODE & pemakaian memori");
    check(ser_find("REASON: HEAP CORRUPTION") && ser_find("CODE:"),
          "kernel_panic: mirror serial memuat REASON/CODE");
    check(g_panic_test_reboots == 1 && g_flushes_at_reboot >= 1,
          "kernel_panic: flush FS lalu reboot");
}

// =====================================================================
// MAIN
// =====================================================================
int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        scenario_begin(2, "control-center", TASK_KIND_SPAWNED);
        g_panic_test_cr2 = 0x00000000BFBFFCB8ull;
        registers_t r = make_regs(14, 0x6, 0x41E6B24, 0x23);
        exception_handler(&r);
        printf("\n");
        fb_text(g_fb_text, sizeof(g_fb_text), 1);
        printf("\n");
        return 0;
    }

    printf("=== test panic handler (host) ===\n");
    test_pf_user_unmapped_write();
    test_pf_kernel_write_ro();
    test_pf_kernel_wild_pointer();
    test_pf_non_canonical();
    test_repeat_panic();
    test_kernel_panic_manual();

    printf("\n%d skenario gagal\n", g_fails);
    return g_fails ? 1 : 0;
}
