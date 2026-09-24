// Real VirtIO backend/transport. A RAM device publishes completions explicitly,
// including out-of-order batches and queue-lock contention. No hardware I/O.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "damage_test_lock.h"
#include "virtqueue.h"
#include "gpu_alloc.h"
#include "display.h"
#undef VIRTQ_POLL_MAX_ITER
#define VIRTQ_POLL_MAX_ITER 1000 // fault injection must not burn a hardware timeout

uint64_t hhdm_offset;
uint32_t pci_read_word(uint8_t b, uint8_t s, uint8_t f, uint8_t o) {
    (void)b; (void)s; (void)f; (void)o; return UINT32_MAX;
}
void serial_print(const char* s) { (void)s; }
void serial_dec(uint64_t n) { (void)n; }
uint64_t timer_get_ticks(void) { return 0; }
const display_mode_t* display_get_mode(void) { return NULL; }
void* kmalloc(size_t n) { return calloc(1, n); }
void kfree(void* p) { free(p); }
int gpu_alloc_page(gpu_page_t* p) {
    p->virt = calloc(1, 4096); p->phys = (uintptr_t)p->virt;
    return p->virt ? 0 : -1;
}
void gpu_free_pages(gpu_page_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { free(p[i].virt); p[i].virt = NULL; p[i].phys = 0; }
}
int gpu_alloc_pages_contiguous(uint32_t n, gpu_page_t* p) { (void)n; (void)p; return -1; }

#include "../../../drivers/graphics/hw/virtqueue.c"
#include "../../../drivers/graphics/hw/virtio_gpu_dev.c"
#include "../../../drivers/graphics/hw/virtio_gpu_cmd.c"
#include "../../../graphics/backend/virtio_gpu.c"

static unsigned failures;
static void check(int ok, const char* label) {
    if (!ok) { failures++; printf("FAIL %s\n", label); }
}
typedef struct { uint64_t fence; uint32_t head[2]; } batch_t;
static batch_t submit(void) {
    virtio_gpu_transfer_to_host_2d_t t;
    virtio_gpu_resource_flush_t f;
    virtio_gpu_cmd_transfer_to_host(&t, 1, 5, 7, 3, 4, 64 * 4);
    virtio_gpu_cmd_resource_flush(&f, 1, 5, 7, 3, 4);
    uint16_t a = g_vgpu.controlq.avail->idx;
    batch_t b;
    b.fence = virtio_gpu_dev_submit2(&t, sizeof(t), &f, sizeof(f));
    assert(b.fence);
    b.head[0] = g_vgpu.controlq.avail->ring[a % g_vgpu.controlq.queue_size];
    b.head[1] = g_vgpu.controlq.avail->ring[(a + 1) % g_vgpu.controlq.queue_size];
    return b;
}
static void device_complete(uint32_t head, uint32_t type) {
    virtq_t* q = &g_vgpu.controlq;
    const virtio_gpu_ctrl_hdr_t* cmd = (void*)(uintptr_t)q->desc[head].addr;
    virtio_gpu_ctrl_hdr_t* resp = (void*)(uintptr_t)q->desc[q->desc[head].next].addr;
    *resp = *cmd; resp->type = type;
    uint16_t idx = q->used->idx;
    q->used->ring[idx % q->queue_size] = (virtq_used_elem_t){head, sizeof(*resp)};
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ((volatile virtq_used_t*)q->used)->idx = (uint16_t)(idx + 1);
}
static void finish(batch_t b) {
    device_complete(b.head[0], VIRTIO_GPU_RESP_OK_NODATA);
    device_complete(b.head[1], VIRTIO_GPU_RESP_OK_NODATA);
    virtio_gpu_dev_poll_fences();
}
static void fence_checks(void) {
    batch_t a = submit();
    device_complete(a.head[0], VIRTIO_GPU_RESP_OK_NODATA);
    check(!virtio_gpu_dev_fence_done(a.fence), "TRANSFER completion alone is not present completion");
    device_complete(a.head[1], VIRTIO_GPU_RESP_OK_NODATA);
    check(virtio_gpu_dev_fence_done(a.fence), "both commands complete fence");
    a = submit();
    device_complete(a.head[1], VIRTIO_GPU_RESP_OK_NODATA);
    check(!virtio_gpu_dev_fence_done(a.fence), "out-of-order FLUSH does not release TRANSFER backing");
    device_complete(a.head[0], VIRTIO_GPU_RESP_OK_NODATA);
    check(virtio_gpu_dev_fence_done(a.fence), "reverse completion retires pair");
    a = submit(); batch_t b = submit();
    finish(b);
    check(!virtio_gpu_dev_fence_done(b.fence), "latest fence also covers older in-flight transfers");
    finish(a);
    check(virtio_gpu_dev_fence_done(b.fence), "watermark advances after older pair retires");
    a = submit();
    device_complete(a.head[0], VIRTIO_GPU_RESP_OK_NODATA);
    uint16_t used = g_vgpu.controlq.last_used_idx;
    spinlock_lock(&g_vgpu.cmd_lock);
    (void)virtio_gpu_dev_fence_done(a.fence);
    check(g_vgpu.controlq.last_used_idx == used, "poll cannot reap under another queue owner");
    virtio_gpu_ctrl_hdr_t command = {0};
    check(virtio_gpu_dev_submit2(&command, sizeof(command), &command, sizeof(command)) == 0,
          "busy queue rejects submission without spinning");
    spinlock_unlock(&g_vgpu.cmd_lock);
    device_complete(a.head[1], VIRTIO_GPU_RESP_OK_NODATA);
    virtio_gpu_dev_poll_fences();
    a = submit();
    uint64_t err = g_vgpu.stats.err_count;
    device_complete(a.head[0], VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER);
    device_complete(a.head[1], VIRTIO_GPU_RESP_OK_NODATA);
    check(virtio_gpu_dev_fence_done(a.fence), "error completion still releases DMA ownership");
    check(g_vgpu.stats.err_count == err + 1, "device error reaches compositor retry statistics");

    // A long-lived pair must keep its response slots across arbitrarily many
    // newer completions; round-robin slots eventually alias the live pair.
    a = submit();
    uint64_t ra = g_vgpu.controlq.desc[g_vgpu.controlq.desc[a.head[0]].next].addr;
    int distinct = 1;
    for (int i = 0; i < 80; i++) {
        b = submit();
        for (int j = 0; j < 2; j++)
            if (g_vgpu.controlq.desc[g_vgpu.controlq.desc[b.head[j]].next].addr == ra) distinct = 0;
        finish(b);
    }
    check(distinct, "response slots stay owned until both pair completions");
    finish(a);
    check(g_vgpu.controlq.num_free == g_vgpu.controlq.queue_size, "all descriptors returned");
}

