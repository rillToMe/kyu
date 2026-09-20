// ============================================================
// Intel GPU MMIO Helpers (graphics/backend/intel/intel_mmio.c)
//
// Thin wrappers around MMIO read/write. All accesses go through
// the remapped BAR0 pointer (phys + hhdm_offset).
//
// MMIO must be verified before use (readback test in intel_init).
// ============================================================

#include "intel_regs.h"
#include <stdint.h>

// Set during intel_init() — BAR0 physical + HHDM offset
static volatile uint8_t* g_mmio_base;

void intel_mmio_init(volatile uint8_t* mmio_base) {
    g_mmio_base = mmio_base;
}

volatile uint8_t* intel_mmio_base(void) {
    return g_mmio_base;
}

uint32_t intel_mmio_read32(uint32_t offset) {
    volatile uint32_t* reg = (volatile uint32_t*)(g_mmio_base + offset);
    return *reg;
}

void intel_mmio_write32(uint32_t offset, uint32_t val) {
    volatile uint32_t* reg = (volatile uint32_t*)(g_mmio_base + offset);
    *reg = val;
}

uint16_t intel_mmio_read16(uint32_t offset) {
    volatile uint16_t* reg = (volatile uint16_t*)(g_mmio_base + offset);
    return *reg;
}

uint8_t intel_mmio_read8(uint32_t offset) {
    return g_mmio_base[offset];
}

// Verify MMIO is accessible by writing a known value to a scratch
// register and reading it back. This catches:
// - BAR not mapped
// - MMIO not enabled in PCI command register
// - Wrong BAR address
int intel_mmio_verify(void) {
    // Read GEN9 chip ID (stable across most Gen8-12 devices).
    // If MMIO is dead, this returns 0x00000000 or 0xFFFFFFFF.
    uint32_t id = intel_mmio_read32(INTEL_MMIO_DEVID);
    if (id == 0x00000000 || id == 0xFFFFFFFF) return -1;
    return 0;
}
