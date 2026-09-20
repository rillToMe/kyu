// ============================================================
// Gen12 engine discovery — AL-3
// (graphics/backend/intel/intel_gen12_engine.c)
//
// Sources (no guessing):
// - Engine mask: i915 i915_pci.c adl_s_info.platform_engine_mask =
//   RCS0 | BCS0 | VECS0 | VCS0 | VCS2  → exactly one copy engine.
// - ADL-S IDs: i915_pciids.h INTEL_ADLS_IDS =
//   4680-4683, 4690-4693 (+4698/4699, later patch).
// - 0x468B: LKDDb i915_pci.c match + linux-hardware.org "Alder Lake-S",
//   matches host WMI on this machine (REV_0C).
//
// Method limits: presence comes from the platform table (like i915),
// NOT from MMIO engine probing — Gen12 submission uses execlists,
// whose ports are AL-5 territory. Execution proof waits for AL-8.
// No MMIO writes here; nothing is enabled.
// ============================================================

#include "intel_gen12.h"
#include "intel_regs.h"

extern intel_gen_t intel_get_generation(void);
extern uint16_t intel_get_device_id(void);
extern void serial_print(const char* s);

gen12_engines_t g_gen12_engines = { 0, GEN12_ENG_UNKNOWN, 0 };

static int is_adls(uint16_t id) {
    switch (id) {
    case 0x4680: case 0x4681: case 0x4682: case 0x4683:
    case 0x468B:   // LKDDb + linux-hardware.org (this machine, REV_0C)
    case 0x4690: case 0x4691: case 0x4692: case 0x4693:
    case 0x4698: case 0x4699:
        return 1;
    default:
        return 0;
    }
}

int intel_gen12_engine_discovery(void) {
    g_gen12_engines.copy_present = 0;
    g_gen12_engines.copy = GEN12_ENG_UNKNOWN;
    g_gen12_engines.blocked = 0;

    if (intel_get_generation() != INTEL_GEN12) return -1;  // SKIP: legacy owns it
    if (!is_adls(intel_get_device_id())) {
        g_gen12_engines.blocked = 1;                       // BLOCKED: unknown topology
        return -1;
    }
    // ADL-S per i915 adl_s_info: one copy engine (BCS0).
    // Render/video exist but are out of 2D scope — ignored, not enabled.
    g_gen12_engines.copy_present = 1;
    g_gen12_engines.copy = GEN12_ENG_BCS0;
    return 0;
}

void intel_gen12_engine_diag(void) {
    if (intel_get_generation() != INTEL_GEN12) return;     // silent off-Gen12
    if (g_gen12_engines.copy_present) {
        serial_print("Gen12 engines:\n");
        serial_print("  copy: present (BCS0)\n");
        serial_print("  render: ignored\n");
        serial_print("  video: ignored\n");
    } else {
        serial_print("Gen12 engines:\n");
        serial_print("  copy: BLOCKED (unknown platform topology)\n");
        serial_print("Gen12 2D acceleration: BLOCKED\n");
    }
}
