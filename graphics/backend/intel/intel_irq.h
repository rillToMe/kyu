#ifndef INTEL_IRQ_H
#define INTEL_IRQ_H

// ============================================================
// Intel GPU completion IRQ — Phase 14
//
// Split design (see intel_irq.c):
// - intel_irq_handler(): lock-free ISR body. Snapshots the
//   fence status seqno + bumps the wakeup counter. No locks,
//   no allocator, no MMIO — safe from real ISR context.
//   Until an IDT vector is wired (needs real HW to validate),
//   the sleep-wait loop calls it after every wakeup, so the
//   observation path is live and counted.
// - IMR unmask / IDT vector / EOI: deferred. Unmasking GPU
//   INTx with no installed vector would #GP on real HW;
//   QEMU has no iGPU to validate against. (Agent Rule 20.)
// ============================================================

#include <stdint.h>

// ISR body (also called from sleep-wait after each wakeup).
void intel_irq_handler(void);

// Last status seqno observed by the handler.
uint32_t intel_irq_last_seqno(void);

// Handler invocation count (wakeups observed).
uint32_t intel_irq_count(void);

// One-line serial log: wakeup count (diagnostics).
void intel_irq_log(void);

#endif // INTEL_IRQ_H
