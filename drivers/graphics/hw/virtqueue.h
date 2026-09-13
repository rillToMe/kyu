#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

// ============================================================
// Generic split virtqueue (drivers/graphics/hw/virtqueue.h)
//
// REUSABLE untuk driver VirtIO lain (net, blk) — tidak tahu apa pun
// tentang virtio-gpu. Mengelola descriptor/avail/used ring, submit,
// notify, dan busy-poll used ring dengan timeout.
//
// Phase 2C (roadmap §9.1): dukungan MULTI-CHAIN in-flight. Beberapa
// chain boleh outstanding sekaligus (submit → submit → notify sekali),
// completion di-reap lewat virtq_poll(). Freelist descriptor eksplisit
// (free_next) sehingga reclaim antar chain tidak saling menginjak.
//
// PRECONDITION (roadmap §2A.4): TIDAK ada lock di virtqueue.
// Akses virtq_submit/virtq_poll/virtq_wait diasumsikan single-context.
// Caller yang memakai ulang untuk device dengan akses konkuren WAJIB
// menambahkan lock sendiri — jangan berasumsi virtqueue thread-safe.
// ============================================================

#include <stdint.h>
#include "virtio_gpu_regs.h"   // virtio_pci_common_cfg_t, virtq_desc_t dll.

#define VIRTQ_POLL_MAX_ITER 10000000   // batas busy-poll (roadmap §6.9)

// Batas ukuran queue yang didukung struktur freelist di bawah. Device
// boleh menawarkan lebih besar (spec sampai 32768) — virtq_init clamp.
#define VG_VQ_MAX_SIZE  256
#define VG_VQ_INVALID   0xFFFF

typedef struct {
    uint16_t         queue_index;      // 0 = controlq, 1 = cursorq
    uint16_t         queue_size;
    virtq_desc_t*    desc;             // virtual addr
    virtq_avail_t*   avail;
    virtq_used_t*    used;
    uint64_t         desc_phys;        // physical addr (untuk common_cfg)
    uint64_t         avail_phys;
    uint64_t         used_phys;
    uint16_t         free_head;        // freelist eksplisit: head daftar kosong
    uint16_t         num_free;
    uint16_t         last_used_idx;    // posisi reap used ring (polling)
    volatile uint16_t* notify_addr;    // MMIO notify register

    // Freelist eksplisit + panjang chain per head — syarat multi-chain:
    // reclaim satu chain tidak boleh mengasumsikan deskriptor berurutan.
    uint16_t         free_next[VG_VQ_MAX_SIZE];   // free_next[i] = desc kosong berikutnya
    uint8_t          chain_len[VG_VQ_MAX_SIZE];   // chain_len[head] = jumlah desc chain
} virtq_t;

// Setup virtqueue `queue_index` pada `common`, alokasi ring, tulis physical
// addr ke common_cfg, enable queue. notify_base = MMIO NOTIFY_CFG base.
// Returns 0 sukses, <0 error.
int virtq_init(virtq_t* vq, uint16_t queue_index, uint16_t queue_size,
               volatile virtio_pci_common_cfg_t* common,
               volatile uint16_t* notify_base, uint32_t notify_off_multiplier);

// Submit satu chain descriptor (n_bufs buffer). Setiap buffer punya
// addr/len/flags. Boleh dipanggil berkali-kali sebelum notify (deferred
// kick, §9.1). Return descriptor head index, atau -1 kalau penuh.
int virtq_submit(virtq_t* vq, uint64_t* addrs, uint32_t* lens,
                 uint16_t* flags, int n_bufs);

void virtq_notify(virtq_t* vq);

// NON-BLOCKING: reap SATU chain yang sudah selesai dari used ring (kalau
// ada), kembalikan deskriptornya ke freelist. Return 0 dan isi out_head/
// out_len bila ada chain selesai; -1 bila used ring kosong.
int virtq_poll(virtq_t* vq, uint32_t* out_head, uint32_t* out_len);

// Busy-poll sampai chain `head` selesai (virtq_poll berulang, chain lain
// yang selesai lebih dulu ikut di-reclaim), ATAU timeout.
// Return 0 sukses, -ETIMEDOUT. out_written_len opsional.
int virtq_wait(virtq_t* vq, int head, uint32_t* out_written_len);

#endif // VIRTQUEUE_H
