// ============================================================
// Intel GPU Backend — Init + PCI Detection + MMIO Verify
// (graphics/backend/intel_init.c)
//
// Phase 3-4: Detect Intel iGPU via PCI, map BAR0, verify MMIO.
// Phase 2+ stubs: surface/fill/blit/present do CPU fallback
// until the blitter engine is wired up (Phase 7-11).
//
// GHAL vtable: this file provides the full intel_backend_ops.
// ============================================================

#include "ghal.h"
#include "intel_regs.h"
#include "intel_mmio.h"
#include "intel_gtt.h"
#include "intel_gpu_alloc.h"
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_test_copy.h"
#include "intel_test_fill.h"
#include "intel_test_blit.h"
#include "intel_rect.h"
#include "intel_fence.h"
#include "intel_irq.h"
#include "intel_surface.h"
#include "pci.h"
#include "io.h"
#include "heap.h"
#include "display.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// --- External kernel functions ---
extern void kprint(const char* s);
extern void print_hex(uint32_t num);
extern void serial_print(const char* s);
extern uint64_t hhdm_offset;

// --- Intel GPU state ---
static int      g_intel_active = 0;
static uint8_t  g_pci_bus, g_pci_slot, g_pci_func;
static uint16_t g_pci_device_id;
static intel_gen_t g_intel_gen;
static uint64_t g_bar0_phys;
static volatile uint8_t* g_bar0_virt;

// Display mode from framebuffer (boot-fixed)
static display_mode_t g_mode;

// --- PCI Detection (Phase 3) ---

// Scan PCI bus for Intel VGA controller (class 0x03, subclass 0x00).
// Returns 0 if found, -1 if not.
static int intel_pci_find_gpu(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vid = pci_read_word(bus, slot, func, 0);
                if (vid == 0xFFFF) continue;
                if (vid != INTEL_PCI_VENDOR_ID) continue;

                uint32_t class_info = pci_read_word(bus, slot, func, 0x0A);
                uint8_t class_code = (class_info >> 8) & 0xFF;
                uint8_t subclass = class_info & 0xFF;
                if (class_code != INTEL_PCI_CLASS_VGA ||
                    subclass != INTEL_PCI_SUBCLASS_VGA) continue;

                // Found Intel VGA controller
                g_pci_bus = bus;
                g_pci_slot = slot;
                g_pci_func = func;
                g_pci_device_id = pci_read_word(bus, slot, func, 2);
                return 0;
            }
        }
    }
    return -1;
}

// Read BAR0 and extract physical address + size.
// BAR0 on Intel iGPU = MMIO register space.
static int intel_pci_read_bar0(void) {
    uint32_t bar0_low  = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0);
    uint32_t bar0_high = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0 + 4);

    // Determine if 64-bit or 32-bit BAR
    int is_64bit = ((bar0_low & 0x6) == 0x4);
    if (is_64bit) {
        g_bar0_phys = ((uint64_t)bar0_high << 32) | (bar0_low & ~0xFULL);
    } else {
        g_bar0_phys = bar0_low & ~0xFULL;
    }

    if (g_bar0_phys == 0) return -1;
    return 0;
}

// Enable bus mastering + memory space in PCI command register.
static void intel_pci_enable(void) {
    uint16_t cmd = pci_read_word(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_COMMAND);
    cmd |= INTEL_PCI_CMD_BUS_MASTER | INTEL_PCI_CMD_MEM_SPACE;
    pci_write32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_COMMAND, cmd);
}

// --- Surfaces: GPU-backed (Phase 17) with CPU fallback ---
// Private surfaces own a GTT-mapped GPU buffer when possible;
// scanout wraps the physical framebuffer (zero-copy, like the
// software backend). All ops below try the BCS engine first
// (Phase 18) and fall back to CPU pixel access on any failure.

struct ghal_surface {
    uint32_t    width;
    uint32_t    height;
    uint32_t    stride;
    ghal_format_t format;
    uint32_t*   pixels;       // RAM or wrapped fb (NULL when gpu_backed)
    uint8_t     owns_pixels;
    intel_gsurf_t gsurf;      // GPU backing (buf_id -1 = none)
    uint8_t     gpu_backed;
    uint8_t     is_scanout;
};

