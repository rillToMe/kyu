#include <stdint.h>
#include "task.h"
#include "serial.h"
#include "display.h"

// registers_t is provided by task.h — must match PUSHA64 in isr_macro.inc

// =======================================================================
// SERIAL MIRROR PANIC — dump ke COM1 (-serial stdio) agar teks panic
// selamat walau framebuffer BSOD langsung ketimpa kompositor (desktop).
// =======================================================================
static void ser_hex(uint64_t v) {
    const char* d = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    serial_print(buf);
}
static void ser_dec(uint64_t v) {
    char buf[22]; int i = 20; buf[21] = '\0';
    if (v == 0) { serial_print("0"); return; }
    while (v > 0 && i >= 0) { buf[i--] = '0' + (v % 10); v /= 10; }
    serial_print(&buf[i + 1]);
}

extern uint32_t* fb_ptr;
extern const unsigned char font8x16[256][16];

// Impor fungsi cek rute fisik (dari paging.c)
extern int paging_is_mapped(uint64_t vaddr);

// SMP diagnostics (read-only, safe in panic context)
extern uint32_t smp_current_cpu_index(void);
extern uint32_t smp_online_cpu_count(void);

// PMM diagnostics (O(1) volatile reads, no lock needed in panic)
extern uint64_t pmm_get_used_pages(void);
extern uint64_t pmm_get_total_pages(void);

// Task diagnostics
extern int task_count;

// =======================================================================
// MESIN GAMBAR DARURAT (TANPA MALLOC)
// =======================================================================

static uint32_t panic_cursor_x = 50;
static uint32_t panic_cursor_y = 30;

static void panic_reset_cursor(void) {
    panic_cursor_x = 50;
    panic_cursor_y = 30;
}

void panic_draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    const display_mode_t* m = display_get_mode();
    if (!fb_ptr || !m || x >= m->width || y >= m->height) return;
    fb_ptr[(y * (m->pitch_bytes / 4)) + x] = color;
}

void panic_draw_char(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    if ((unsigned char)c > 127) c = '?';
    const unsigned char* bmp = font8x16[(unsigned char)c];
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            panic_draw_pixel(x + col, y + row,
                (bmp[row] & (0x80 >> col)) ? fg : bg);
        }
    }
}

void panic_draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    for (int i = 0; str[i]; i++) {
        panic_draw_char(str[i], x + (i * 8), y, fg, bg);
    }
}

// Draw at current cursor, auto-advance
static void p_str(const char* str, uint32_t fg, uint32_t bg) {
    panic_draw_string(str, panic_cursor_x, panic_cursor_y, fg, bg);
    panic_cursor_x += 8 * (uint32_t)__builtin_strlen(str);
}

static void p_newline(uint32_t bg) {
    (void)bg;
    panic_cursor_x = 50;
    panic_cursor_y += 18;
}

// FORMAT HEX 64-BIT
void panic_draw_hex(uint64_t num, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    const char* digits = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) {
        buf[i] = digits[num & 0xF];
        num >>= 4;
    }
    panic_draw_string(buf, x, y, fg, bg);
}

static void p_hex(const char* label, uint64_t val, uint32_t fg, uint32_t bg) {
    p_str(label, fg, bg);
    panic_draw_hex(val, panic_cursor_x, panic_cursor_y, fg, bg);
    panic_cursor_x += 18 * 8;
}

void panic_draw_dec(uint64_t num, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    char buf[22];
    int i = 20;
    buf[21] = '\0';
    if (num == 0) { panic_draw_char('0', x, y, fg, bg); return; }
    while (num > 0 && i >= 0) {
        buf[i--] = '0' + (num % 10);
        num /= 10;
    }
    panic_draw_string(&buf[i + 1], x, y, fg, bg);
}

static void p_dec(const char* label, uint64_t val, uint32_t fg, uint32_t bg) {
    p_str(label, fg, bg);
    char buf[22]; int i = 20; buf[21] = '\0';
    if (val == 0) { buf[i] = '0'; i--; }
    else { while (val > 0 && i >= 0) { buf[i--] = '0' + (val % 10); val /= 10; } }
    panic_draw_string(&buf[i + 1], panic_cursor_x, panic_cursor_y, fg, bg);
    panic_cursor_x += 8 * (uint32_t)__builtin_strlen(&buf[i + 1]);
}

