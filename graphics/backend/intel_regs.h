#ifndef INTEL_REGS_H
#define INTEL_REGS_H

#include <stdint.h>

// ============================================================
// Intel Integrated GPU — MMIO Register Definitions
// (Gen8 / Broadwell through Gen12 / Alder Lake)
//
// Reference: Intel Open Source i915 driver (drivers/gpu/drm/i915/)
// These are the registers needed for 2D blitter (BCS) bring-up.
//
// MMIO base = BAR0 (physical) + hhdm_offset.
// All offsets are byte offsets from MMIO base.
// ============================================================

// --- PCI Config Space (standard, not MMIO) ---
#define INTEL_PCI_VENDOR_ID       0x8086
#define INTEL_PCI_CLASS_VGA       0x03
#define INTEL_PCI_SUBCLASS_VGA    0x00

// PCI config offsets (byte)
#define INTEL_PCI_BAR0            0x10
#define INTEL_PCI_COMMAND         0x04
#define INTEL_PCI_CMD_BUS_MASTER  (1u << 2)
#define INTEL_PCI_CMD_MEM_SPACE   (1u << 1)

// --- MMIO Register Offsets (byte offset from BAR0) ---

// Chip identification (read-only)
#define INTEL_MMIO_DEVID          0x00013818   // Device ID
#define INTEL_MMIO_REVID          0x00000008   // Revision ID

// General status
#define INTEL_MMIO_ACTHD          0x000040D8   // Active Head Pointer (render)
#define INTEL_MMIO_ARB_STATUS     0x00012054   // Arbitration status

// BCS (Blitter Command Stream) ring registers
// MMIO base + 0x12000 = BCS ring base
#define INTEL_BCS_OFFSET          0x12000
#define INTEL_BCS_RING_BASE       0x12040   // Ring buffer base (write head/tail)
#define INTEL_BCS_RING_TAIL       0x12060   // Ring tail pointer (write here to submit)
#define INTEL_BCS_RING_HEAD       0x12064   // Ring head pointer (hardware advances)
#define INTEL_BCS_RING_CTL        0x12068   // Ring control (enable/clear)
#define INTEL_BCS_IMR             0x1200C   // BCS Interrupt Mask Register

// BCS ring buffer start (within MMIO space)
// Commands are written as DWORDs starting at MMIO base + 0x12100
#define INTEL_BCS_CMD_BASE        0x12100

// BCS ring control bits (RING_CTL @ 0x12068)
#define INTEL_BCS_CTL_ENABLE      (1u << 0)   // bit0 = ring enable

// --- MI (Blitter) opcodes — Phase 7/8 ---
// Stable across Gen8-12. Ref: i915 intel_gpu_commands.h (MI_INSTR).
#define INTEL_MI_NOOP             0x00000000u
#define INTEL_MI_BB_END           (0x0Au << 23)  // MI_BATCH_BUFFER_END
#define INTEL_MI_STORE_DW_IMM     (0x20u << 23)  // MI_STORE_DWORD_IMM
#define INTEL_MI_STORE_LEN        4              // hdr + addr_lo + addr_hi + data

// --- XY 2D commands (Gen8+) — Phase 8 ---
// Ref: i915 XY_SRC_COPY_BLT / XY_COLOR_BLT. Layout comments in
// intel_cmd.c. Encoding HW-validated in Phase 9/10, not here.
#define INTEL_XY_SRC_COPY         ((0x2u << 29) | (0x53u << 22))
#define INTEL_XY_COLOR_BLT        ((0x2u << 29) | (0x50u << 22))
#define INTEL_XY_COPY_LEN         12
#define INTEL_XY_FILL_LEN         8
#define INTEL_ROP_SRCCOPY         0xCCu
#define INTEL_ROP_PATCOPY         0xF0u

// --- BCS status ---
#define INTEL_BCS_BUSY            (1u << 0)

// --- GTT (Global Graphics Translation) Registers ---
// MMIO offsets for GTT page table control (Gen8+).
// The GTT page table lives in system RAM; these registers tell the GPU
// where it is and how big it is.
//
// Reference: i915 intel_mchbar_regs.h, intel_gtt.c

// GTT memory control (Gen8-11: 0x13800C, Gen12: 0x13800C)
#define INTEL_GTT_MEM_CTRL        0x13800C
#define INTEL_GTT_MEM_ENABLE      (1u << 8)   // enable GTT paging

// GTT base address — physical pointer to the GTT page table in system RAM
#define INTEL_GTT_BASE_LOW        0x138010   // bits [31:12] of base, aligned 4KB
#define INTEL_GTT_BASE_HIGH       0x138014   // bits [63:32] of base (Gen12 only, 0 on Gen8-11)

// GTT size — total GPU virtual address space in bytes
// Actual usable = this value * 1024 (register stores size >> 10)
#define INTEL_GTT_SIZE            0x138008

// GTT invalidation trigger
#define INTEL_GTT_INV             0x13801C
#define INTEL_GTT_INV_TRIGGER     (1u << 0)

// --- GTT PTE (Page Table Entry) Format ---
// Gen8-11: 32-bit PTEs (4 bytes each)
// Gen12:   64-bit PTEs (8 bytes each, LO and HI DWORDs)
//
// Both formats: PTE = (phys_addr & ~0xFFF) | valid_bit | cache_bits

