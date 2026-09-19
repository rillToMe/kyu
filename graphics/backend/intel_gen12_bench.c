// ============================================================
// Gen12 benchmark — AL-12
// (graphics/backend/intel_gen12_bench.c)
//
// min-of-N TSC cycles, same as intel_bench.c. Workloads: STORE-only
// round trip (= submission overhead + fence wait floor), fill/copy/
// blit 64 (op + submit + exec + sync), batch-build only (CPU emit
// cost). GPU damage-upload benches wait for GHAL wiring (AL-14) —
// the CPU damage baselines already exist in intel_bench.c.
// ============================================================

#include "intel_gen12_bench.h"
#include "intel_gen12.h"
#include "intel_gen12_batch.h"
#include "intel_gen12_fence.h"
#include "intel_gen12_submit.h"
#include "intel_gen12_ppgtt.h"
#include "intel_gpu_alloc.h"
#include "intel_gen12_test_util.h"
#include <string.h>

extern void serial_print(const char* s);

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void dec(uint64_t v) {
    char b[24];
    int n = 0;
    if (v == 0) { serial_print("0"); return; }
    char t[24];
    int i = 0;
    while (v > 0 && i < 23) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) b[n++] = t[--i];
    b[n] = 0;
    serial_print(b);
}

static void line(const char* name, uint64_t mn, uint64_t avg, uint32_t n) {
    serial_print("[bench] ");
    serial_print(name);
    serial_print(": min=");
    dec(mn);
    serial_print(" avg=");
    dec(avg);
    serial_print(" cycles (n=");
    dec(n);
    serial_print(", gen12)\n");
}

// One timed submit of an already-staged batch. 0 ok, -1 on any failure.
static int timed_submit(gen12_batch_t* b, gen12_fence_t* f, uint64_t* dt) {
    uint64_t t0 = rdtsc();
    if (gen12_submit_commit(b->cpu, b->n) != 0) return -1;
    if (gen12_fence_wait(f, 0) != 0) return -1;
    *dt = rdtsc() - t0;
    return 0;
}

static int g_bench_done = 0;

