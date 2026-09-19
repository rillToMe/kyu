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
#include "intel_gen12.h"
#include "intel_gen12_ctx.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include "intel_gen12_ppgtt.h"
#include "intel_gen12_test_copy.h"
#include "intel_gen12_test_fill.h"
#include "intel_gen12_test_blit.h"
#include "intel_gen12_bench.h"
#include "intel_gen12_robust.h"
#include "intel_gen12_fault.h"
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
extern void serial_print_hex(uint64_t v);
extern void serial_dec(uint64_t v);
extern uint64_t hhdm_offset;

// --- Intel GPU state ---
static int      g_intel_active = 0;
static uint8_t  g_pci_bus, g_pci_slot, g_pci_func;
static uint16_t g_pci_device_id;
static uint8_t  g_pci_rev;        // AL-1: PCI revision ID (offset 0x08)
static uint32_t g_pci_class24;    // AL-1: class/subclass/prog-if (offset 0x08 dword >> 8)
static intel_gen_t g_intel_gen;
static uint64_t g_bar0_phys;
static uint64_t g_bar0_size;      // AL-1: probed BAR0 size, 0 = unknown
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
                // AL-1: capture revision + full class dword (offset 0x08:
                // rev[7:0] prog-if[15:8] subclass[23:16] class[31:24]).
                {
                    uint32_t c08 = pci_read32(bus, slot, func, 0x08);
                    g_pci_rev = (uint8_t)(c08 & 0xFF);
                    g_pci_class24 = (c08 >> 8) & 0xFFFFFFu;
                }
                return 0;
            }
        }
    }
    return -1;
}

// AL-1: probe BAR0 size (standard save → all-ones → mask → restore).
// Config-space only, no MMIO touch. Result in g_bar0_size, 0 = unknown.
static void intel_pci_probe_bar0_size(void) {
    g_bar0_size = 0;
    uint32_t save_lo = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0);
    int is_64bit = ((save_lo & 0x6) == 0x4);
    uint32_t save_hi = 0;
    if (is_64bit)
        save_hi = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0 + 4);

    pci_write32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0, 0xFFFFFFFFu);
    uint32_t mask_lo = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0);
    uint64_t mask = mask_lo & ~0xFULL;
    if (is_64bit) {
        pci_write32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0 + 4, 0xFFFFFFFFu);
        uint32_t mask_hi = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0 + 4);
        mask |= (uint64_t)mask_hi << 32;
    }

    pci_write32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0, save_lo);
    if (is_64bit)
        pci_write32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0 + 4, save_hi);

    if (mask == 0) return;
    g_bar0_size = (~mask) + 1;
}

static const char* intel_gen_name(intel_gen_t g) {
    switch (g) {
    case INTEL_GEN7:  return "Gen7";
    case INTEL_GEN8:  return "Gen8";
    case INTEL_GEN9:  return "Gen9";
    case INTEL_GEN11: return "Gen11";
    case INTEL_GEN12: return "Gen12";
    default:          return "UNKNOWN";
    }
}

// AL-1: full PCI discovery diagnostic. Print-only, fail-closed.
static void intel_pci_diag(void) {
    intel_pci_probe_bar0_size();
    serial_print("Intel GPU:\n");
    serial_print("  vendor: ");
    serial_print_hex(INTEL_PCI_VENDOR_ID);
    serial_print("\n  device: ");
    serial_print_hex(g_pci_device_id);
    serial_print("\n  revision: ");
    serial_print_hex(g_pci_rev);
    serial_print("\n  class: ");
    serial_print_hex(g_pci_class24);
    serial_print("\n  BAR0: ");
    serial_print_hex(g_bar0_phys);
    serial_print("\n  BAR0 size: ");
    if (g_bar0_size) serial_dec(g_bar0_size);
    else serial_print("unknown");
    serial_print("\n  generation: ");
    serial_print(intel_gen_name(g_intel_gen));
    serial_print("\n");
}

// Read BAR0 and extract physical address.
// BAR0 on Intel iGPU = MMIO register space.
// AL-2: validate BAR type before trusting it — must be a memory BAR
// (bit0==0), locatable type (00=32bit, 10=64bit; 01 is reserved),
// non-zero, and 4KB-aligned. Anything else → invalid, fail closed.
static int intel_pci_read_bar0(void) {
    uint32_t bar0_low  = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0);
    uint32_t bar0_high = pci_read32(g_pci_bus, g_pci_slot, g_pci_func, INTEL_PCI_BAR0 + 4);

    if (bar0_low & 0x1) return -1;              // I/O BAR — not MMIO
    uint32_t type = bar0_low & 0x6;
    if (type != 0x0 && type != 0x4) return -1;  // reserved type
    int is_64bit = (type == 0x4);
    if (is_64bit) {
        g_bar0_phys = ((uint64_t)bar0_high << 32) | (bar0_low & ~0xFULL);
    } else {
        g_bar0_phys = bar0_low & ~0xFULL;
    }

    if (g_bar0_phys == 0) return -1;
    if (g_bar0_phys & 0xFFFULL) return -1;      // must be page-aligned
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