// Physical framebuffer stash (from ghal_set_framebuffer fan-out).
static uint32_t* g_fb_virt;
static uint32_t  g_fb_w, g_fb_h, g_fb_pitch4;

void intel_backend_set_fb(uint32_t* fb, uint32_t w, uint32_t h, uint32_t pitch_bytes) {
    g_fb_virt = fb; g_fb_w = w; g_fb_h = h;
    g_fb_pitch4 = pitch_bytes / 4;
}

static uint32_t surf_read(struct ghal_surface* s, uint32_t x, uint32_t y) {
    if (s->gpu_backed) return intel_gsurf_read(&s->gsurf, x, y);
    if (!s->pixels || x >= s->width || y >= s->height) return 0;
    return s->pixels[(uint64_t)y * s->stride + x];
}

static void surf_write(struct ghal_surface* s, uint32_t x, uint32_t y, uint32_t v) {
    if (s->gpu_backed) { intel_gsurf_write(&s->gsurf, x, y, v); return; }
    if (!s->pixels || x >= s->width || y >= s->height) return;
    s->pixels[(uint64_t)y * s->stride + x] = v;
}

static struct ghal_surface* intel_surface_create(uint32_t w, uint32_t h, ghal_format_t fmt) {
    if (w == 0 || h == 0) return NULL;
    uint64_t bytes = (uint64_t)w * h * 4;
    if (w != 0 && (bytes / 4 / w) != h) return NULL;

    struct ghal_surface* s = (struct ghal_surface*)kmalloc(sizeof(*s));
    if (!s) return NULL;
    s->width = w;
    s->height = h;
    s->stride = w;
    s->format = fmt;
    s->is_scanout = 0;
    s->gsurf.buf_id = -1;
    if (intel_gsurf_create(&s->gsurf, w, h) == 0) {
        s->pixels = NULL;       // GPU-backed: CPU via gsurf per-page
        s->owns_pixels = 0;
        s->gpu_backed = 1;
        return s;
    }
    s->pixels = (uint32_t*)kmalloc((size_t)bytes);
    if (!s->pixels) { kfree(s); return NULL; }
    memset(s->pixels, 0, (size_t)bytes);
    s->owns_pixels = 1;
    s->gpu_backed = 0;
    return s;
}

static struct ghal_surface* intel_surface_create_scanout(uint32_t w, uint32_t h,
                                                         ghal_format_t fmt) {
    // Wrap the physical fb when known and HHDM-derived phys is sane;
    // otherwise a private surface (upload still lands on screen via
    // compositor's present path on the software backend — never black).
    if (g_fb_virt && g_fb_w && g_fb_h &&
        (uint64_t)g_fb_virt >= hhdm_offset) {
        struct ghal_surface* s = (struct ghal_surface*)kmalloc(sizeof(*s));
        if (!s) return NULL;
        s->pixels = g_fb_virt;
        s->width  = w < g_fb_w ? w : g_fb_w;
        s->height = h < g_fb_h ? h : g_fb_h;
        s->stride = g_fb_pitch4;
        s->format = fmt;
        s->owns_pixels = 0;
        s->gpu_backed = 0;
        s->is_scanout = 1;
        s->gsurf.buf_id = -1;
        return s;
    }
    return intel_surface_create(w, h, fmt);
}

static void intel_surface_destroy(ghal_surface_t* gs) {
    struct ghal_surface* s = (struct ghal_surface*)gs;
    if (!s) return;
    if (s->gpu_backed) intel_gsurf_destroy(&s->gsurf);
    else if (s->owns_pixels && s->pixels) kfree(s->pixels);
    kfree(s);
}

