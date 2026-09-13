// ============================================================
// Host-side unit test: generic split virtqueue (test/virtqueue_test.c)
//
// Menguji Phase 2C multi-chain model TANPA device asli (mock MMIO =
// struct biasa di memori — pola roadmap §11 / desktop_manifest_test.c):
//   - init: freelist eksplisit, avail/used di-nolkan, notify terpasang
//   - submit: chain benar (addr/len/flags/next), avail ring terisi
//   - deferred kick: dua chain outstanding sebelum notify
//   - completion OUT-OF-ORDER: poll chain kedua dulu — freelist chain
//     pertama TIDAK boleh kembali sebelum chain-nya sendiri selesai
//   - reuse deskriptor terdaur: chain baru integritas
//   - exhaustion: submit saat num_free kurang → -1
//   - wait: sukses segera bila completion sudah ada
//
// Jalankan: make test-virtqueue
// ============================================================

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "virtqueue.h"
#include "gpu_alloc.h"

// --- mock gpu_alloc (dipakai virtq_init) ---
static uint8_t  g_mock_pages[8][4096] __attribute__((aligned(4096)));
static uint64_t g_mock_phys_base = 0x1000000;   // palsu, hanya dicatat
static int      g_mock_page_used = 0;

int gpu_alloc_page(gpu_page_t* out) {
    if (g_mock_page_used >= 8) return -1;
    out->virt = g_mock_pages[g_mock_page_used];
    out->phys = g_mock_phys_base + (uint64_t)g_mock_page_used * 4096;
    g_mock_page_used++;
    return 0;
}

void gpu_free_pages(gpu_page_t* pages, uint32_t count) {
    (void)pages; (void)count;   // mock: halaman statis, tidak perlu daur
}

uint32_t gpu_alloc_pages(uint32_t count, gpu_page_t* pages) {
    uint32_t n = 0;
    for (; n < count; n++)
        if (gpu_alloc_page(&pages[n]) != 0) break;
    return n;
}

// --- mock MMIO: common_cfg + notify register ---
static virtio_pci_common_cfg_t g_common;
static uint16_t g_notify_reg[4];