static void backend_checks(void) {
    uint32_t backing[64 * 48], source[80 * 48], host[64 * 48];
    for (unsigned i = 0; i < 80 * 48; i++) source[i] = 0xFF000000 | i;
    for (unsigned i = 0; i < 64 * 48; i++) backing[i] = host[i] = 0x123456;
    struct ghal_surface s = { .resource_id = 1, .width = 64, .height = 48,
        .backing_virt = backing, .scanout_set = 1 };
    ghal_rect_t r = {5, 7, 3, 4};
    virtio_surface_upload(&s, source, 80, r);
    uint16_t start = g_vgpu.controlq.avail->idx;
    uint64_t waits = g_vgpu.stats.wait_calls;
    virtio_present(&s, &r);
    uint32_t head = g_vgpu.controlq.avail->ring[start % g_vgpu.controlq.queue_size];
    const virtio_gpu_transfer_to_host_2d_t* t = (void*)(uintptr_t)g_vgpu.controlq.desc[head].addr;
    check(t->offset == (7 * 64 + 5) * 4 && t->r.x == 5 && t->r.y == 7 &&
          t->r.width == 3 && t->r.height == 4, "partial transfer offset and rectangle");
    for (unsigned y = 0; y < t->r.height; y++)
        memcpy(host + (t->r.y + y) * 64 + t->r.x,
               (uint8_t*)backing + t->offset + y * 64 * 4, t->r.width * 4);
    int exact = 1;
    for (unsigned y = 0; y < 48; y++) for (unsigned x = 0; x < 64; x++) {
        uint32_t want = x >= 5 && x < 8 && y >= 7 && y < 11 ? source[y * 80 + x] : 0x123456;
        if (host[y * 64 + x] != want) exact = 0;
    }
    check(exact, "padded CPU stride to linear backing to host pixels");
    r = (ghal_rect_t){30, 20, 1, 1};
    virtio_present(&s, &r);
    check(g_vgpu.stats.wait_calls == waits, "multiple regions submit without per-rect fence waits");
    uint16_t end = g_vgpu.controlq.avail->idx;
    for (uint16_t i = start; i != end; i++)
        device_complete(g_vgpu.controlq.avail->ring[i % g_vgpu.controlq.queue_size], VIRTIO_GPU_RESP_OK_NODATA);
    virtio_gpu_dev_poll_fences();

    uint64_t before = g_vgpu.fence_counter;
    r = (ghal_rect_t){5, 7, 0, 4}; virtio_present(&s, &r);
    check(g_vgpu.fence_counter == before, "zero area never reaches device");
    start = g_vgpu.controlq.avail->idx;
    r = (ghal_rect_t){60, 45, UINT32_MAX, UINT32_MAX}; virtio_present(&s, &r);
    if (g_vgpu.controlq.avail->idx != start) {
        head = g_vgpu.controlq.avail->ring[start % g_vgpu.controlq.queue_size];
        t = (void*)(uintptr_t)g_vgpu.controlq.desc[head].addr;
        check(t->r.width == 4 && t->r.height == 3, "present clipped without unsigned overflow");
    } else check(0, "clipped edge present submitted");

    uint32_t src[128], dst[64];
    for (int i = 0; i < 128; i++) src[i] = (uint32_t)i;
    for (int i = 0; i < 64; i++) dst[i] = 0xCAFE;
    struct ghal_surface a = {.width = 2, .height = 2, .backing_virt = src};
    struct ghal_surface b = {.width = 8, .height = 8, .backing_virt = dst};
    virtio_blit(&b, (ghal_rect_t){0, 0, 8, 8}, &a, (ghal_rect_t){1, 1, 8, 8});
    exact = dst[0] == 3;
    for (int i = 1; i < 64; i++) if (dst[i] != 0xCAFE) exact = 0;
    check(exact, "blit clips to source as well as destination");
    for (uint32_t i = 0; i < VGPU_FENCE_MAX_HEADS; i++)
        if (g_vgpu.fence_of_head[i]) device_complete(i, VIRTIO_GPU_RESP_OK_NODATA);
    virtio_gpu_dev_poll_fences();
}

