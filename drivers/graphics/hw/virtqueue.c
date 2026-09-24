// ============================================================
// Generic split virtqueue (drivers/graphics/hw/virtqueue.c)
//
// Phase 2C model: multi-chain in-flight + deferred kick.
// virtq_submit boleh dipanggil berkali-kali sebelum notify; semua chain
// outstanding dilacak lewat freelist eksplisit (free_next) dan panjang
// chain per head (chain_len). Completion di-reap via virtq_poll() —
// deskriptor tiap chain dikembalikan ke freelist saat chain-nya muncul
// di used ring, tanpa asumsi urutan maupun kontiguitas deskriptor.
// ============================================================

#include "virtqueue.h"
#include "gpu_alloc.h"
#include <stddef.h>

int virtq_init(virtq_t* vq, uint16_t queue_index, uint16_t queue_size,
               volatile virtio_pci_common_cfg_t* common,
               volatile uint16_t* notify_base, uint32_t notify_off_multiplier) {
    if (vq == NULL || common == NULL || queue_size == 0) return -1;

    // Select queue & baca ukuran max dari device.
    common->queue_select = queue_index;
    __asm__ volatile("" ::: "memory");
    uint16_t dev_size = common->queue_size;
    if (dev_size == 0) return -1;            // queue tidak ada
    if (queue_size > dev_size) queue_size = dev_size;
    if (queue_size > VG_VQ_MAX_SIZE) queue_size = VG_VQ_MAX_SIZE;

    // Ukuran ring harus DISEPAKATI, bukan diasumsikan. Kalau guest tidak
    // menulis queue_size, device tetap memakai ukuran bawaannya (QEMU: 64
    // untuk controlq) sedangkan driver memakai angka di atas (32) → rumus
    // slot `avail->ring[idx % N]` / `used->ring[idx % N]` beda modulus dan
    // completion/fence nyantol ke chain yang salah begitu idx melewati N.
    // Tulis nilai final, lalu baca balik: apa pun yang device pegang
    // (menghormati tulis kita, atau mengabaikannya) itulah yang dipakai
    // driver sehingga kedua sisi selalu sinkron.
    common->queue_size = queue_size;
    __asm__ volatile("" ::: "memory");
    uint16_t negotiated = common->queue_size;
    if (negotiated >= 2 && negotiated <= VG_VQ_MAX_SIZE) queue_size = negotiated;

    gpu_page_t desc_pg, avail_pg, used_pg;
    if (gpu_alloc_page(&desc_pg) != 0) return -1;
    if (gpu_alloc_page(&avail_pg) != 0) { gpu_free_pages(&desc_pg, 1); return -1; }
    if (gpu_alloc_page(&used_pg) != 0)  { gpu_free_pages(&desc_pg, 1); gpu_free_pages(&avail_pg, 1); return -1; }

    vq->queue_index  = queue_index;
    vq->queue_size   = queue_size;
    vq->desc         = (virtq_desc_t*)desc_pg.virt;
    vq->avail        = (virtq_avail_t*)avail_pg.virt;
    vq->used         = (virtq_used_t*)used_pg.virt;
    vq->desc_phys    = desc_pg.phys;
    vq->avail_phys   = avail_pg.phys;
    vq->used_phys    = used_pg.phys;
    vq->last_used_idx = 0;

    // Freelist eksplisit: free_next[i] = i+1, ekor = INVALID.
    for (uint16_t i = 0; i < queue_size; i++) {
        vq->desc[i].addr  = 0;
        vq->desc[i].len   = 0;
        vq->desc[i].flags = 0;
        vq->desc[i].next  = VG_VQ_INVALID;
        vq->free_next[i]  = (uint16_t)(i + 1);
        vq->chain_len[i]  = 0;
    }
    vq->free_next[queue_size - 1] = VG_VQ_INVALID;
    vq->free_head = 0;
    vq->num_free  = queue_size;

    vq->avail->flags = 0;
    vq->avail->idx   = 0;
    vq->used->flags  = 0;
    vq->used->idx    = 0;

    // Tulis physical addr ke common_cfg (64-bit write — aligned di x86-64).
    common->queue_desc  = vq->desc_phys;
    common->queue_driver = vq->avail_phys;
    common->queue_device = vq->used_phys;
    __asm__ volatile("" ::: "memory");

    common->queue_enable = 1;
    __asm__ volatile("" ::: "memory");

    uint16_t notify_off = common->queue_notify_off;
    // PCI notify offsets are bytes, not uint16_t register indices.
    vq->notify_addr = (volatile uint16_t*)((volatile uint8_t*)notify_base +
                         (uint32_t)notify_off * notify_off_multiplier);

    return 0;
}

