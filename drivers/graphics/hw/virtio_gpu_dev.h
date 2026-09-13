#ifndef VIRTIO_GPU_DEV_H
#define VIRTIO_GPU_DEV_H

// ============================================================
// VirtIO-GPU Driver Core (drivers/graphics/hw/virtio_gpu_dev.h)
//
// Satu-satunya tempat yang boleh menyentuh MMIO/PCI VirtIO-GPU.
// Tugas: PCI bring-up, capability parsing, MMIO map, device status FSM,
// feature negotiation, virtqueue setup, dan eksekusi command ke device.
// TIDAK tahu konsep "surface"/"compositor" — hanya transport + command.
// ============================================================

#include <stdint.h>
#include "virtio_gpu_regs.h"
#include "virtqueue.h"
#include "spinlock.h"
#include "gpu_alloc.h"

// Buffer command/response pre-alokasi (dipakai berulang).
// PENTING: virtio_gpu_dev_command dipanggil dari compositor_flush yang berjalan
// di TIMER IRQ. Alokasi halaman per-command (pmm_alloc_page) di IRQ tidak aman
// dan lambat; karena itu buffer dialokasikan SEKALI saat probe dan dipakai ulang.
#define VGPU_CMD_MAX_PAGES 16

// Layout resp_page (satu halaman 4096):
//   offset 0 .. VGPU_RESP_SYNC_MAX  → response command SINKRON
//     (dev_command; GET_DISPLAY_INFO butuh 408 B, sisanya <= 64 B)
//   offset VGPU_RESP_SYNC_MAX + i*64 → response slot ASYNC per chain
//     (submit2; satu slot per chain in-flight, dialokasikan round-robin)
#define VGPU_RESP_SYNC_MAX  512
#define VGPU_RESP_SLOT      64
#define VGPU_RESP_N_ASYNC   ((4096 - VGPU_RESP_SYNC_MAX) / VGPU_RESP_SLOT)   // 56

// Batas head yang dilacak tabel fence driver core. Harus >= queue_size yang
// diminta setup_queues; diverifikasi saat probe.
#define VGPU_FENCE_MAX_HEADS 64

// --- Phase 2C §9.6 — statistik per-frame GPU (driver core) ---
// Semua counter kumulatif sejak boot; konsumen (syscall/shell/settings)
// mengambil dua sampel dan menghitung delta per-frame sendiri.
// wait_ticks = timer ticks PIT (resolusi ~20ms @50Hz) — ukurannya
// kualitatif: >0 berarti ada blocking nyata, ~0 = present benar-benar async.
typedef struct {
    uint64_t present_count;   // diisi backend (ghal_present virtio)
    uint64_t cmd_count;       // command device terkirim (controlq+cursorq)
    uint64_t cmd_bytes;       // total byte payload command
    uint64_t notify_count;    // virtq_notify (kick) — metrik batching §9.1
    uint64_t wait_calls;      // panggilan blocking-wait (sync + fence_wait)
    uint64_t wait_ticks;      // total waktu blocking (timer ticks)
    uint64_t err_count;       // response device != OK_NODATA
} virtio_gpu_stats_t;

