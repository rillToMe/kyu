// ============================================================
// Host-side test: VirtIO-GPU command encoder (test/virtio_gpu_cmd_test.c)
//
// Diuji tanpa device: hasil encode dibandingkan dengan cara device (QEMU)
// MEMBACA command itu. Yang dikunci di sini adalah offset TRANSFER_TO_HOST_2D,
// karena salahnya tidak terlihat di log mana pun — device tetap menjawab
// OK_NODATA, hanya isi rect-nya yang salah.
//
// Referensi perilaku device (hw/display/virtio-gpu.c,
// virtio_gpu_transfer_to_host_2d):
//
//   if (r.x || r.width != image_width)          // per baris
//       for (h = 0; h < r.height; h++) {
//           src = offset + stride * h;
//           dst = (r.y + h) * stride + r.x * bpp;
//           iov_to_buf(backing, src, image + dst, r.width * bpp);
//       }
//   else                                        // blok penuh selebar image
//       iov_to_buf(backing, offset, image + r.y * stride, stride * r.height);
//
// Jadi `offset` = awal data yang ditransfer di backing. Driver Linux
// (virtio_gpu_update_dumb_bo) mengirim off = x*cpp + y*pitches[0].
//
// Jalankan: make test-virtio-cmd
// ============================================================

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "virtio_gpu_cmd.h"

#define W 32u
#define H 8u
#define PITCH (W * 4u)

static uint32_t g_backing[W * H];   // backing resource (linear, pitch = W*4)
static uint32_t g_image[W * H];     // "pixman image" di sisi device

// Pola unik per piksel: (y << 16) | x — supaya salah baris/kolom langsung
// ketahuan, bukan cuma "berbeda".
static void fill_pattern(uint32_t* buf) {
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            buf[y * W + x] = (y << 16) | x;
}

// Emulasi virtio_gpu_transfer_to_host_2d() — copy apa adanya dari QEMU.
static void device_transfer(const virtio_gpu_transfer_to_host_2d_t* c) {
    uint32_t bpp = 4;
    uint32_t stride = PITCH;
    const uint8_t* backing = (const uint8_t*)g_backing;
    uint8_t* img = (uint8_t*)g_image;

    if (c->r.x || c->r.width != W) {
        for (uint32_t h = 0; h < c->r.height; h++) {
            uint64_t src = c->offset + (uint64_t)stride * h;
            uint64_t dst = ((uint64_t)c->r.y + h) * stride + (uint64_t)c->r.x * bpp;
            memcpy(img + dst, backing + src, c->r.width * bpp);
        }
    } else {
        uint64_t src = c->offset;
        uint64_t dst = (uint64_t)c->r.y * stride + (uint64_t)c->r.x * bpp;
        memcpy(img + dst, backing + src, (uint64_t)stride * c->r.height);
    }
}

// 1 = seluruh piksel di dalam rect image == backing (inilah yang benar).
static int rect_matches(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++)
            if (g_image[(y + j) * W + (x + i)] != g_backing[(y + j) * W + (x + i)])
                return 0;
    return 1;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   - %s\n", msg); } \
    else      { printf("  FAIL - %s\n", msg); g_fail++; } \
} while (0)

int main(void) {
    printf("virtio-gpu command encoder test\n");

    fill_pattern(g_backing);

    // --- rect penuh di (0,0): offset harus 0 ---
    {
        virtio_gpu_transfer_to_host_2d_t c;
        virtio_gpu_cmd_transfer_to_host(&c, 1, 0, 0, W, H, PITCH);
        CHECK(c.hdr.type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, "type TRANSFER_TO_HOST_2D");
        CHECK(c.offset == 0, "rect penuh di (0,0) → offset 0");
        memset(g_image, 0, sizeof(g_image));
        device_transfer(&c);
        CHECK(rect_matches(0, 0, W, H), "rect (0,0) mentransfer isi yang benar");
    }

    // --- strip selebar image tapi TIDAK di baris 0 (jalur "else" QEMU) ---
    {
        virtio_gpu_transfer_to_host_2d_t c;
        virtio_gpu_cmd_transfer_to_host(&c, 1, 0, 3, W, 2, PITCH);
        CHECK(c.offset == 3u * PITCH, "strip y=3 → offset = y*pitch");
        memset(g_image, 0, sizeof(g_image));
        device_transfer(&c);
        CHECK(rect_matches(0, 3, W, 2), "strip y=3 mentransfer baris yang benar");
    }

    // --- rect kecil di x,y > 0 (jalur "per baris" QEMU) ---
    {
        virtio_gpu_transfer_to_host_2d_t c;
        virtio_gpu_cmd_transfer_to_host(&c, 1, 5, 2, 8, 3, PITCH);
        CHECK(c.offset == 2u * PITCH + 5u * 4u, "rect (5,2) → offset = y*pitch + x*4");
        CHECK(c.r.x == 5 && c.r.y == 2 && c.r.width == 8 && c.r.height == 3,
              "rect tersimpan apa adanya (x,y,w,h)");
        memset(g_image, 0, sizeof(g_image));
        device_transfer(&c);
        CHECK(rect_matches(5, 2, 8, 3), "rect (5,2)+8x3 mentransfer isi yang benar");
    }

    // --- regresi: offset 0 untuk rect di luar (0,0) HARUS salah ---
    // (uji sensitivitas: memastikan test ini benar-benar menangkap bug lama)
    {
        virtio_gpu_transfer_to_host_2d_t c;
        virtio_gpu_cmd_transfer_to_host(&c, 1, 5, 2, 8, 3, PITCH);
        c.offset = 0;                     // perilaku lama
        memset(g_image, 0, sizeof(g_image));
        device_transfer(&c);
        CHECK(!rect_matches(5, 2, 8, 3), "offset 0 pada rect (5,2) memang salah (deteksi bug)");
    }

    // --- ukuran & field lain command tidak berubah ---
    {
        virtio_gpu_transfer_to_host_2d_t c;
        virtio_gpu_cmd_transfer_to_host(&c, 7, 1, 1, 2, 2, PITCH);
        CHECK(sizeof(c) == 56, "sizeof command = 56 byte (spec/QEMU)");
        CHECK(c.resource_id == 7 && c.padding == 0 && c.hdr.flags == 0,
              "resource_id/padding/flags terisi benar");
    }

    printf("%s\n", g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
