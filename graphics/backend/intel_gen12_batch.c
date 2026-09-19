// ============================================================
// Gen12 batch buffer — AL-6
// (graphics/backend/intel_gen12_batch.c)
//
// LAYOUT SOURCES (all executed on real Gen12 HW, none guessed):
// - XY opcodes 0x50/0x53 on Gen12: Intel PRM OSRC TGL Vol 10
//   (Copy Engine) 2D command map.
// - XY_SRC_COPY 10-DW order: IGT benchmarks/gem_latency.c +
//   gem_streaming_writes.c + 2025 benchmarks/blt patch
//   (COPY_BLT_CMD|WRITE_ALPHA|WRITE_RGB [+2 for 64-bit],
//   dw1 = ROP|bit25|bit24|pitch, coords before addrs).
// - XY_COLOR_BLT 7-DW order: i915 intel_migrate.c emit_clear()
//   (hdr|WRITE_RGBA|(7-2), dw1 = DEPTH|ROP|pitch).
// - 32bpp XY depth = 3 (i915 BLT_DEPTH_32, IGT bit25+24).
//   (Legacy Phase-8 builders use depth 2 and a 12/8-DW order —
//   NOT reused here, NOT changed here: frozen, reported separately.)
// - BB_END plain (IGT batches; i915 adds BIT(0) only inside the
//   LRC restore image, never in submitted batches).
//
// WHAT THIS PHASE DOES NOT DO: submit (AL-8), enable engines,
// interrupts, GHAL wiring, or reinterpret Phase-12 validation.
// Full buffer-span checks wait for AL-8 (buffer objects known);
// here every embedded address is page-checked in-window.
// ============================================================

#include "intel_gen12_batch.h"
#include "intel_gen12.h"
#include "intel_gtt.h"
#include "pmm.h"

extern void serial_print(const char* s);
extern uint64_t hhdm_offset;

#define XY_COPY_OP   ((0x2u << 29) | (0x53u << 22))
#define XY_FILL_OP   ((0x2u << 29) | (0x50u << 22))
#define WR_A         (1u << 21)
#define WR_RGB       (1u << 20)
#define DEPTH_32     (3u << 24)
#define ROP_SRC      (0xCCu << 16)
#define ROP_PAT      (0xF0u << 16)
#define MI_BB_END    (0x0Au << 23)

#define COPY_DW 10
#define FILL_DW 7

int gen12_batch_begin(gen12_batch_t* b) {
    if (!b) return -1;
    b->cpu = 0; b->phys = 0; b->gpu = 0;
    b->n = 0; b->sealed = 0; b->backed = 0;
    if (!intel_gtt_is_active()) {
        serial_print("[gen12_batch] SKIP (GTT unavailable)\n");
        return -1;
    }
    phys_addr_t pg = pmm_alloc_page();
    if (pg == PHYS_NULL) {
        serial_print("[gen12_batch] SKIP (no PMM page)\n");
        return -1;
    }
    uint32_t idx = intel_gtt_map_pages(0, &pg, 1);   // bump, in-window
    if (idx == (uint32_t)-1) {
        pmm_free_page(pg);
        serial_print("[gen12_batch] FAIL (GGTT map)\n");
        return -1;
    }
    b->phys = pg;
    b->gpu = (uint64_t)idx * 4096ULL;
    if (intel_gen12_vm_valid(b->gpu, 4096) != 0) {
        intel_gtt_unmap(idx, 1);
        pmm_free_page(pg);
        b->phys = 0; b->gpu = 0;
        serial_print("[gen12_batch] FAIL (backing outside GGTT)\n");
        return -1;
    }
    b->cpu = (uint32_t*)(pg + hhdm_offset);
    for (uint32_t i = 0; i < 1024; i++) b->cpu[i] = 0;
    b->backed = 1;
    return 0;
}

void gen12_batch_free(gen12_batch_t* b) {
    if (!b || !b->backed) return;
    intel_gtt_unmap((uint32_t)(b->gpu >> 12), 1);
    pmm_free_page(b->phys);
    b->cpu = 0; b->phys = 0; b->gpu = 0;
    b->n = 0; b->sealed = 0; b->backed = 0;
}

static int room(gen12_batch_t* b, uint32_t need) {
    if (!b || !b->cpu || b->sealed) return -1;
    if (need > GEN12_BATCH_DWORDS) return -1;
    if (b->n + need > GEN12_BATCH_DWORDS) return -1;
    return 0;
}