// Gen8-11 PTE (32-bit)
#define INTEL_G8_GTT_PTE_VALID    (1u << 0)
#define INTEL_G8_GTT_PTE_CACHE    (0u)        // cache all (WB)
#define INTEL_G8_GTT_PTE_MASK     0x00000FFFULL  // reserved bits mask

// Gen12 PTE (64-bit)
#define INTEL_G12_GTT_PTE_VALID   (1ULL << 0)
#define INTEL_G12_GTT_PTE_CACHE   (0ULL)
#define INTEL_G12_GTT_PTE_MASK    0x00000FFFULL

// --- Blitter Command Opcodes (Gen8+) ---
// Written as the first DWORD of each command packet.
// Bits [31:29] = opcode, remaining bits = operation-specific fields.

// MI (Motorola Interface) commands (common to all engines)
#define INTEL_MI_CMD              (0x0u << 29)
#define INTEL_MI_FLUSH            (0x04u << 23)   // MI_FLUSH_DW
#define INTEL_MI_STORE_DW_IMM     (0x20u << 23)   // MI_STORE_DWORD_IMM

// BLT (Blitter) commands
#define INTEL_BLT_CMD            (0x4u << 29)     // Blitter command prefix

// COPY operations (Gen8+)
// Command: BLT_CMD | (0x43 << 22) | (depth << 19) | ...
// depth: 0 = 8bpp, 1 = 16bpp, 2 = 32bpp
#define INTEL_BLT_COPY           (INTEL_BLT_CMD | (0x43u << 22))
#define INTEL_BLT_COPY_DWORD_DEPTH  2             // 32bpp = depth 2

// FILL operations (Gen8+)
// Command: BLT_CMD | (0xF0 << 22) | (pattern_type << 21) | ...
#define INTEL_BLT_FILL           (INTEL_BLT_CMD | (0xF0u << 22))
#define INTEL_BLT_FILL_SOLID     (0u << 21)       // solid color fill

// Color depth
#define INTEL_BLT_DEPTH_8BPP    0
#define INTEL_BLT_DEPTH_16BPP   1
#define INTEL_BLT_DEPTH_32BPP   2

// Tiling
#define INTEL_BLT_TILING_LINEAR  0
#define INTEL_BLT_TILING_X       (1u << 11)
#define INTEL_BLT_TILING_Y       (1u << 12)

// Rotation
#define INTEL_BLT_ROTATE_0       0
#define INTEL_BLT_ROTATE_90      (1u << 0)

// --- Intel GPU Generation Detection ---
// From device ID ranges (i915 intel_device_info.c)
typedef enum {
    INTEL_GEN_UNKNOWN = 0,
    INTEL_GEN7 = 7,       // Ivy Bridge / Haswell (pre-Broadwell)
    INTEL_GEN8 = 8,       // Broadwell
    INTEL_GEN9 = 9,       // Skylake / Kaby Lake / Coffee Lake
    INTEL_GEN11 = 11,     // Ice Lake
    INTEL_GEN12 = 12,     // Tiger Lake / Alder Lake / Raptor Lake
} intel_gen_t;

// Device ID to generation mapping (common ranges)
// Gen9: 0x3E90-0x3E9F (Coffee Lake GT1), 0x3E92-0x3E9B (GT2)
// Gen12: 0x9A49-0x9A4C (Tiger Lake GT2), 0x4690-0x4693 (Alder Lake GT1)
// Reference: i915 intel_device_info.c
static inline intel_gen_t intel_device_id_to_gen(uint16_t device_id) {
    // Gen12 (Alder Lake / Raptor Lake)
    if (device_id >= 0x4680 && device_id <= 0x469F) return INTEL_GEN12;
    if (device_id >= 0x9A40 && device_id <= 0x9AFF) return INTEL_GEN12;
    if (device_id >= 0x4C80 && device_id <= 0x4C9F) return INTEL_GEN12;  // Rocket Lake
    // Gen11 (Ice Lake)
    if (device_id >= 0x8A50 && device_id <= 0x8A5F) return INTEL_GEN11;
    if (device_id >= 0x8A56 && device_id <= 0x8A5D) return INTEL_GEN11;
    // Gen9 (Skylake / Kaby Lake / Coffee Lake / Comet Lake)
    if (device_id >= 0x1900 && device_id <= 0x19FF) return INTEL_GEN9;
    if (device_id >= 0x3E90 && device_id <= 0x3EFF) return INTEL_GEN9;
    // Gen8 (Broadwell)
    if (device_id >= 0x1600 && device_id <= 0x16FF) return INTEL_GEN8;
    // Gen7 (Haswell / Ivy Bridge)
    if (device_id >= 0x0400 && device_id <= 0x04FF) return INTEL_GEN7;
    return INTEL_GEN_UNKNOWN;
}

// Ring buffer size lives in intel_bcs.h (32KB, 8 pages).
// (Old 128KB define removed — single source of truth.)

// Alignment requirement for GPU buffers
#define INTEL_GPU_PAGE_SIZE      4096

#endif // INTEL_REGS_H