static void fill_screen(uint32_t color) {
    const display_mode_t* m = display_get_mode();
    if (!m) return;
    for (uint32_t y = 0; y < m->height; y++)
        for (uint32_t x = 0; x < m->width; x++)
            panic_draw_pixel(x, y, color);
}

// =======================================================================
// NAMA-NAMA EXCEPTION x86
// =======================================================================
static const char* exception_names[] = {
    "Divide by Zero",         // 0x00
    "Debug",                  // 0x01
    "NMI",                    // 0x02
    "Breakpoint",             // 0x03
    "Overflow",               // 0x04
    "Bound Range Exceeded",   // 0x05
    "INVALID OPCODE",         // 0x06
    "Device Not Available",   // 0x07
    "DOUBLE FAULT",           // 0x08
    "Coprocessor Overrun",    // 0x09
    "Invalid TSS",            // 0x0A
    "Segment Not Present",    // 0x0B
    "STACK-SEGMENT FAULT",    // 0x0C
    "GENERAL PROTECTION",     // 0x0D
    "PAGE FAULT",             // 0x0E
};

// =======================================================================
// BSOD — Full diagnostic dump
// =======================================================================
void exception_handler(registers_t *r) {
    __asm__ volatile("cli");
#ifdef HEAP_WATCH_DEBUG
    // isr1_stub (vector 1, #DB) already routes here; divert hardware-watchpoint
    // hits to the serial dumper instead of the framebuffer BSOD. Handler hanya
    // MENLOG lalu return — eksekusi dilanjutkan via iretq di stub.
    if (r->int_num == 1) {
        extern void heap_watch_db_handler(registers_t *r);
        heap_watch_db_handler(r);
        return;
    }
#endif
    if (!fb_ptr) { while(1) { __asm__ volatile("hlt"); } }

    uint64_t int_num    = r->int_num;
    uint64_t error_code = r->error_code;

    // Read CR2 (page fault address)
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    // --- Serial mirror (COM1) ---
    serial_print("\n==== KERNEL PANIC (serial dump) ====\nEXCEPTION: ");
    serial_print(int_num < 15 ? exception_names[int_num] : "Unknown");
    serial_print("\nINT: "); ser_dec(int_num);
    serial_print("  ERR: "); ser_hex(error_code);
    if (int_num == 14) {
        serial_print("\nCR2: "); ser_hex(cr2);
        serial_print("  PF: ");
        serial_print(paging_is_mapped(cr2) ? "PROT" : "NONP");
        serial_print(error_code & 2 ? " WRITE" : " READ");
        serial_print(error_code & 4 ? " USER" : " KERNEL");
    }
    serial_print("\nRIP: "); ser_hex(r->rip);
    serial_print("  RSP: "); ser_hex(r->rsp);
    serial_print("  CS: "); ser_hex(r->cs);
    serial_print("  SS: "); ser_hex(r->ss);
    serial_print("\nRAX: "); ser_hex(r->rax);
    serial_print("  RBX: "); ser_hex(r->rbx);
    serial_print("  RCX: "); ser_hex(r->rcx);
    serial_print("  RDX: "); ser_hex(r->rdx);
    serial_print("\nRSI: "); ser_hex(r->rsi);
    serial_print("  RDI: "); ser_hex(r->rdi);
    serial_print("  RBP: "); ser_hex(r->rbp);
    serial_print("  R8:  "); ser_hex(r->r8);
    serial_print("\n");

    // SMP context (safe: reads volatile, no lock)
    uint32_t panic_cpu  = smp_current_cpu_index();
    uint32_t online_cpu = smp_online_cpu_count();

    // PMM stats (safe: reads volatile counters)
    uint64_t pmm_used  = pmm_get_used_pages();
    uint64_t pmm_total = pmm_get_total_pages();

    // Colors
    uint32_t BG   = 0x001144;
    uint32_t FG   = 0xFFFFFF;
    uint32_t HDR  = 0xFF4444;
    uint32_t WARN = 0xFFCC00;
    uint32_t DIM  = 0x88AAFF;
    uint32_t OK   = 0x44FF44;

    fill_screen(BG);
    panic_reset_cursor();

    // ── Header ──
    p_str("====================================================", HDR, BG);
    p_newline(BG);
    p_str("   KYUZEN OS — KERNEL PANIC", HDR, BG);
    p_newline(BG);
    p_str("====================================================", HDR, BG);
    p_newline(BG);
    p_newline(BG);

    // ── Exception info ──
    p_str("EXCEPTION:  ", DIM, BG);
    if (int_num < 15)
        p_str(exception_names[int_num], WARN, BG);
    else
        p_str("Unknown", WARN, BG);
    p_newline(BG);

    p_hex("INT:        ", int_num, FG, BG);
    p_hex("  ERR:  ", error_code, FG, BG);
    p_newline(BG);
    p_newline(BG);

    // ── Crash location ──
    p_hex("CRASH RIP:  ", r->rip, WARN, BG);
    p_newline(BG);
    p_hex("CRASH RSP:  ", r->rsp, FG, BG);
    p_newline(BG);
    p_newline(BG);

    // ── Page Fault analysis ──
    if (int_num == 14) {
        p_str("--- PAGE FAULT ANALYSIS ---", WARN, BG);
        p_newline(BG);
        p_hex("CR2 ADDR:   ", cr2, WARN, BG);
        p_newline(BG);

        p_str("STATUS:     ", DIM, BG);
        if (paging_is_mapped(cr2))
            p_str("PROTECTION VIOLATION (page present, access denied)", HDR, BG);
        else
            p_str("NOT PRESENT (address unmapped)", HDR, BG);
        p_newline(BG);

        p_str("ACCESS:     ", DIM, BG);
        if (error_code & 1) p_str("PROT ", WARN, BG);
        else                p_str("NONP ", HDR, BG);
        if (error_code & 2) p_str("WRITE ", WARN, BG);
        else                p_str("READ ", FG, BG);
        if (error_code & 4) p_str("USER", WARN, BG);
        else                p_str("KERNEL", FG, BG);
        p_newline(BG);
        p_newline(BG);
    }

    // ── GPF analysis ──
    if (int_num == 13) {
        p_str("--- GPF ANALYSIS ---", WARN, BG);
        p_newline(BG);
        if (error_code == 0) {
            p_str("Null selector or non-canonical address", HDR, BG);
        } else {
            int tbl = (error_code >> 1) & 3;
            int idx = (error_code >> 3) & 0x1FFF;
            p_str("Selector index: ", DIM, BG);
            p_dec("", (uint64_t)idx, WARN, BG);
            p_str(tbl == 0 ? " (GDT)" : (tbl == 2 ? " (LDT)" : " (IDT)"), DIM, BG);
        }
        p_newline(BG);
        p_newline(BG);
    }

    // ── Register dump ──
    p_str("--- REGISTERS ---", DIM, BG);
    p_newline(BG);
    p_hex("RAX: ", r->rax, FG, BG);
    p_hex("  RBX: ", r->rbx, FG, BG);
    p_newline(BG);
    p_hex("RCX: ", r->rcx, FG, BG);
    p_hex("  RDX: ", r->rdx, FG, BG);
    p_newline(BG);
    p_hex("RSI: ", r->rsi, FG, BG);
    p_hex("  RDI: ", r->rdi, FG, BG);
    p_newline(BG);
    p_hex("RBP: ", r->rbp, FG, BG);
    p_hex("  R8:  ", r->r8, FG, BG);
    p_newline(BG);
    p_newline(BG);

    // ── SMP + PMM Diagnostics ──
    p_str("--- SYSTEM STATE ---", DIM, BG);
    p_newline(BG);

    p_str("CPU:        ", DIM, BG);
    p_dec("", (uint64_t)panic_cpu, WARN, BG);
    p_str(" of ", DIM, BG);
    p_dec("", (uint64_t)online_cpu, OK, BG);
    p_str(" online", DIM, BG);
    p_newline(BG);

    p_str("TASK:       ", DIM, BG);
    int panic_task = smp_current_task_id();
    if (panic_task >= 0)
        p_dec("#", (uint64_t)panic_task, WARN, BG);
    else
        p_str("idle", WARN, BG);
    p_str(" of ", DIM, BG);
    p_dec("", (uint64_t)task_count, FG, BG);
    p_str(" total", DIM, BG);
    p_newline(BG);

    p_str("PMM PAGES:  ", DIM, BG);
    p_dec("", pmm_used, WARN, BG);
    p_str(" / ", DIM, BG);
    p_dec("", pmm_total, FG, BG);
    p_str(" used", DIM, BG);
    p_newline(BG);

    p_str("PMM RAM:    ", DIM, BG);
    p_dec("", (pmm_used * 4096) / 1024, WARN, BG);
    p_str(" KB / ", DIM, BG);
    p_dec("", (pmm_total * 4096) / 1024, FG, BG);
    p_str(" KB", DIM, BG);
    p_newline(BG);

    p_newline(BG);
    p_str("====================================================", HDR, BG);
    p_newline(BG);
    p_str("System frozen. Restart to recover.", FG, BG);
    p_newline(BG);

    while (1) { __asm__ volatile("hlt"); }
}