static void intel_surface_upload(ghal_surface_t* gs, const uint32_t* src,
                                 uint32_t src_pitch, ghal_rect_t rect) {
    struct ghal_surface* s = (struct ghal_surface*)gs;
    if (!s || !src) return;
    if (rect.x >= s->width || rect.y >= s->height) return;
    if (rect.w == 0 || rect.h == 0) return;
    uint32_t maxw = s->width - rect.x;
    uint32_t maxh = s->height - rect.y;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;

    // CPU->device transfer by definition (Phase 19: program-ordered
    // writes precede any later GPU submit via bcs barrier).
    if (!s->gpu_backed) {
        const uint32_t* src_row = src + (uint64_t)rect.y * src_pitch + rect.x;
        uint32_t* dst_row = s->pixels + (uint64_t)rect.y * s->stride + rect.x;
        for (uint32_t y = 0; y < rect.h; y++) {
            memcpy(dst_row, src_row, rect.w * 4);
            src_row += src_pitch;
            dst_row += s->stride;
        }
        return;
    }
    for (uint32_t y = 0; y < rect.h; y++) {
        const uint32_t* src_row = src + (uint64_t)(rect.y + y) * src_pitch + rect.x;
        for (uint32_t x = 0; x < rect.w; x++)
            surf_write(s, rect.x + x, rect.y + y, src_row[x]);
    }
}

static void intel_fill_rect(ghal_surface_t* gs, ghal_rect_t rect, uint32_t argb) {
    struct ghal_surface* s = (struct ghal_surface*)gs;
    if (!s) return;
    if (rect.x >= s->width || rect.y >= s->height) return;
    uint32_t maxw = s->width - rect.x;
    uint32_t maxh = s->height - rect.y;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;
    uint32_t color = argb & 0xFFFFFF;

    // Phase 18: HW fill on private GPU surfaces; Phase 12 gate first.
    if (s->gpu_backed && !s->is_scanout && intel_bcs_is_available()) {
        intel_gpu_buffer_t* b = intel_gpu_buffer_get(s->gsurf.buf_id);
        if (b) {
            intel_rect_t r = {rect.x, rect.y, rect.w, rect.h};
            if (intel_fill_validate(b, &r) == 0) {
                intel_cmd_t c;
                intel_cmd_begin(&c);
                intel_fence_t f;
                if (intel_cmd_emit_fill(&c, b->gpu_vaddr, b->stride, color,
                                        r.x, r.y, r.w, r.h) == 0 &&
                    intel_gpu_submit(&c, &f) == 0 &&
                    intel_fence_wait_sleep(&f, 0) == 0)
                    return; // Phase 19: GPU done before CPU proceeds
            }
        }
    }
    // Phase 21: linear RAM fast path (row loops, no per-pixel calls).
    if (!s->gpu_backed && s->pixels) {
        for (uint32_t y = 0; y < rect.h; y++) {
            uint32_t* row = s->pixels + (uint64_t)(rect.y + y) * s->stride + rect.x;
            for (uint32_t x = 0; x < rect.w; x++) row[x] = color;
        }
        return;
    }
    for (uint32_t y = 0; y < rect.h; y++)
        for (uint32_t x = 0; x < rect.w; x++)
            surf_write(s, rect.x + x, rect.y + y, color);
}

