// ============================================================
// GPU benchmark — Phase 20
// (graphics/backend/intel_bench.c)
//
// Min-of-N TSC cycles per op (min cuts IRQ noise). All loops
// are µs-scale; total boot cost is a few ms.
// ============================================================

#include "intel_bench.h"
#include "intel_bcs.h"
#include "intel_cmd.h"
#include "intel_fence.h"
#include "intel_gpu_alloc.h"
#include "intel_surface.h"
#include "heap.h"
#include <string.h>

extern void serial_print(const char* s);

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void dec(uint64_t v) {
    char b[24]; int n = 0;
    if (v == 0) { serial_print("0"); return; }
    char t[24]; int i = 0;
    while (v > 0 && i < 23) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) b[n++] = t[--i];
    b[n] = 0;
    serial_print(b);
}

// [bench] <name>: min=<m> avg=<a> cycles (n=<n>, <path>)
static void line(const char* name, uint64_t mn, uint64_t avg, uint32_t n,
                 const char* path) {
    serial_print("[bench] ");
    serial_print(name);
    serial_print(": min=");
    dec(mn);
    serial_print(" avg=");
    dec(avg);
    serial_print(" cycles (n=");
    dec(n);
    serial_print(", ");
    serial_print(path);
    serial_print(")\n");
}

// --- CPU fill/copy on RAM (software-compositor baseline) ---

