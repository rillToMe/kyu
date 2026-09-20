#ifndef VIRTIO_GPU_CMD_H
#define VIRTIO_GPU_CMD_H

// ============================================================
// VirtIO-GPU command encoder (drivers/graphics/hw/virtio_gpu_cmd.h)
//
// Fungsi untuk membangun command buffer (struct packed sesuai spec)
// di memori. Caller menyediakan buffer cukup besar. Murni encode —
// tidak mengirim; pengiriman lewat virtio_gpu_dev_command().
// ============================================================

#include <stdint.h>
#include "virtio_gpu_regs.h"

// Isi header command dengan type. ctx_id/padding di-nolkan.
void virtio_gpu_cmd_hdr(virtio_gpu_ctrl_hdr_t* hdr, uint32_t type);

// Build GET_DISPLAY_INFO (hanya header).
void virtio_gpu_cmd_get_display_info(virtio_gpu_ctrl_hdr_t* hdr);

// Build RESOURCE_CREATE_2D.
void virtio_gpu_cmd_resource_create_2d(virtio_gpu_resource_create_2d_t* c,
                                       uint32_t resource_id, uint32_t format,
                                       uint32_t width, uint32_t height);

// Build RESOURCE_UNREF.
void virtio_gpu_cmd_resource_unref(virtio_gpu_resource_unref_t* c,
                                   uint32_t resource_id);

// Build RESOURCE_ATTACH_BACKING (entries diisi caller, nr = jumlah).
void virtio_gpu_cmd_attach_backing(virtio_gpu_resource_attach_backing_t* c,
                                   uint32_t resource_id,
                                   uint32_t nr_entries);

// Build RESOURCE_DETACH_BACKING (command 32 byte: hdr + resource_id +
// padding — mengirim hanya header membuat device menolak "command data size
// incorrect" dan DETACH jadi no-op).
void virtio_gpu_cmd_detach_backing(virtio_gpu_resource_detach_backing_t* c,
                                   uint32_t resource_id);

// Build SET_SCANOUT.
void virtio_gpu_cmd_set_scanout(virtio_gpu_set_scanout_t* c,
                                uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h);

// Build RESOURCE_FLUSH.
void virtio_gpu_cmd_resource_flush(virtio_gpu_resource_flush_t* c,
                                   uint32_t resource_id,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h);

// Build TRANSFER_TO_HOST_2D. `pitch_bytes` = lebar satu baris backing
// (resource linear = width * 4). Command membawa offset = y*pitch + x*4 —
// device memakai offset itu sebagai awal baris sumber, jadi jangan diisi 0
// untuk rect yang tidak mulai di (0,0).
void virtio_gpu_cmd_transfer_to_host(virtio_gpu_transfer_to_host_2d_t* c,
                                     uint32_t resource_id,
                                     uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h,
                                     uint32_t pitch_bytes);

// Build UPDATE_CURSOR (set gambar + posisi kursor; resource 64x64).
void virtio_gpu_cmd_update_cursor(virtio_gpu_update_cursor_t* c,
                                  uint32_t scanout_id, uint32_t resource_id,
                                  uint32_t x, uint32_t y);

// Build MOVE_CURSOR (posisi baru; resource_id=0 = sembunyikan).
void virtio_gpu_cmd_move_cursor(virtio_gpu_update_cursor_t* c,
                                uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y);

#endif // VIRTIO_GPU_CMD_H