// Simulasi device: tandai satu chain selesai di used ring (urutan bebas).
static void device_complete(virtq_t* vq, uint32_t head, uint32_t len) {
    uint16_t slot = (uint16_t)(vq->used->idx % vq->queue_size);
    vq->used->ring[slot].id  = head;
    vq->used->ring[slot].len = len;
    __asm__ volatile("" ::: "memory");
    vq->used->idx++;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   - %s\n", msg); } \
    else      { printf("  FAIL - %s\n", msg); g_fail++; } \
} while (0)

static uint16_t bufs_flags(const virtq_t* vq, uint16_t head, int i) {
    uint16_t idx = head;
    for (int k = 0; k < i; k++) idx = vq->desc[idx].next;
    return idx;
}

int main(void) {
    memset(&g_common, 0, sizeof(g_common));
    memset(g_notify_reg, 0xAA, sizeof(g_notify_reg));
    g_common.queue_size = 8;        // tawaran device
    g_common.queue_notify_off = 0;

    printf("virtqueue test (multi-chain Phase 2C)\n");

    virtq_t vq;
    int r = virtq_init(&vq, 0 /*controlq*/, 8, &g_common, g_notify_reg, sizeof(uint16_t));
    CHECK(r == 0, "init sukses");
    CHECK(vq.queue_size == 8, "queue_size = tawaran device");
    CHECK(vq.num_free == 8 && vq.free_head == 0, "freelist penuh, head=0");
    CHECK(vq.avail->idx == 0 && vq.used->idx == 0, "avail/used di-nolkan");
    CHECK(g_notify_reg[0] == 0xAAAA, "notify belum tertulis sebelum submit");

    // --- dua chain outstanding (deferred kick §9.1) ---
    uint64_t a1[2] = {0xAAAA0000, 0xBBBB0000};
    uint32_t l1[2] = {32, 64};
    uint16_t f1[2] = {0, VIRTQ_DESC_F_WRITE};
    int head1 = virtq_submit(&vq, a1, l1, f1, 2);
    CHECK(head1 == 0, "submit#1 head=0");

    uint64_t a2[2] = {0xCCCC0000, 0xDDDD0000};
    int head2 = virtq_submit(&vq, a2, l1, f1, 2);
    CHECK(head2 == 2, "submit#2 head=2 (chain kedua, belum notify)");
    CHECK(vq.num_free == 4, "num_free=4 setelah 2 chain x 2 desc");
    CHECK(vq.avail->idx == 2 && vq.avail->ring[0] == 0 && vq.avail->ring[1] == 2,
          "avail ring berisi head 0,2");

    // Integritas chain#1: desc0 -> desc1, flag F_NEXT + WRITE benar.
    CHECK(vq.desc[0].next == 1 && (vq.desc[0].flags & VIRTQ_DESC_F_NEXT),
          "chain#1 desc0.next=1 + F_NEXT");
    CHECK(vq.desc[1].addr == 0xBBBB0000 && vq.desc[1].flags == VIRTQ_DESC_F_WRITE,
          "chain#1 desc1 = resp buffer (F_WRITE)");

    // SATU notify untuk dua chain.
    virtq_notify(&vq);
    CHECK(g_notify_reg[0] == 0, "notify menulis queue_index 0");

    // --- completion OUT-OF-ORDER: chain#2 selesai dulu ---
    device_complete(&vq, (uint32_t)head2, 128);
    uint32_t ph = 0xFFFF, pl = 0;
    r = virtq_poll(&vq, &ph, &pl);
    CHECK(r == 0 && ph == (uint32_t)head2 && pl == 128, "poll#1 → chain#2 (out-of-order)");
    CHECK(vq.num_free == 6, "desc chain#2 kembali (num_free=6), chain#1 BELUM");
    r = virtq_poll(&vq, &ph, &pl);
    CHECK(r == -1, "poll#2 kosong — chain#1 masih in-flight");

    // --- chain#1 selesai; daur ulang penuh ---
    device_complete(&vq, (uint32_t)head1, 96);
    r = virtq_poll(&vq, &ph, &pl);
    CHECK(r == 0 && ph == (uint32_t)head1 && pl == 96, "poll#3 → chain#1");
    CHECK(vq.num_free == 8, "freelist penuh kembali (num_free=8)");

    // --- reuse deskriptor terdaur: chain baru harus tetap integritas ---
    uint64_t a3[2] = {0x11110000, 0x22220000};
    int head3 = virtq_submit(&vq, a3, l1, f1, 2);
    (void)head3;
    uint16_t d0 = bufs_flags(&vq, (uint16_t)head3, 0);
    uint16_t d1 = bufs_flags(&vq, (uint16_t)head3, 1);
    CHECK(vq.desc[d0].addr == 0x11110000 && vq.desc[d1].addr == 0x22220000 &&
          (vq.desc[d0].flags & VIRTQ_DESC_F_NEXT) && vq.desc[d0].next == d1,
          "chain terdaur: addr/next/flags benar");

    // --- exhaustion: sisa 6 desc, minta 8 → tolak ---
    uint64_t a8[8] = {0};
    uint32_t l8[8] = {8,8,8,8,8,8,8,8};
    uint16_t f8[8] = {0};
    int head4 = virtq_submit(&vq, a8, l8, f8, 8);
    CHECK(head4 == -1, "submit 8 desc saat sisa 6 → ditolak");

    // --- wait: completion sudah ada → sukses segera ---
    device_complete(&vq, (uint32_t)head3, 7);
    uint32_t wl = 0;
    r = virtq_wait(&vq, head3, &wl);
    CHECK(r == 0 && wl == 7, "wait segera setelah completion tersedia");

    if (g_fail == 0) { printf("ALL PASS\n"); return 0; }
    printf("%d FAILURE(S)\n", g_fail);
    return 1;
}
