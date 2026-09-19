#ifndef INTEL_MMIO_H
#define INTEL_MMIO_H

#include <stdint.h>

// Initialize MMIO base pointer (called once during intel_init)
void intel_mmio_init(volatile uint8_t* mmio_base);

// Get MMIO base pointer
volatile uint8_t* intel_mmio_base(void);

// Register access
uint32_t intel_mmio_read32(uint32_t offset);
void     intel_mmio_write32(uint32_t offset, uint32_t val);
uint16_t intel_mmio_read16(uint32_t offset);
uint8_t  intel_mmio_read8(uint32_t offset);

// Verify MMIO is accessible (returns 0 on success)
int intel_mmio_verify(void);

#endif // INTEL_MMIO_H
