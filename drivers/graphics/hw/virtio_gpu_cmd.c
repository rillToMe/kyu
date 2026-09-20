// ============================================================
// VirtIO-GPU command encoder (drivers/graphics/hw/virtio_gpu_cmd.c)
// ============================================================

#include "virtio_gpu_cmd.h"

void virtio_gpu_cmd_hdr(virtio_gpu_ctrl_hdr_t* hdr, uint32_t type) {
    hdr->type     = type;
    hdr->flags    = 0;
    hdr->fence_id = 0;
    hdr->ctx_id   = 0;
    hdr->padding  = 0;
}

void virtio_gpu_cmd_get_display_info(virtio_gpu_ctrl_hdr_t* hdr) {
    virtio_gpu_cmd_hdr(hdr, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);
}

void virtio_gpu_cmd_resource_create_2d(virtio_gpu_resource_create_2d_t* c,
                                       uint32_t resource_id, uint32_t format,
                                       uint32_t width, uint32_t height) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    c->resource_id = resource_id;
    c->format = format;
    c->width  = width;
    c->height = height;
}

void virtio_gpu_cmd_resource_unref(virtio_gpu_resource_unref_t* c,
                                   uint32_t resource_id) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
    c->resource_id = resource_id;
    c->padding = 0;
}

void virtio_gpu_cmd_attach_backing(virtio_gpu_resource_attach_backing_t* c,
                                   uint32_t resource_id,
                                   uint32_t nr_entries) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    c->resource_id = resource_id;
    c->nr_entries = nr_entries;
}

void virtio_gpu_cmd_detach_backing(virtio_gpu_resource_detach_backing_t* c,
                                   uint32_t resource_id) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
    c->resource_id = resource_id;
    c->padding = 0;
}

void virtio_gpu_cmd_set_scanout(virtio_gpu_set_scanout_t* c,
                                uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    c->r.x = x; c->r.y = y;
    c->r.width = w; c->r.height = h;
    c->scanout_id = scanout_id;
    c->resource_id = resource_id;
}

void virtio_gpu_cmd_resource_flush(virtio_gpu_resource_flush_t* c,
                                   uint32_t resource_id,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    c->r.x = x; c->r.y = y;
    c->r.width = w; c->r.height = h;
    c->resource_id = resource_id;
    c->padding = 0;
}

void virtio_gpu_cmd_transfer_to_host(virtio_gpu_transfer_to_host_2d_t* c,
                                     uint32_t resource_id,
                                     uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h,
                                     uint32_t pitch_bytes) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    c->r.x = x; c->r.y = y;
    c->r.width = w; c->r.height = h;
    // offset = awal data yang ditransfer di backing, BUKAN 0 kecuali rect-nya
    // memang di (0,0): device membaca baris sumber h dari `offset + pitch*h`
    // lalu menulisnya ke baris (r.y+h) kolom r.x. Dengan offset 0, rect di
    // y/x > 0 menarik data dari pojok kiri-atas backing → isi layar salah
    // (teks nempel potongan pojok). Formula yang sama dipakai driver Linux:
    // off = x*cpp + y*pitches[0] (virtio_gpu_update_dumb_bo).
    c->offset = (uint64_t)y * pitch_bytes + (uint64_t)x * 4;
    c->resource_id = resource_id;
    c->padding = 0;
}

void virtio_gpu_cmd_update_cursor(virtio_gpu_update_cursor_t* c,
                                  uint32_t scanout_id, uint32_t resource_id,
                                  uint32_t x, uint32_t y) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_UPDATE_CURSOR);
    c->scanout_id = scanout_id;
    c->resource_id = resource_id;
    c->x = x;
    c->y = y;
    c->hot_x = 0;   // semua bentuk kursor kita hot spot (0,0)
    c->hot_y = 0;
    c->padding = 0;
    c->tail_padding = 0;
}

void virtio_gpu_cmd_move_cursor(virtio_gpu_update_cursor_t* c,
                                uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_MOVE_CURSOR);
    c->scanout_id = scanout_id;
    c->resource_id = resource_id;
    c->x = x;
    c->y = y;
    c->hot_x = 0;   // device hanya memakai ini untuk UPDATE_CURSOR
    c->hot_y = 0;
    c->padding = 0;
    c->tail_padding = 0;
}
