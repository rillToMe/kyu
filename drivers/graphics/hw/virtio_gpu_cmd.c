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

void virtio_gpu_cmd_detach_backing(virtio_gpu_ctrl_hdr_t* hdr,
                                   uint32_t resource_id) {
    virtio_gpu_cmd_hdr(hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
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
                                     uint32_t w, uint32_t h) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    c->r.x = x; c->r.y = y;
    c->r.width = w; c->r.height = h;
    c->offset = 0;   // v1: backing linear, offset 0
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
}

void virtio_gpu_cmd_move_cursor(virtio_gpu_update_cursor_t* c,
                                uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y) {
    virtio_gpu_cmd_hdr(&c->hdr, VIRTIO_GPU_CMD_MOVE_CURSOR);
    c->scanout_id = scanout_id;
    c->resource_id = resource_id;
    c->x = x;
    c->y = y;
}