static void bench_cpu_fill(uint32_t dim, uint32_t iters) {
    uint64_t bytes = (uint64_t)dim * dim * 4;
    uint32_t* p = (uint32_t*)kmalloc((size_t)bytes);
    if (!p) return;
    uint64_t mn = ~0ULL, sum = 0;
    for (uint32_t it = 0; it < iters; it++) {
        uint64_t t0 = rdtsc();
        for (uint32_t y = 0; y < dim; y++) {
            uint32_t* row = p + (uint64_t)y * dim;
            for (uint32_t x = 0; x < dim; x++) row[x] = 0xFF112233;
        }
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    line(dim == 64 ? "cpu_fill_64" : "cpu_fill_256", mn, sum / iters, iters, "cpu");
    kfree(p);
}

static void bench_cpu_copy(uint32_t dim, uint32_t iters) {
    uint64_t bytes = (uint64_t)dim * dim * 4;
    uint32_t* d = (uint32_t*)kmalloc((size_t)bytes);
    uint32_t* s = (uint32_t*)kmalloc((size_t)bytes);
    if (!d || !s) { if (d) kfree(d); if (s) kfree(s); return; }
    memset(s, 0x5A, (size_t)bytes);
    uint64_t mn = ~0ULL, sum = 0;
    for (uint32_t it = 0; it < iters; it++) {
        uint64_t t0 = rdtsc();
        for (uint32_t y = 0; y < dim; y++)
            memcpy(d + (uint64_t)y * dim, s + (uint64_t)y * dim, dim * 4);
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    // Prevent DCE on the copy destination.
    volatile uint32_t sink = d[(bytes / 4) - 1];
    (void)sink;
    line(dim == 64 ? "cpu_copy_64" : "cpu_copy_256", mn, sum / iters, iters, "cpu");
    kfree(d);
    kfree(s);
}

// --- Phase 21: per-pixel call overhead vs direct loop ---
// Mimics the surf_write accessor cost on linear RAM.

static uint32_t* g_px_base;
static uint32_t  g_px_stride;

static void __attribute__((noinline)) px_write(uint32_t x, uint32_t y, uint32_t v) {
    g_px_base[(uint64_t)y * g_px_stride + x] = v;
}

static void bench_cpu_fill_call(void) {
    const uint32_t dim = 64, iters = 50;
    uint32_t* p = (uint32_t*)kmalloc(dim * dim * 4);
    if (!p) return;
    g_px_base = p;
    g_px_stride = dim;
    uint64_t mn = ~0ULL, sum = 0;
    for (uint32_t it = 0; it < iters; it++) {
        uint64_t t0 = rdtsc();
        for (uint32_t y = 0; y < dim; y++)
            for (uint32_t x = 0; x < dim; x++)
                px_write(x, y, 0xFF112233);
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    line("cpu_fill_call_64", mn, sum / iters, iters, "cpu");
    kfree(p);
}

// --- Command generation (pure CPU, no HW) ---

static void bench_cmd_gen(void) {
    intel_cmd_t c;
    uint64_t mn = ~0ULL, sum = 0;
    const uint32_t n = 200;
    for (uint32_t it = 0; it < n; it++) {
        uint64_t t0 = rdtsc();
        intel_cmd_begin(&c);
        intel_cmd_emit_copy(&c, 0x100000ULL, 64, 0x200000ULL, 64, 0, 0, 64, 64);
        intel_cmd_emit_fill(&c, 0x100000ULL, 64, 0xFF00FF00, 0, 0, 64, 64);
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    line("cmd_gen_copy+fill", mn, sum / n, n, "cpu");
}

// --- Damage-upload pattern: N dirty 64x64 rects RAM->RAM ---

static void bench_damage(uint32_t nrects) {
    // Grid muat semua rect: <=16 -> 4 kolom (256x256), 32 -> 8 kolom (512x256).
    uint32_t cols = nrects <= 16 ? 4 : 8;
    uint32_t dst_w = cols * 64;
    uint32_t* src = (uint32_t*)kmalloc(64 * 64 * 4);
    uint32_t* dst = (uint32_t*)kmalloc((size_t)dst_w * 256 * 4);
    if (!src || !dst) { if (src) kfree(src); if (dst) kfree(dst); return; }
    memset(src, 0xA5, 64 * 64 * 4);
    const uint32_t iters = 10;
    uint64_t mn = ~0ULL, sum = 0;
    for (uint32_t it = 0; it < iters; it++) {
        uint64_t t0 = rdtsc();
        for (uint32_t r = 0; r < nrects; r++) {
            uint32_t dx = (r % cols) * 64, dy = (r / cols) * 64;
            for (uint32_t y = 0; y < 64; y++)
                memcpy(dst + (uint64_t)(dy + y) * dst_w + dx,
                       src + (uint64_t)y * 64, 64 * 4);
        }
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    volatile uint32_t sink = dst[dst_w * 256 - 1];
    (void)sink;
    line(nrects == 1 ? "damage_1rect" : nrects == 4 ? "damage_4rect" :
         nrects == 16 ? "damage_16rect" : "damage_32rect",
         mn, sum / iters, iters, "cpu");
    kfree(src);
    kfree(dst);
}

// --- HW submit+exec+sync round-trip (BCS live only) ---

static void bench_hw_submit(void) {
    const uint32_t n = 20;
    uint64_t mn = ~0ULL, sum = 0;
    for (uint32_t it = 0; it < n; it++) {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        intel_cmd_emit_noop(&c);
        intel_fence_t f;
        uint64_t t0 = rdtsc();
        if (intel_gpu_submit(&c, &f) != 0) return;
        if (intel_fence_wait_sleep(&f, 0) != 0) {
            serial_print("[bench] hw_submit_sync: TIMEOUT\n");
            return;
        }
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    line("hw_submit_sync", mn, sum / n, n, "bcs");
}

static void bench_hw_fill(void) {
    intel_gsurf_t s;
    if (intel_gsurf_create(&s, 64, 64) != 0) return;
    const uint32_t n = 20;
    uint64_t mn = ~0ULL, sum = 0;
    // Resolve backing buffer once (stable across fills).
    intel_gpu_buffer_t* b = intel_gpu_buffer_get(s.buf_id);
    if (!b) { intel_gsurf_destroy(&s); return; }
    for (uint32_t it = 0; it < n; it++) {
        intel_cmd_t c;
        intel_cmd_begin(&c);
        intel_cmd_emit_fill(&c, b->gpu_vaddr, b->stride, 0xFF112233, 0, 0, 64, 64);
        intel_fence_t f;
        uint64_t t0 = rdtsc();
        if (intel_gpu_submit(&c, &f) != 0) break;
        if (intel_fence_wait_sleep(&f, 0) != 0) {
            serial_print("[bench] hw_fill_64: TIMEOUT\n");
            break;
        }
        uint64_t dt = rdtsc() - t0;
        if (dt < mn) mn = dt;
        sum += dt;
    }
    line("hw_fill_64", mn, sum / n, n, "bcs");
    intel_gsurf_destroy(&s);
}

void intel_bench_run(void) {
    serial_print("[bench] --- GPU benchmark (TSC cycles, this boot only) ---\n");
    bench_cpu_fill(64, 50);
    bench_cpu_fill_call();
    bench_cpu_fill(256, 20);
    bench_cpu_copy(64, 50);
    bench_cpu_copy(256, 20);
    bench_cmd_gen();
    bench_damage(1);
    bench_damage(4);
    bench_damage(16);
    bench_damage(32);
    if (intel_bcs_is_available()) {
        bench_hw_submit();
        bench_hw_fill();
    } else {
        serial_print("[bench] hw_* skipped (no BCS)\n");
    }
    serial_print("[bench] --- end ---\n");
}
