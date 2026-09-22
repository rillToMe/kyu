#ifndef PANIC_INTERNAL_H
#define PANIC_INTERNAL_H

// =====================================================================
// API internal modul panic (kernel/panic/)
//
// panic.c dipecah empat supaya tiap bagian bisa dibaca sendiri:
//   panic.c         orchestrator — lockdown, entry point (exception_handler,
//                   kernel_panic), persistensi crash, loop interaktif.
//   panic_draw.c    mesin gambar darurat — pemilihan target (scanout device /
//                   framebuffer), primitive gambar, tata letak banner & baris
//                   status.
//   panic_hw.c      akses hardware darurat TANPA IRQ — PIT2 & PS/2 polling,
//                   reset/reboot, power-off, freeze.
//   panic_explain.c penerjemah fault — verdict, klasifikasi RIP, backtrace.
//
// Hanya yang dipakai lintas file dideklarasikan di sini; sisanya `static` di
// file masing-masing. Header ini BUKAN API publik — konsumen luar (compositor,
// mouse, dll.) tetap memakai include/panic.h.
//
// ATURAN YANG BERLAKU UNTUK SEMUA FILE DI DIREKTORI INI
//   * tanpa heap, tanpa lock, tanpa IRQ — jalur ini berjalan dengan `cli` di
//     CPU yang bisa jadi sedang memegang lock yang kita butuhkan (self-deadlock
//     = BSOD tidak pernah muncul, tidak ada reboot);
//   * hanya alamat higher-half yang boleh dibaca (lihat catatan panjang di
//     panic_explain.c) — membaca alamat user di sini = #PF kedua;
//   * tidak ada libc: tanpa malloc/memcpy/printf, semua buffer statis.
// =====================================================================

#include <stdint.h>
#include <stddef.h>  // NULL (tidak lagi didapat dari header lain yang di-drop)
#include "panic.h"   // API publik + seam host test (g_panic_test_*, panic_log_*)
#include "task.h"    // registers_t, tasks[], MAX_TASKS, TASK_KIND_*

// =====================================================================
// Konstanta tata letak & warna (dipakai panic.c + panic_draw.c)
// =====================================================================
#define PANIC_X          50u    // margin kiri semua blok teks
#define PANIC_TOP_Y      30u    // baris pertama banner
#define PANIC_ROW_H      18u    // jarak antar baris (font 8x16 + 2)
#define PANIC_CHAR_W     8u
#define PANIC_LABEL_W    11u    // lebar kolom label ("PENYEBAB   ")
#define PANIC_INDENT     (PANIC_X + PANIC_LABEL_W * PANIC_CHAR_W)  // 138
#define PANIC_BOTTOM_GAP 64u    // area bawah yang disisakan untuk baris info/tombol

#define C_BG     0x001144u      // latar biru tua
#define C_FG     0xFFFFFFu      // teks utama
#define C_TITLE  0xFF4444u      // judul / verdict
#define C_KEY    0xFFCC00u      // angka & nilai penting
#define C_DIM    0x88AAFFu      // label
#define C_FAINT  0x6E82A8u      // bagian DETAIL (sengaja redup)
#define C_RULE   0x3A4E78u      // garis tipis 1px
#define C_OK     0x44FF44u      // penanda "normal / di dalam kernel text"

// Di bawah tinggi ini layar dianggap terlalu pendek: baris info & petunjuk
// tombol dilewati (lebih baik BSOD tanpa catatan kaki daripada bertumpuk).
#define PANIC_UI_MIN_HEIGHT    140u

// Host test: loop tombol di kernel bersifat TERMINAL (tidak pernah kembali),
// jadi seam uji butuh jalan keluar setara supaya bisa diperiksa bahwa tanpa
// input memang TIDAK ada reboot. Tidak dipakai di build kernel.
#define PANIC_HOST_IDLE_LOOPS  3u

// Alamat dasar kernel text (lihat linker.ld) — dipakai untuk mengklasifikasi
// RIP dan menandai alamat backtrace. Aturan higher-half ada di panic_explain.c.
#define KERNEL_VMA             0xFFFFFFFF80000000ull
#define PANIC_HIGHER_HALF      0xFFFF800000000000ull

// =====================================================================
// Dependensi eksternal (dideklarasikan di sini, bukan lewat header lain:
// beberapa header kernel membawa lock/alokasi yang tidak boleh dipakai di
// jalur panic). Semua pemanggil WAJIB memakai varian _nolock/_try — versi
// ber-lock bisa self-deadlock kalau CPU yang fault memegang lock itu.
// =====================================================================
extern uint32_t* fb_ptr;                        // framebuffer Limine (fallback)
extern unsigned char font8x16[][16];            // definisi: kernel/gfx/fb.c