// --- AL-13: Gen12 dispatch (GHAL → Gen12 path, silent CPU fallback) ---
// Only attempted when the boot STORE proved the engine live. Any
// submit/wait failure clears live (fail-fast: no per-frame stalls).
// Staging failures keep live (cheap to retry, no waits involved).

static void gen12_flush_buf(intel_gpu_buffer_t* b) {
    if (!b) return;
    for (uint32_t p = 0; p < b->num_pages; p++)
        gen12_cpu_clflush(b->phys_addrs[p], 4096);
}

// Stage a sealed batch through the single owner + wait.
// 0 = GPU done; -1 = staging (keep live); -2 = submit/wait (kill live).
static int gen12_run_batch(gen12_batch_t* b, gen12_fence_t* f) {
    if (gen12_submit_commit(b->cpu, b->n) != 0) return -2;
    return gen12_fence_wait(f, 0) == 0 ? 0 : -2;
}

// Caller holds a Phase-12-validated rect. 0 = GPU done, -1 = CPU fallback.
static int gen12_fill_hw(intel_gpu_buffer_t* b, intel_rect_t* r,
                         uint32_t color) {
    if (!gen12_is_live()) return -1;
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) return -1;
    gen12_batch_t bt;
    if (gen12_batch_begin(&bt) != 0) return -1;
    int ok = gen12_batch_emit_fill(&bt, b->gpu_vaddr, b->stride, color,
                                   r->x, r->y, r->w, r->h);
    if (ok == 0) ok = gen12_fence_emit(&bt, &f);
    if (ok == 0) ok = gen12_batch_end(&bt);
    if (ok == 0) ok = gen12_run_batch(&bt, &f);
    gen12_batch_free(&bt);
    if (ok == -2) gen12_note_hw_failure();
    if (ok != 0) { gen12_count_fb(); return -1; }
    gen12_flush_buf(b);   // GPU output for subsequent CPU readers
    gen12_count_hw();
    return 0;
}

static int gen12_blit_hw(intel_gpu_buffer_t* db, intel_gpu_buffer_t* sb,
                         intel_rect_t* sr, intel_rect_t* dr) {
    if (!gen12_is_live()) return -1;
    gen12_flush_buf(sb);   // CPU-written pattern visible to GPU
    gen12_fence_t f;
    if (gen12_fence_alloc(&f) != 0) return -1;
    gen12_batch_t bt;
    if (gen12_batch_begin(&bt) != 0) return -1;
    int ok = gen12_batch_emit_blit(&bt, db->gpu_vaddr, db->stride,
                                   sb->gpu_vaddr, sb->stride,
                                   sr->x, sr->y, dr->x, dr->y,
                                   sr->w, sr->h);
    if (ok == 0) ok = gen12_fence_emit(&bt, &f);
    if (ok == 0) ok = gen12_batch_end(&bt);
    if (ok == 0) ok = gen12_run_batch(&bt, &f);
    gen12_batch_free(&bt);
    if (ok == -2) gen12_note_hw_failure();
    if (ok != 0) { gen12_count_fb(); return -1; }
    gen12_flush_buf(db);
    gen12_count_hw();
    return 0;
}