// Submit chain sepanjang n_bufs, memoped n deskriptor dari freelist
// eksplisit dan merangkai lewat desc.next (tidak harus berurutan index).
// Return head index atau -1.
int virtq_submit(virtq_t* vq, uint64_t* addrs, uint32_t* lens,
                 uint16_t* flags, int n_bufs) {
    if (vq == NULL || n_bufs <= 0) return -1;
    if ((uint32_t)n_bufs > vq->num_free) return -1;

    uint16_t head = vq->free_head;
    for (int i = 0; i < n_bufs; i++) {
        uint16_t idx = vq->free_head;
        vq->free_head = vq->free_next[idx];
        vq->num_free--;

        vq->desc[idx].addr  = addrs[i];
        vq->desc[idx].len   = lens[i];
        vq->desc[idx].flags = flags[i];
        if (i < n_bufs - 1) {
            // Deskriptor berikutnya = pop freelist berikutnya (persis yang
            // dipakai iterasi i+1) — chain valid meski index tak berurutan.
            vq->desc[idx].flags |= VIRTQ_DESC_F_NEXT;
            vq->desc[idx].next  = vq->free_head;
        }
    }
    vq->chain_len[head] = (uint8_t)n_bufs;

    // Device may poll avail without a kick. Publish descriptors/backing before
    // idx; DMA uses coherent WB memory on x86, requiring release ordering.
    uint16_t slot = (uint16_t)(vq->avail->idx % vq->queue_size);
    vq->avail->ring[slot] = head;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    ((volatile virtq_avail_t*)vq->avail)->idx++;

    return (int)head;
}

void virtq_notify(virtq_t* vq) {
    if (vq == NULL || vq->notify_addr == NULL) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST); // backing/avail visible before MMIO kick
    *vq->notify_addr = vq->queue_index;
}

// Reap SATU chain selesai dari used ring (non-blocking). Deskriptor chain
// di-walk lewat chain_len[head] + desc.next (device tidak mengubah desc
// table) dan dikembalikan ke freelist.
int virtq_poll(virtq_t* vq, uint32_t* out_head, uint32_t* out_len) {
    if (vq == NULL) return -1;
    uint16_t used_idx = ((volatile virtq_used_t*)vq->used)->idx;
    if (vq->last_used_idx == used_idx) return -1;   // tidak ada completion baru
    __atomic_thread_fence(__ATOMIC_ACQUIRE); // used payload/response follows device idx

    virtq_used_elem_t* ue =
        &vq->used->ring[vq->last_used_idx % vq->queue_size];
    uint32_t head = ue->id;
    if (head >= vq->queue_size || head >= VG_VQ_MAX_SIZE || !vq->chain_len[head]) {
        vq->last_used_idx++;
        return -1;
    }
    if (out_head) *out_head = head;
    if (out_len)  *out_len  = ue->len;
    vq->last_used_idx++;

    uint16_t n = vq->chain_len[head];
    uint16_t idx = (uint16_t)head;
    for (uint16_t i = 0; i < n; i++) {
        // Push kembali ke freelist (urutan bebas — freelist eksplisit).
        vq->free_next[idx] = vq->free_head;
        vq->free_head = idx;
        uint16_t next = vq->desc[idx].next;
        vq->desc[idx].flags = 0;
        idx = next;
    }
    vq->chain_len[head] = 0;
    vq->num_free = (uint16_t)(vq->num_free + n);
    return 0;
}

int virtq_wait(virtq_t* vq, int head, uint32_t* out_written_len) {
    if (vq == NULL || head < 0) return -1;
    uint32_t iter = 0;
    for (;;) {
        uint32_t h, l;
        if (virtq_poll(vq, &h, &l) == 0) {
            if (h == (uint32_t)head) {
                if (out_written_len) *out_written_len = l;
                return 0;
            }
            continue;   // chain lain selesai lebih dulu — sudah di-reclaim
        }
        if (++iter > VIRTQ_POLL_MAX_ITER) return -2;   // -ETIMEDOUT
        __asm__ volatile("pause");
    }
}