static void intel_blit(ghal_surface_t* gs, ghal_rect_t dst_rect,
                       ghal_surface_t* gsrc, ghal_rect_t src_rect) {
    struct ghal_surface* dst = (struct ghal_surface*)gs;
    struct ghal_surface* src = (struct ghal_surface*)gsrc;
    if (!dst || !src) return;
    if (dst_rect.w != src_rect.w || dst_rect.h != src_rect.h) return;

    if (dst_rect.x >= dst->width || dst_rect.y >= dst->height) return;
    uint32_t w = dst_rect.w, h = dst_rect.h;
    if (w > dst->width - dst_rect.x) w = dst->width - dst_rect.x;
    if (h > dst->height - dst_rect.y) h = dst->height - dst_rect.y;
    if (src_rect.x >= src->width || src_rect.y >= src->height) return;
    if (w > src->width - src_rect.x) w = src->width - src_rect.x;
    if (h > src->height - src_rect.y) h = src->height - src_rect.y;

    // Phase 18: HW 1:1 blit between private GPU surfaces.
    if (dst->gpu_backed && src->gpu_backed &&
        !dst->is_scanout && !src->is_scanout &&
        intel_bcs_is_available()) {
        intel_gpu_buffer_t* db = intel_gpu_buffer_get(dst->gsurf.buf_id);
        intel_gpu_buffer_t* sb = intel_gpu_buffer_get(src->gsurf.buf_id);
        if (db && sb) {
            intel_rect_t dr = {dst_rect.x, dst_rect.y, w, h};
            intel_rect_t sr = {src_rect.x, src_rect.y, w, h};
            if (intel_blit_validate(db, sb, &sr, &dr) == 0) {
                intel_cmd_t c;
                intel_cmd_begin(&c);
                intel_fence_t f;
                if (intel_cmd_emit_blit(&c, db->gpu_vaddr, db->stride,
                                        sb->gpu_vaddr, sb->stride,
                                        sr.x, sr.y, dr.x, dr.y,
                                        sr.w, sr.h) == 0 &&
                    intel_gpu_submit(&c, &f) == 0 &&
                    intel_fence_wait_sleep(&f, 0) == 0)
                    return;
            }
        }
    }
    // Phase 21: linear-RAM fast path (row memcpy, pre-Phase-17 shape).
    if (!dst->gpu_backed && !src->gpu_backed && dst->pixels && src->pixels) {
        const uint32_t* srow = src->pixels + (uint64_t)src_rect.y * src->stride + src_rect.x;
        uint32_t* drow = dst->pixels + (uint64_t)dst_rect.y * dst->stride + dst_rect.x;
        for (uint32_t y = 0; y < h; y++) {
            memcpy(drow, srow, w * 4);
            srow += src->stride;
            drow += dst->stride;
        }
        return;
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            surf_write(dst, dst_rect.x + x, dst_rect.y + y,
                       surf_read(src, src_rect.x + x, src_rect.y + y));
}

static void intel_present(ghal_surface_t* gs, const ghal_rect_t* rect) {
    // Phase 19: synchronous model — every HW op above fence-waits
    // before returning, so nothing is outstanding here. Scanout
    // pixels are already on screen (upload wrote them).
    (void)gs; (void)rect;
}

// --- Display mode (boot-fixed) ---

static int intel_mode_get(display_mode_t* out) {
    if (!out || g_mode.width == 0) return -1;
    *out = g_mode;
    return 0;
}

static int intel_mode_enumerate(display_mode_t* out, uint32_t max) {
    if (!out || max == 0) return -1;
    if (g_mode.width == 0) return -1;
    out[0] = g_mode;
    return 1;
}

// --- GHAL Backend Init ---

static int intel_init(void) {
    if (g_intel_active) return 0;

    // Phase 3: PCI GPU detection
    if (intel_pci_find_gpu() != 0) {
        serial_print("[intel] no Intel VGA GPU found\n");
        return -1;  // No Intel GPU → let software backend take over
    }

    // Identify GPU generation
    g_intel_gen = intel_device_id_to_gen(g_pci_device_id);

    // Log detection
    serial_print("[intel] GPU detected\n");

    // Phase 3: Read BAR0
    if (intel_pci_read_bar0() != 0) {
        serial_print("[intel] BAR0 invalid\n");
        return -1;
    }

    // Enable bus mastering + memory space
    intel_pci_enable();

    // Map BAR0 via HHDM
    g_bar0_virt = (volatile uint8_t*)(g_bar0_phys + hhdm_offset);

    // Phase 4: Initialize MMIO helpers and verify access
    intel_mmio_init(g_bar0_virt);
    if (intel_mmio_verify() != 0) {
        serial_print("[intel] MMIO verify failed\n");
        g_bar0_virt = NULL;
        return -1;
    }

    // Read actual device ID from MMIO to confirm (DEVID in PCI config differs
    // from MMIO DEVID on some platforms — MMIO DEVID is authoritative).
    uint32_t mmio_devid = intel_mmio_read32(INTEL_MMIO_DEVID);
    serial_print("[intel] MMIO DEVID=");
    print_hex(mmio_devid);
    serial_print("\n");

    // Get framebuffer mode (set by display_boot_init before ghal_init)
    const display_mode_t* m = display_get_mode();
    if (m) {
        g_mode = *m;
    } else {
        // Fallback: assume 1920x1080
        g_mode.width = 1920;
        g_mode.height = 1080;
        g_mode.pitch_bytes = 1920 * 4;
        g_mode.bpp = 32;
        g_mode.format = DISPLAY_FMT_XRGB8888;
    }

    g_intel_active = 1;

    // Phase 5: Initialize GPU buffer allocator
    intel_gpu_alloc_init();

    // Phase 6: Initialize GTT (GPU page table)
    if (intel_gtt_init() != 0) {
        serial_print("[intel] GTT init failed — GPU acceleration disabled\n");
        // GTT failure is non-fatal: CPU fallback still works.
        // Mark g_intel_active as 1 anyway — surface ops use CPU memcpy.
    }

    // Phase 7: BCS ring init (Gen8-11 legacy; Gen12 reports
    // unavailable pending execlists — non-fatal, fallback stays).
    int bcs_ok = intel_bcs_init();

    // Phase 8: status page + builder self-test (no HW touch).
    if (intel_cmd_status_init() != 0)
        serial_print("[intel] status page failed — fences deferred\n");
    intel_cmd_selftest();

    // Phase 12: rect/boundary gate self-test (pure CPU).
    intel_rect_selftest();

    // Phase 13: fence round-trip (HW when BCS live, else semantics).
    intel_fence_test();

    // Phase 9: HW COPY test (non-fatal — boot continues regardless).
    intel_test_copy_run();

    // Phase 10: HW FILL tests (non-fatal).
    intel_test_fill_run();

    // Phase 11: HW BLIT tests (non-fatal).
    intel_test_blit_run();

    // Phase 14: completion observations via sleep-wait path.
    intel_irq_log();

    // Phase 16: GPU-backed surface abstraction (needs GTT only).
    intel_gsurf_selftest();

    if (bcs_ok == 0) {
        kprint("[intel] backend initialized (BCS available)\n");
        serial_print("[intel] backend ready — BCS ring live\n");
    } else {
        kprint("[intel] backend initialized (CPU fallback mode)\n");
        serial_print("[intel] backend ready — 2D acceleration pending blitter init\n");
    }
    return 0;
}

static void intel_shutdown(void) {
    g_intel_active = 0;
    g_bar0_virt = NULL;
}

// --- Phase 15: acceleration info (BCS-live = HW fill/blit) ---

static int intel_accel_enabled(void) { return intel_bcs_is_available() ? 1 : 0; }
static const char* intel_engine_name(void) { return "BCS"; }

// --- GHAL Backend vtable ---

const ghal_backend_ops_t intel_backend_ops = {
    .name           = "intel",
    .capabilities   = GHAL_CAP_PARTIAL_FLUSH,
    .init           = intel_init,
    .shutdown       = intel_shutdown,
    .surface_create = intel_surface_create,
    .surface_destroy= intel_surface_destroy,
    .surface_create_scanout = intel_surface_create_scanout,
    .surface_upload = intel_surface_upload,
    .fill_rect      = intel_fill_rect,
    .blit           = intel_blit,
    .present        = intel_present,
    .present_fence  = NULL,
    .fence_pending  = NULL,
    .fence_wait     = NULL,
    .cursor_update  = NULL,
    .cursor_move    = NULL,
    .gpu_stats      = NULL,
    .mode_get       = intel_mode_get,
    .mode_enumerate = intel_mode_enumerate,
    .mode_set       = NULL,
    .mode_changed   = NULL,
    .acceleration_enabled = intel_accel_enabled,
    .engine_name    = intel_engine_name,
};

// --- Accessors for blitter module (Phase 7+) ---

intel_gen_t intel_get_generation(void) { return g_intel_gen; }
uint16_t intel_get_device_id(void) { return g_pci_device_id; }
int intel_is_active(void) { return g_intel_active; }