typedef struct {
    // PCI identity
    uint16_t bus, slot, func;

    // Capability physical locations (BAR phys + offset), di-map via HHDM.
    uint64_t common_cfg_phys;      // base BAR phys untuk COMMON_CFG
    uint32_t common_cfg_off;
    uint64_t notify_base_phys;     // base BAR phys untuk NOTIFY_CFG
    uint32_t notify_base_off;
    uint32_t notify_off_multiplier;
    uint64_t device_cfg_phys;
    uint32_t device_cfg_off;

    // MMIO virtual (HHDM translated)
    volatile virtio_pci_common_cfg_t* common;
    volatile uint16_t*  notify_base;
    volatile virtio_gpu_config_t* device_cfg;

    // Virtqueues
    virtq_t controlq;
    virtq_t cursorq;

    // Buffer command/response pre-alokasi + lock (IRQ-safe).
    gpu_page_t cmd_pages[VGPU_CMD_MAX_PAGES];
    uint32_t   cmd_pages_n;
    gpu_page_t resp_page;
    spinlock_t cmd_lock;

    // Phase 2C §9.4 — halaman command/response khusus cursorq (cmd di offset
    // 0, response di offset 512) + lock (dipakai dari timer IRQ via compositor
    // flush DAN dari syscall kwm_set_cursor — dua konteks).
    gpu_page_t cursor_page;
    spinlock_t cursor_lock;

    // Phase 2C §9.6 — statistik.
    virtio_gpu_stats_t stats;

    // Phase 2C §9.2 — fence async present. fence_of_head[slot] = fence_id
    // chain yang sedang in-flight pada descriptor head tsb (0 = tidak ada);
    // last_fence_done = fence tertinggi yang sudah selesai (device memproses
    // controlq in-order, jadi fence selesai selalu monotonic).
    uint64_t fence_counter;                       // fence_id terakhir yang dialokasikan
    uint64_t last_fence_done;
    uint64_t fence_of_head[VGPU_FENCE_MAX_HEADS];
    uint8_t  slot_of_head[VGPU_FENCE_MAX_HEADS];  // response slot async per head
    uint32_t async_slot_seq;                      // round-robin alokasi slot

    // Display info (dari GET_DISPLAY_INFO)
    uint32_t scanout_width;
    uint32_t scanout_height;
    uint32_t num_scanouts;

    // State
    int      initialized;
    int      negotiation_done;
} virtio_gpu_dev_t;

// Instance device tunggal (KyuzenOS tidak mendukung multi-GPU v1).
extern virtio_gpu_dev_t g_vgpu;

// Probe PCI & init device sampai DRIVER_OK. Return 0 sukses, <0 gagal.
// Dipanggil dari virtio_gpu_backend_ops.init().
int virtio_gpu_dev_probe(void);

// Kirim satu command virtio-gpu (buffer sudah berisi ctrl_hdr + payload).
// `out` = buffer response (harus ada ruang). Return 0 sukses, <0 error.
int virtio_gpu_dev_command(const void* cmd, uint32_t cmd_len,
                           void* out, uint32_t out_len);

// --- Phase 2C §9.1/§9.2: batching + fence async present ---
// Kirim DUA command sebagai DUA chain terpisah (spec: satu command per
// chain), dengan SATU notify dan TANPA menunggu completion. Kedua command
// diberi VIRTIO_GPU_FLAG_FENCE + fence_id sama. Return fence_id (>0)
// sukses, 0 gagal (tidak ada yang dikirim / queue penuh).
uint64_t virtio_gpu_dev_submit2(const void* cmd1, uint32_t cmd1_len,
                                const void* cmd2, uint32_t cmd2_len);

// Non-blocking: drain used ring, update last_fence_done, log error response.
// Return jumlah chain yang diselesaikan (>=0), <0 bila device belum init.
int virtio_gpu_dev_poll_fences(void);

// 1 = fence selesai, 0 = masih in-flight / id tidak dikenal.
int virtio_gpu_dev_fence_done(uint64_t fence);

// Blocking sampai fence selesai atau timeout (§6.9). 0 sukses, <0 timeout.
// PRECONDITION: single-context bersama submit2/dev_command (dipanggil dari
// titik yang sama, compositor_flush) — tidak mengambil cmd_lock karena
// dev_command memegang lock saat polling di jalurnya sendiri.
int virtio_gpu_dev_fence_wait(uint64_t fence);

// Kirim command cursor (UPDATE_CURSOR/MOVE_CURSOR) lewat CURSORQ — queue
// terpisah dari controlq (roadmap §6.5) supaya mouse move tidak antre di
// belakang command rendering. Synchronous (submit→notify→wait), IRQ-safe.
// Return 0 sukses (response OK_NODATA), <0 error/timeout.
int virtio_gpu_dev_cursor_command(const void* cmd, uint32_t cmd_len);

// Akses statistik (dibaca backend untuk ghal_gpu_stats).
const virtio_gpu_stats_t* virtio_gpu_dev_stats(void);

// Helper: alokasi resource_id monotonic (tidak pernah di-reuse, §6.3).
uint32_t virtio_gpu_next_resource_id(void);

#endif // VIRTIO_GPU_DEV_H