extern int      paging_is_mapped_nolock(uint64_t vaddr);
extern uint32_t smp_current_cpu_index(void);
extern uint32_t smp_online_cpu_count(void);
extern uint64_t pmm_get_used_pages_nolock(void);
extern uint64_t pmm_get_total_pages_nolock(void);
extern uint64_t timer_get_ms(void);
extern int      kfs_sync_all_try(void);

// Konteks syscall terakhir (kernel/syscall/syscall.c) — best-effort, tanpa lock.
extern volatile uint64_t g_last_syscall_num;
extern volatile int32_t  g_last_syscall_task;

// =====================================================================
// panic_draw.c — mesin gambar darurat
// =====================================================================
// Target gambar dipilih SEKALI per panic: memori scanout device (virtio) bila
// ada, kalau tidak framebuffer hardware. 0 = ada target, <0 = tidak ada
// (laporan ke serial saja).
int  panic_target_begin(void);
int  panic_target_ready(void);   // 1 = ada target gambar (setelah target_begin)
uint32_t panic_screen_w(void);
uint32_t panic_screen_h(void);

// Kirim perubahan yang belum terkirim ke device (no-op di jalur framebuffer).
void p_flush(void);

void fill_screen(uint32_t color);
void panic_draw_pixel(uint32_t x, uint32_t y, uint32_t color);
void panic_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
void panic_draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
void panic_draw_hex(uint64_t num, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
void panic_draw_dec(uint64_t num, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);

// --- tata letak berbasis kursor (dipakai panic.c merangkai layar BSOD) ---
void p_home(void);
void p_newline(void);
void p_label(const char* label);          // label redup, nilai di kolom PANIC_INDENT
void p_cont_begin(void);                  // baris lanjutan (tanpa label)
void p_str(const char* s, uint32_t fg);
void p_hex(uint64_t v, uint32_t fg);
void p_dec(uint64_t v, uint32_t fg);
void p_line(uint32_t y, const char* s, uint32_t fg);
void p_rule(const char* label);           // label + garis tipis sampai margin kanan
void p_banner(int repeat);                // pita judul "KERNEL PANIC"
void panic_overlay_banner(const char* msg);  // pita peringatan panic bersarang
void panic_status_line(const char* msg);     // umpan balik sebelum reboot/shutdown

// Mirror serial angka (versi lokal: tanpa lock, buffer di stack).
void ser_dec(uint64_t v);
void ser_hex(uint64_t v);

#ifdef PANIC_HOST_TEST
// Seam host test: target gambar di-reset antar skenario (di kernel satu panic
// = satu target, dari awal sampai reboot/power-off).
void panic_target_reset(void);
#endif

// =====================================================================
// panic_hw.c — hardware darurat tanpa IRQ, aksi operator
// =====================================================================
void panic_cli(void);
uint64_t panic_read_cr2(void);
uint64_t panic_read_cr3(void);
uint64_t panic_clock_ms(void);            // waktu tanpa efek samping (uptime)

void panic_monotonic_reset(void);         // program PIT ch2 (sekali, saat masuk panic)
void panic_idle_slice(void);              // ~5-10 ms sambil mem-polling PIT2
void panic_input_irq_mask(void);          // kernel: matikan IRQ1/IRQ12 (host: no-op)
void panic_keys_drain(void);              // buang sisa ketikan sebelum panic

void panic_freeze(void);                  // kernel: halt selamanya
void panic_flush_and_reboot(void);        // sync best-effort lalu reset hardware

#ifdef PANIC_HOST_TEST
// Seam host test: jam palsu, tombol, counter aksi, dan reboot hook — semua
// state yang dimiliki panic_hw.c.
void panic_hw_reset(void);
#endif

// =====================================================================
// panic_explain.c — penerjemah fault
// =====================================================================
extern const char* const exception_names[15];

typedef struct {
    const char* verdict;   // baris PENYEBAB
    const char* hint1;     // petunjuk 1 (boleh NULL)
    const char* hint2;     // petunjuk 2 (boleh NULL)
} panic_cause_t;

panic_cause_t panic_explain(uint64_t intno, uint64_t err, uint64_t cr2, uint64_t rip);
int panic_is_canonical(uint64_t a);
int panic_in_kernel(uint64_t rip);
const char* panic_rip_class(uint64_t rip, uint64_t cs);
uint32_t panic_rip_class_color(uint64_t rip, uint64_t cs);

void p_backtrace(uint64_t rbp, uint64_t rsp, uint64_t cs);
void panic_backtrace_unavailable(void);   // bagian BACKTRACE saat tidak ada frame
// Kumpulkan alamat backtrace untuk crashdump (versi teks, tanpa menggambar).
uint32_t panic_bt_collect(uint64_t rbp, uint64_t rsp, uint64_t cs,
                          uint64_t* out, uint32_t max);

#endif // PANIC_INTERNAL_H
