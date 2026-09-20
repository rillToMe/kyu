// ============================================================
// Intel GPU completion IRQ — Phase 14
// (graphics/backend/intel/intel_irq.c)
//
// Handler touches only lock-free state (volatile snapshot +
// counter), so a future IDT vector can call it directly.
// No MMIO here: IIR ack needs a readback-verified offset
// (Gen8+ GT IRQ layout TBD on HW); the fence status page is
// the source of truth and has no side effects to clear.
// ============================================================

#include "intel_irq.h"
#include "intel_cmd.h" // status read (HHDM, lock-free)

extern void serial_print(const char* s);

static volatile uint32_t g_last_seqno = 0;
static volatile uint32_t g_count = 0;

void intel_irq_handler(void) {
    g_last_seqno = intel_cmd_status_read();
    g_count++;
}

uint32_t intel_irq_last_seqno(void) { return g_last_seqno; }
uint32_t intel_irq_count(void) { return g_count; }

void intel_irq_log(void) {
    char b[12]; int n = 0;
    uint32_t v = g_count;
    if (v == 0) { serial_print("[intel_irq] wakeups=0\n"); return; }
    char t[12]; int i = 0;
    while (v > 0 && i < 11) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) b[n++] = t[--i];
    b[n] = 0;
    serial_print("[intel_irq] wakeups=");
    serial_print(b);
    serial_print("\n");
}