// =======================================================================
// GENERIC KERNEL PANIC (called manually from kernel code)
// =======================================================================
__attribute__((weak))
void kernel_panic(const char* title, const char* desc, uint64_t code) {
    __asm__ volatile("cli");
    if (!fb_ptr) { while(1) { __asm__ volatile("hlt"); } }

    // --- Serial mirror (COM1) ---
    serial_print("\n==== KERNEL PANIC (serial dump) ====\nREASON: ");
    serial_print(title);
    serial_print("\nDETAILS: "); serial_print(desc);
    serial_print("\nCODE: "); ser_hex(code);
    serial_print("\n");

    uint32_t BG   = 0x001144;
    uint32_t FG   = 0xFFFFFF;
    uint32_t HDR  = 0xFF4444;
    uint32_t WARN = 0xFFCC00;
    uint32_t DIM  = 0x88AAFF;

    fill_screen(BG);
    panic_reset_cursor();

    p_str("====================================================", HDR, BG);
    p_newline(BG);
    p_str("   KYUZEN OS - KERNEL PANIC", HDR, BG);
    p_newline(BG);
    p_str("====================================================", HDR, BG);
    p_newline(BG);
    p_newline(BG);

    p_str("REASON:     ", DIM, BG);
    p_str(title, WARN, BG);
    p_newline(BG);

    p_str("DETAILS:    ", DIM, BG);
    p_str(desc, FG, BG);
    p_newline(BG);

    p_hex("CODE:       ", code, WARN, BG);
    p_newline(BG);
    p_newline(BG);

    // SMP context
    p_str("CPU:        ", DIM, BG);
    p_dec("", (uint64_t)smp_current_cpu_index(), WARN, BG);
    p_str(" of ", DIM, BG);
    p_dec("", (uint64_t)smp_online_cpu_count(), FG, BG);
    p_str(" online", DIM, BG);
    p_newline(BG);

    // PMM state
    p_str("PMM PAGES:  ", DIM, BG);
    p_dec("", pmm_get_used_pages(), WARN, BG);
    p_str(" / ", DIM, BG);
    p_dec("", pmm_get_total_pages(), FG, BG);
    p_str(" used", DIM, BG);
    p_newline(BG);

    p_newline(BG);
    p_str("System frozen. Restart to recover.", FG, BG);
    p_newline(BG);

    while (1) { __asm__ volatile("hlt"); }
}

// =======================================================================
// SHORTCUT HANDLER (legacy)
// =======================================================================
void page_fault_handler(registers_t *r, uint64_t fault_addr) {
    (void)fault_addr;
    exception_handler(r);
}
