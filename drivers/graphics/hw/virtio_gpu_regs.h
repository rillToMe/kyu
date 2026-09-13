#ifndef VIRTIO_GPU_REGS_H
#define VIRTIO_GPU_REGS_H

// ============================================================
// VirtIO-GPU register & command layout (drivers/graphics/hw/)
//
// Sesuai VirtIO 1.1 spec + virtio-gpu spec. Semua struct packed,
// field order PERSIS urutan spec — device (QEMU) baca byte offset
// absolut. JANGAN ubah urutan field (roadmap §5 rule 8).
// ============================================================

#include <stdint.h>

// --- Virtio PCI capability cfg_type (capability dengan cap_vndr==0x09) ---
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

// PCI Vendor/Device ID VirtIO-GPU
#define VIRTIO_PCI_VENDOR_ID  0x1AF4
#define VIRTIO_PCI_DEVICE_GPU 0x1050   // modern

// --- Device status flags ---
#define VIRTIO_STATUS_ACKNOWLEDGE      0x01
#define VIRTIO_STATUS_DRIVER           0x02
#define VIRTIO_STATUS_DRIVER_OK        0x04
#define VIRTIO_STATUS_FEATURES_OK      0x08
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED           0x80

// --- Virtqueue descriptor flags ---
#define VIRTQ_DESC_F_NEXT   1
#define VIRTQ_DESC_F_WRITE  2

// --- ctrl_hdr.flags (VirtIO-GPU spec: request fence pada response) ---
#define VIRTIO_GPU_FLAG_FENCE  (1u << 0)

// --- Feature bits (virtio-gpu) ---
#define VIRTIO_GPU_F_VIRGL          (1u << 0)
#define VIRTIO_GPU_F_EDID           (1u << 1)
#define VIRTIO_GPU_F_RESOURCE_BLOB  (1u << 3)

// ============================================================
// virtio_pci_common_cfg — feature negotiation, status, per-queue
// ============================================================
typedef struct {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t guest_feature_select;
    uint32_t guest_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed)) virtio_pci_common_cfg_t;

// --- virtio_gpu_config (DEVICE_CFG) ---
typedef struct {
    uint32_t events_read;
    uint32_t events_clear;
    uint32_t num_scanouts;
    uint32_t reserved;
} __attribute__((packed)) virtio_gpu_config_t;

// ============================================================
// Virtqueue structures (split virtqueue, VirtIO 1.1 §2.6)
// ============================================================
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];
} __attribute__((packed)) virtq_used_t;

// ============================================================
// VirtIO-GPU commands & responses
// ============================================================
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO      0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D    0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF        0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT           0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH        0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D   0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define VIRTIO_GPU_CMD_UPDATE_CURSOR         0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR           0x0301

#define VIRTIO_GPU_RESP_OK_NODATA            0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO      0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC           0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY    0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER 0x1204

// --- Formats ---
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM   1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM   2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM   3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM   4

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

// --- GET_DISPLAY_INFO response ---
typedef struct {
    uint32_t x, y;
    uint32_t width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

typedef struct {
    virtio_gpu_rect_t rect;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed)) virtio_gpu_display_one_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[16];
} __attribute__((packed)) virtio_gpu_resp_display_info_t;

// --- RESOURCE_CREATE_2D ---
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

// --- RESOURCE_UNREF ---
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

// --- mem_entry for ATTACH_BACKING ---
typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    virtio_gpu_mem_entry_t entries[];
} __attribute__((packed)) virtio_gpu_resource_attach_backing_t;

// --- SET_SCANOUT ---
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

// --- RESOURCE_FLUSH ---
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;

// --- UPDATE_CURSOR / MOVE_CURSOR (spec: virtio_gpu_update_cursor) ---
// resource 64x64 (format dengan alpha). x,y = posisi kiri-atas plane
// kursor di scanout. resource_id=0 pada MOVE_CURSOR = sembunyikan.
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t scanout_id;
    uint32_t resource_id;
    uint32_t x;
    uint32_t y;
} __attribute__((packed)) virtio_gpu_update_cursor_t;

// --- TRANSFER_TO_HOST_2D ---
typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

#endif // VIRTIO_GPU_REGS_H