#ifdef _WIN32
static DWORD WINAPI submit_worker(void* unused) {
    (void)unused;
    virtio_gpu_transfer_to_host_2d_t t;
    virtio_gpu_resource_flush_t f;
    virtio_gpu_cmd_transfer_to_host(&t, 1, 1, 1, 1, 1, 64 * 4);
    virtio_gpu_cmd_resource_flush(&f, 1, 1, 1, 1, 1);
    for (int i = 0; i < 1000; i++)
        while (!virtio_gpu_dev_submit2(&t, sizeof(t), &f, sizeof(f))) SwitchToThread();
    return 0;
}
static void concurrent_queue_checks(void) {
    uint16_t next = g_vgpu.controlq.avail->idx;
    uint64_t before = g_vgpu.fence_counter, err = g_vgpu.stats.err_count;
    HANDLE workers[2] = {CreateThread(NULL, 0, submit_worker, NULL, 0, NULL),
                         CreateThread(NULL, 0, submit_worker, NULL, 0, NULL)};
    assert(workers[0] && workers[1]);
    uint64_t deadline = GetTickCount64() + 10000;
    for (;;) {
        uint16_t end = ((volatile virtq_avail_t*)g_vgpu.controlq.avail)->idx;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        while (next != end) {
            uint32_t head = g_vgpu.controlq.avail->ring[next % g_vgpu.controlq.queue_size];
            device_complete(head, VIRTIO_GPU_RESP_OK_NODATA);
            next++;
        }
        if (WaitForMultipleObjects(2, workers, TRUE, 0) == WAIT_OBJECT_0 &&
            next == ((volatile virtq_avail_t*)g_vgpu.controlq.avail)->idx) break;
        assert(GetTickCount64() < deadline);
        SwitchToThread();
    }
    for (int i = 0; i < 2; i++) CloseHandle(workers[i]);
    check(virtio_gpu_dev_fence_done(g_vgpu.fence_counter), "SMP queue stress retires final fence");
    check(g_vgpu.fence_counter == before + 2000 && g_vgpu.stats.err_count == err &&
          g_vgpu.controlq.num_free == g_vgpu.controlq.queue_size,
          "2000 concurrent batches preserve descriptors and responses");
}
#endif
int main(void) {
    static virtio_pci_common_cfg_t common;
    static volatile uint16_t notify;
    common.queue_size = 32;
    assert(virtq_init(&g_vgpu.controlq, 0, 32, &common, &notify, 0) == 0);
    g_vgpu.initialized = 1;
    assert(gpu_alloc_page(&g_vgpu.resp_page) == 0);
    for (int i = 0; i < VGPU_ASYNC_PAIRS; i++) for (int j = 0; j < 2; j++)
        assert(gpu_alloc_page(&g_vgpu.async_cmd[i][j]) == 0);
    g_vgpu.async_ready = 1;
    fence_checks();
    backend_checks();
#ifdef _WIN32
    concurrent_queue_checks();
#endif
    printf("virtio damage transport failures=%u\n", failures);
    return failures ? 1 : 0;
}