// Geometry sanity: 16-bit fields, no wrap. (Clipping itself is Phase-12.)
static int geom(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) return -1;
    if (x > 0xFFFFu || y > 0xFFFFu || w > 0xFFFFu || h > 0xFFFFu) return -1;
    if ((uint64_t)x + w > 0x10000ULL) return -1;
    if ((uint64_t)y + h > 0x10000ULL) return -1;
    return 0;
}

static int pitch_bytes(uint32_t px, uint32_t* out) {
    uint64_t v = (uint64_t)px * 4;
    if (v == 0 || v > 0xFFFFULL) return -1;
    *out = (uint32_t)v;
    return 0;
}

// Containing page must sit inside the GGTT window.
static int addr_ok(uint64_t gpu) {
    if (gpu & 0xFFFULL) return -1;
    return intel_gen12_vm_valid(gpu & ~0xFFFULL, 4096);
}

static void put(gen12_batch_t* b, uint32_t v) { b->cpu[b->n++] = v; }

static int emit_copy_like(gen12_batch_t* b, uint64_t dst, uint32_t dst_pb,
                          uint64_t src, uint32_t src_pb,
                          uint32_t sx, uint32_t sy,
                          uint32_t dx, uint32_t dy,
                          uint32_t w, uint32_t h) {
    if (room(b, COPY_DW) != 0) return -1;
    if (geom(dx, dy, w, h) != 0) return -1;
    if (geom(sx, sy, w, h) != 0) return -1;
    if (addr_ok(dst) != 0 || addr_ok(src) != 0) return -1;
    put(b, XY_COPY_OP | 8 | WR_A | WR_RGB);
    put(b, ROP_SRC | (1u << 25) | (1u << 24) | dst_pb);
    put(b, (dy << 16) | (dx & 0xFFFFu));
    put(b, (h << 16) | (w & 0xFFFFu));
    put(b, (uint32_t)(dst & 0xFFFFFFFFu));
    put(b, (uint32_t)(dst >> 32));
    put(b, (sy << 16) | (sx & 0xFFFFu));
    put(b, src_pb);
    put(b, (uint32_t)(src & 0xFFFFFFFFu));
    put(b, (uint32_t)(src >> 32));
    return 0;
}

int gen12_batch_emit_copy(gen12_batch_t* b, uint64_t dst, uint32_t dst_px,
                          uint64_t src, uint32_t src_px,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t dpb, spb;
    if (pitch_bytes(dst_px, &dpb) != 0) return -1;
    if (pitch_bytes(src_px, &spb) != 0) return -1;
    return emit_copy_like(b, dst, dpb, src, spb, x, y, x, y, w, h);
}

int gen12_batch_emit_blit(gen12_batch_t* b, uint64_t dst, uint32_t dst_px,
                          uint64_t src, uint32_t src_px,
                          uint32_t sx, uint32_t sy,
                          uint32_t dx, uint32_t dy,
                          uint32_t w, uint32_t h) {
    uint32_t dpb, spb;
    if (pitch_bytes(dst_px, &dpb) != 0) return -1;
    if (pitch_bytes(src_px, &spb) != 0) return -1;
    return emit_copy_like(b, dst, dpb, src, spb, sx, sy, dx, dy, w, h);
}

int gen12_batch_emit_fill(gen12_batch_t* b, uint64_t dst, uint32_t dst_px,
                          uint32_t color,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t dpb;
    if (room(b, FILL_DW) != 0) return -1;
    if (geom(x, y, w, h) != 0) return -1;
    if (pitch_bytes(dst_px, &dpb) != 0) return -1;
    if (addr_ok(dst) != 0) return -1;
    put(b, XY_FILL_OP | 5 | WR_A | WR_RGB);
    put(b, DEPTH_32 | ROP_PAT | dpb);
    put(b, (uint32_t)(dst & 0xFFFFFFFFu));
    put(b, (uint32_t)(dst >> 32));
    put(b, color);
    put(b, (y << 16) | (x & 0xFFFFu));
    put(b, (h << 16) | (w & 0xFFFFu));
    return 0;
}

int gen12_batch_end(gen12_batch_t* b) {
    if (room(b, 1) != 0) return -1;
    put(b, MI_BB_END);
    b->sealed = 1;
    return 0;
}