static void intel_fill_rect(ghal_surface_t* gs, ghal_rect_t rect, uint32_t argb) {    struct ghal_surface* s = (struct ghal_surface*)gs;
    if (!s) return;
    if (rect.x >= s->width || rect.y >= s->height) return;
    uint32_t maxw = s->width - rect.x;
    uint32_t maxh = s->height - rect.y;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;
    uint32_t color = argb & 0xFFFFFF;

    // Phase 18: HW fill on private GPU surfaces; Phase 12 gate first.
    // AL-13: Gen12 path preferred when live, legacy BCS otherwise.
    if (s->gpu_backed && !s->is_scanout && gen12_is_live()) {
        intel_gpu_buffer_t* b = intel_gpu_buffer_get(s->gsurf.buf_id);
        if (b) {
            intel_rect_t r = {rect.x, rect.y, rect.w, rect.h};
            if (intel_fill_validate(b, &r) == 0 &&
                gen12_fill_hw(b, &r, color) == 0)
                return;
        }
    }
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
    // AL-13: Gen12 path preferred when live, legacy BCS otherwise.
    if (dst->gpu_backed && src->gpu_backed &&
        !dst->is_scanout && !src->is_scanout &&
        gen12_is_live()) {
        intel_gpu_buffer_t* db = intel_gpu_buffer_get(dst->gsurf.buf_id);
        intel_gpu_buffer_t* sb = intel_gpu_buffer_get(src->gsurf.buf_id);
        if (db && sb) {
            intel_rect_t dr = {dst_rect.x, dst_rect.y, w, h};
            intel_rect_t sr = {src_rect.x, src_rect.y, w, h};
            if (intel_blit_validate(db, sb, &sr, &dr) == 0 &&
                gen12_blit_hw(db, sb, &sr, &dr) == 0)
                return;
        }
    }
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

    // Identify GPU generation (ranges from i915 intel_device_info.c;
    // 0x468B = Alder Lake-S GT1 UHD = Gen12: confirmed via LKDDb
    // i915_pci.c match + host WMI PNPDeviceID on this machine).
    g_intel_gen = intel_device_id_to_gen(g_pci_device_id);

    // Phase 3: Read BAR0
    if (intel_pci_read_bar0() != 0) {
        serial_print("[intel] BAR0 invalid\n");
        return -1;
    }

    // AL-1: full PCI discovery diagnostic (print-only, fail-closed).
    intel_pci_diag();

    // Enable bus mastering + memory space
    intel_pci_enable();

    // Map BAR0 via HHDM
    g_bar0_virt = (volatile uint8_t*)(g_bar0_phys + hhdm_offset);

    // Phase 4 / AL-2: Initialize MMIO helpers and verify access.
    // Runs on BSP before SMP bring-up (ghal_init precedes smp_init),
    // so no serialization needed here; runtime MMIO is fenced by
    // g_bcs_lock in intel_bcs.c. Never exposed through GHAL.
    intel_mmio_init(g_bar0_virt);
    if (intel_mmio_verify() != 0) {
        serial_print("[intel] MMIO verify failed\n");
        if (g_intel_gen == INTEL_GEN12) {
            serial_print("Gen12 MMIO:\n  MMIO: unavailable\n");
            serial_print("Gen12 acceleration: disabled\n");
        }
        g_bar0_virt = NULL;
        return -1;
    }
    serial_print("[intel] MMIO probe: PASS (BAR0 ");
    serial_print_hex(g_bar0_phys);
    serial_print(")\n");

    // AL-3: Gen12 engine discovery (table-only, nothing enabled).
    // Non-fatal by design: SKIP off-Gen12, BLOCKED on unknown topology.
    intel_gen12_engine_discovery();
    intel_gen12_engine_diag();

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

    // AL-4: Gen12 VM gate (GGTT-only minimum; legacy GTT untouched).
    if (g_intel_gen == INTEL_GEN12)
        serial_print("Gen12 VM: GGTT only (no PPGTT)\n");
    intel_gen12_vm_selftest();

    // AL-5: Gen12 context foundation (alloc+image+map, no submission).
    intel_gen12_ctx_create();

    // AL-6: Gen12 batch buffer (build+validate, no submission).
    gen12_batch_selftest();

    // AL-7: Gen12 fence (status page + bounded wait, no submission).
    gen12_fence_selftest();

    // AL-8: minimal submit (STORE-to-status through ELSP, fenced).
    // Non-fatal by design: PASS / SKIP / BLOCKED only.
    // AL-9: COPY tests run only on live submission — without it every
    // case would burn a full fence timeout; SKIP says why instead.
    if (gen12_submit_init() == 0) {
        if (gen12_submit_store_test() == 0 &&
            gen12_ppgtt_init() == 0) {
            gen12_test_copy_run();
            gen12_test_fill_run();
            gen12_test_blit_run();
            gen12_bench_run();
            gen12_submit_smp_selftest();
        } else {
            serial_print("[gen12_copy] SKIP (no live submission)\n");
        }
    }

    // AL-16: robustness runs on every intel-backend boot (pure-API
    // rejections need no engine; fail-closed proofs need no-live).
    gen12_robust_selftest();

    // AL-17: fault/timeout handling (self-gated on fence state).
    gen12_fault_selftest();

    // AL-18: final Gen12 diagnostic (roadmap format). acceleration:
    // enabled appears here IFF the boot STORE proved the engine —
    // never merely because PCI detection succeeded.
    if (g_intel_gen == INTEL_GEN12) {
        int live = gen12_is_live();
        serial_print("GPU:\n  backend: intel\n  generation: Gen12\n");
        serial_print(live ? "  engine: BCS0\n  submission: gen12\n"
                          "  acceleration: enabled\n"
                        : "  engine: BCS0\n  submission: unavailable\n"
                          "  acceleration: disabled\n  fallback: software\n");
    }

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

static int intel_accel_enabled(void) {
    if (gen12_is_live()) return 1;
    return intel_bcs_is_available() ? 1 : 0;
}
static const char* intel_engine_name(void) {
    if (gen12_is_live()) return "BCS0";
    return "BCS";
}

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