void gen12_bench_run(void) {
    if (g_bench_done) return;
    g_bench_done = 1;

    // Probe: one STORE round trip through the validated emit path.
    // Dead engine → SKIP, one timeout max.
    {
        gen12_fence_t pf;
        gen12_batch_t pb;
        if (gen12_fence_alloc(&pf) != 0 ||
            gen12_batch_begin(&pb) != 0 ||
            gen12_fence_emit(&pb, &pf) != 0 ||
            gen12_batch_end(&pb) != 0) {
            serial_print("[bench] gen12_* SKIP (no batch)\n");
            return;
        }
        uint64_t pdt = 0;
        int pok = timed_submit(&pb, &pf, &pdt);
        gen12_batch_free(&pb);
        if (pok != 0) {
            serial_print("[bench] gen12_* SKIP (engine not live)\n");
            return;
        }
    }

    int src = intel_gpu_buffer_create(64, 64);
    int dst = intel_gpu_buffer_create(64, 64);
    if (src < 0 || dst < 0) {
        if (src >= 0) intel_gpu_buffer_destroy(src);
        if (dst >= 0) intel_gpu_buffer_destroy(dst);
        serial_print("[bench] gen12_* SKIP (no buffers)\n");
        return;
    }
    if (intel_gpu_buffer_map_gpu(src) != 0 ||
        intel_gpu_buffer_map_gpu(dst) != 0) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print("[bench] gen12_* SKIP (no map)\n");
        return;
    }
    intel_gpu_buffer_t* sb = intel_gpu_buffer_get(src);
    intel_gpu_buffer_t* db = intel_gpu_buffer_get(dst);
    if (!sb || !db) {
        intel_gpu_buffer_destroy(src);
        intel_gpu_buffer_destroy(dst);
        serial_print("[bench] gen12_* SKIP (no buf)\n");
        return;
    }
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            gen12t_write(sb, x, y, x + y * 64);
    gen12t_flush(sb);
    // NOTE: PPGTT mirror-sync happens inside commit (single owner).

    const uint32_t N = 10;
    // submit_sync: STORE-only round trip (overhead + fence floor).
    {
        uint64_t mn = ~0ULL, sum = 0;
        for (uint32_t it = 0; it < N; it++) {
            gen12_fence_t f;
            gen12_batch_t b;
            if (gen12_fence_alloc(&f) != 0) break;
            if (gen12_batch_begin(&b) != 0) break;
            int ok = gen12_fence_emit(&b, &f);
            if (ok == 0) ok = gen12_batch_end(&b);
            uint64_t dt = 0;
            if (ok == 0) ok = timed_submit(&b, &f, &dt);
            gen12_batch_free(&b);
            if (ok != 0) break;
            if (dt < mn) mn = dt;
            sum += dt;
        }
        line("gen12_submit_sync", mn, sum / N, N);
    }
    // fill / copy / blit 64.
    {
        struct { const char* nm; int kind; } ops[3] = {
            {"gen12_fill_64", 0}, {"gen12_copy_64", 1}, {"gen12_blit_64", 2}
        };
        for (int o = 0; o < 3; o++) {
            uint64_t mn = ~0ULL, sum = 0;
            uint32_t done = 0;
            for (uint32_t it = 0; it < N; it++) {
                gen12_fence_t f;
                gen12_batch_t b;
                if (gen12_fence_alloc(&f) != 0) break;
                if (gen12_batch_begin(&b) != 0) break;
                int ok = -1;
                if (ops[o].kind == 0)
                    ok = gen12_batch_emit_fill(&b, db->gpu_vaddr, db->stride,
                                               0xFF112233u, 0, 0, 64, 64);
                else if (ops[o].kind == 1)
                    ok = gen12_batch_emit_copy(&b, db->gpu_vaddr, db->stride,
                                               sb->gpu_vaddr, sb->stride,
                                               0, 0, 64, 64);
                else
                    ok = gen12_batch_emit_blit(&b, db->gpu_vaddr, db->stride,
                                               sb->gpu_vaddr, sb->stride,
                                               0, 0, 8, 8, 32, 32);
                if (ok == 0) ok = gen12_fence_emit(&b, &f);
                if (ok == 0) ok = gen12_batch_end(&b);
                uint64_t dt = 0;
                if (ok == 0) ok = timed_submit(&b, &f, &dt);
                gen12_batch_free(&b);
                if (ok != 0) break;
                if (dt < mn) mn = dt;
                sum += dt;
                done++;
            }
            if (done == N) line(ops[o].nm, mn, sum / N, N);
            else {
                serial_print("[bench] ");
                serial_print(ops[o].nm);
                serial_print(": TIMEOUT\n");
            }
        }
    }
    // Batch-build only (CPU emit cost, no submit).
    {
        static uint32_t scratch[64];
        gen12_batch_t b;
        b.cpu = scratch; b.phys = 0; b.gpu = 0;
        uint64_t mn = ~0ULL, sum = 0;
        const uint32_t M = 200;
        for (uint32_t it = 0; it < M; it++) {
            b.n = 0; b.sealed = 0; b.backed = 0;
            uint64_t t0 = rdtsc();
            if (gen12_batch_emit_copy(&b, 0x100000ULL, 64, 0x200000ULL, 64,
                                      0, 0, 64, 64) != 0) break;
            if (gen12_batch_emit_fill(&b, 0x100000ULL, 64, 0xFF00FF00u,
                                      0, 0, 64, 64) != 0) break;
            uint64_t dt = rdtsc() - t0;
            if (dt < mn) mn = dt;
            sum += dt;
        }
        line("gen12_cmd_gen", mn, sum / M, M);
    }

    intel_gpu_buffer_destroy(src);
    intel_gpu_buffer_destroy(dst);
    serial_print("[bench] gen12 damage_* pending AL-14 (no GHAL path yet)\n");
}