uint64_t gen12_batch_gpu_addr(const gen12_batch_t* b) {
    if (!b || !b->backed || b->n == 0) return 0;
    return b->gpu;
}

static int g_batch_tested = 0;

void gen12_batch_selftest(void) {
    if (g_batch_tested) return;
    g_batch_tested = 1;

    // Pure structural test on scratch (no GPU needed — runs everywhere).
    static uint32_t scratch[GEN12_BATCH_DWORDS];
    gen12_batch_t t;
    t.cpu = scratch; t.phys = 0; t.gpu = 0;
    t.n = 0; t.sealed = 0; t.backed = 0;
    for (uint32_t i = 0; i < GEN12_BATCH_DWORDS; i++) scratch[i] = 0xDEADDEADu;

    // COPY: dst=0x100000/64px, src=0x200000/64px, 8x8 at 0,0.
    if (gen12_batch_emit_copy(&t, 0x100000ULL, 64, 0x200000ULL, 64,
                              0, 0, 8, 8) != 0) goto fail;
    if (t.n != COPY_DW) goto fail;
    if (scratch[0] != 0x54F00008u) goto fail;             // hdr|8|WA|WR
    if (scratch[1] != (0xCC0000u | 0x3000000u | 256u)) goto fail;
    if (scratch[2] != 0 || scratch[3] != 0x80008u) goto fail;
    if (scratch[4] != 0x100000u || scratch[5] != 0) goto fail;
    if (scratch[6] != 0 || scratch[7] != 256u) goto fail;
    if (scratch[8] != 0x200000u || scratch[9] != 0) goto fail;

    // FILL: dst=0x100000/64px, color, 8x8.
    if (gen12_batch_emit_fill(&t, 0x100000ULL, 64, 0xFF00FF00u,
                              0, 0, 8, 8) != 0) goto fail;
    if (t.n != COPY_DW + FILL_DW) goto fail;
    if (scratch[10] != 0x54300005u) goto fail;            // hdr|5|WA|WR
    if (scratch[11] != (0x3000000u | 0xF00000u | 256u)) goto fail;
    if (scratch[12] != 0x100000u || scratch[13] != 0) goto fail;
    if (scratch[14] != 0xFF00FF00u) goto fail;
    if (scratch[15] != 0 || scratch[16] != 0x80008u) goto fail;

    // Rejections: zero-size, wrap, bad pitch, misaligned/OOB addr.
    if (gen12_batch_emit_fill(&t, 0x100000ULL, 64, 0, 0, 0, 0, 8) == 0) goto fail;
    if (gen12_batch_emit_fill(&t, 0x100000ULL, 64, 0, 0xFFFFu, 0, 2, 8) == 0) goto fail;
    if (gen12_batch_emit_fill(&t, 0x100000ULL, 0, 0, 0, 0, 8, 8) == 0) goto fail;
    if (gen12_batch_emit_fill(&t, 0x100001ULL, 64, 0, 0, 0, 8, 8) == 0) goto fail;
    if (gen12_batch_emit_fill(&t, 0x800000ULL, 64, 0, 0, 0, 8, 8) == 0) goto fail;

    // Seal: end() terminates, further emits refused, double end refused.
    if (gen12_batch_end(&t) != 0) goto fail;
    if (scratch[t.n - 1] != MI_BB_END) goto fail;
    if (gen12_batch_emit_fill(&t, 0x100000ULL, 64, 0, 0, 0, 8, 8) == 0) goto fail;
    if (gen12_batch_end(&t) == 0) goto fail;

    // Live backed-batch path when GTT is programmed.
    if (intel_gtt_is_active()) {
        gen12_batch_t lb;
        if (gen12_batch_begin(&lb) != 0) goto fail;
        if (gen12_batch_emit_fill(&lb, 0x100000ULL, 64, 1, 0, 0, 4, 4) != 0) {
            gen12_batch_free(&lb); goto fail;
        }
        if (gen12_batch_end(&lb) != 0) { gen12_batch_free(&lb); goto fail; }
        if (gen12_batch_gpu_addr(&lb) == 0) { gen12_batch_free(&lb); goto fail; }
        gen12_batch_free(&lb);
        if (lb.backed) goto fail;
    } else {
        serial_print("[gen12_batch] SKIP live (GTT unavailable)\n");
    }

    serial_print("[gen12_batch] batch: PASS\n");
    return;
fail:
    serial_print("[gen12_batch] batch: FAIL\n");
}
