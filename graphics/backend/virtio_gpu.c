// ============================================================
// VirtIO-GPU Backend (graphics/backend/virtio_gpu.c)
//
// Menerjemahkan ghal_backend_ops_t ke command VirtIO-GPU lewat GPU
// Driver Core. Phase 2A: bring-up + resource + scanout + present
// (target: satu warna solid full-screen di QEMU).
//
// struct ghal_surface (opaque) untuk backend ini:
//   resource_id + daftar backing pages (host RAM) + geometry.
// ============================================================

#include "ghal.h"
#include "virtio_gpu_dev.h"
#include "virtio_gpu_cmd.h"
#include "gpu_alloc.h"
#include "heap.h"
#include "display.h"
#include "serial.h"
#include <string.h>
#include <stddef.h>

// Format VirtIO untuk XRGB8888 kita: byte [B,G,R,X] = X8R8G8B8_UNORM.

struct ghal_surface {
    uint32_t    resource_id;
    uint32_t    width;
    uint32_t    height;
    ghal_format_t format;
    gpu_page_t* pages;         // backing pages (contiguous logical list)
    uint32_t    num_pages;
    uint32_t*   backing_virt;  // = pages[0].virt (linear)
    uint64_t    backing_phys;  // = pages[0].phys (untuk attach)
    uint8_t     scanout_set;   // 1 = sudah SET_SCANOUT
};

static int g_virtio_active = 0;

// Resolusi output yang dipakai compositor. pmodes[0] host hanyalah salah satu
// masukan — lihat negosiasi di virtio_init(). Resource virtio linear → pitch
// selalu width*4.
static uint32_t g_pref_width  = 0;
static uint32_t g_pref_height = 0;

// Scanout aktif (dibuat compositor lewat ghal_surface_create_scanout). Dipakai
// jalur panic/BSOD: menggambar langsung ke backing resource ini — bukan ke
// framebuffer Limine, yang tidak lagi tampil sejak device punya scanout.
static ghal_surface_t* g_scanout_surface = NULL;

// Didefinisikan di bawah (butuh surface_create), dipakai virtio_init().
static int virtio_probe_scanout(uint32_t w, uint32_t h);

// Mode yang diiklankan device (GET_DISPLAY_INFO pmodes[]). pmodes[0] yang
// enabled = scanout aktif. Resource virtio linear → pitch = width*4.
#define VGPU_MAX_PMODES 16
static display_mode_t g_pmodes[VGPU_MAX_PMODES];
static uint32_t       g_pmode_count = 0;

// --- helper: kirim command & periksa response OK_NODATA ---
static int vgpu_send_ok(const void* cmd, uint32_t cmd_len) {
    uint32_t resp[8] = {0};   // ctrl_hdr response
    if (virtio_gpu_dev_command(cmd, cmd_len, resp, sizeof(resp)) != 0) return -1;
    if (resp[0] != VIRTIO_GPU_RESP_OK_NODATA) return -1;
    return 0;
}

static int virtio_init(void) {
    if (g_virtio_active) return 0;
    if (virtio_gpu_dev_probe() != 0) return -1;

    // GET_DISPLAY_INFO → simpan resolusi.
    virtio_gpu_ctrl_hdr_t cmd;
    virtio_gpu_cmd_get_display_info(&cmd);
    virtio_gpu_resp_display_info_t resp;
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_dev_command(&cmd, sizeof(cmd), &resp, sizeof(resp)) != 0) return -1;
    if (resp.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) return -1;

    // Enumerasi pmodes enabled → daftar mode kanonik. pmodes[0] enabled =
    // scanout aktif (dipakai compositor untuk ukuran main surface).
    g_pmode_count = 0;
    for (int i = 0; i < VGPU_MAX_PMODES; i++) {
        if (!resp.pmodes[i].enabled) continue;
        uint32_t w = resp.pmodes[i].rect.width;
        uint32_t h = resp.pmodes[i].rect.height;
        if (w == 0 || h == 0 || w > GHAL_MAX_DIM || h > GHAL_MAX_DIM) continue;
        display_mode_t* m = &g_pmodes[g_pmode_count++];
        m->width = w; m->height = h;
        m->pitch_bytes = (uint32_t)((uint64_t)w * 4ULL);
        m->bpp = 32;
        m->format = DISPLAY_FMT_XRGB8888;
    }
    if (g_pmode_count == 0) return -1;
    g_vgpu.scanout_width  = g_pmodes[0].width;
    g_vgpu.scanout_height = g_pmodes[0].height;
    g_pref_width  = g_vgpu.scanout_width;
    g_pref_height = g_vgpu.scanout_height;

    // --- Negosiasi resolusi ---
    // pmodes[0] = ukuran yang dilaporkan host SAAT QUERY. Di QEMU angka itu
    // hanya mencerminkan ukuran window host (di setup -device
    // virtio-vga,xres=1920,yres=1080 sering tetap 640x480), sehingga
    // mempercayainya apa adanya membuat layar tampak "kotak" meski firmware
    // sudah menyetel mode besar.
    //
    // VirtIO-GPU v1 tidak punya command mode-set guest, TAPI rect SET_SCANOUT
    // bebas: device (QEMU) menyesuaikan console/window ke ukuran surface
    // scanout. Jadi resolusi dinegosiasikan SEKALI di sini — sebelum
    // display_sync_from_backend()/display_alloc_buffers() mengunci mode dan
    // mengalokasi buffer — dengan resource probe: device terbukti menerima
    // ukuran besar baru mode itu dipakai. Salah menebak = desktop tidak pernah
    // tampil, jadi tebakan tidak boleh dipakai tanpa bukti.
    const display_mode_t* boot = display_get_mode();
    if (boot && boot->width && boot->height &&
        boot->width <= GHAL_MAX_DIM && boot->height <= GHAL_MAX_DIM &&
        (uint64_t)boot->width * boot->height >
            (uint64_t)g_pref_width * g_pref_height) {
        if (virtio_probe_scanout(boot->width, boot->height) == 0) {
            g_pref_width  = boot->width;
            g_pref_height = boot->height;
        } else {
            serial_print("[vgpu] mode boot ");
            serial_dec(boot->width); serial_print("x"); serial_dec(boot->height);
            serial_print(" ditolak device — tetap di mode host\n");
        }
    }
    serial_print("[vgpu] mode host=");
    serial_dec(g_vgpu.scanout_width); serial_print("x"); serial_dec(g_vgpu.scanout_height);
    serial_print(" guest=");
    serial_dec(g_pref_width); serial_print("x"); serial_dec(g_pref_height);
    serial_print("\n");

    g_virtio_active = 1;
    return 0;
}

static void virtio_shutdown(void) { g_virtio_active = 0; }

// --- surface_create: buat resource + attach backing ---
static ghal_surface_t* virtio_surface_create(uint32_t w, uint32_t h, ghal_format_t fmt) {
    if (w == 0 || h == 0 || w > GHAL_MAX_DIM || h > GHAL_MAX_DIM) return NULL;
    uint64_t bytes = (uint64_t)w * h * 4;
    if (bytes == 0 || w != 0 && (bytes / 4 / w) != h) return NULL;
    uint32_t num_pages = (uint32_t)((bytes + 4095) / 4096);

    gpu_page_t* pages = (gpu_page_t*)kmalloc(sizeof(gpu_page_t) * num_pages);
    if (!pages) { extern void serial_print(const char* s); serial_print("[vgpu] surface: kmalloc pages failed\n"); return NULL; }
    // Backing HARUS contiguous: backing_virt dipakai sebagai satu buffer linear
    // (`backing_virt + y*width`), jadi halaman non-contiguous akan menulis ke
    // memori acak. gpu_alloc_pages() tidak menjamin ini.
    if (gpu_alloc_pages_contiguous(num_pages, pages) != 0) {
        extern void serial_print(const char* s);
        serial_print("[vgpu] surface: contiguous backing alloc failed\n");
        kfree(pages);
        return NULL;
    }

    struct ghal_surface* s = (struct ghal_surface*)kmalloc(sizeof(*s));
    if (!s) { gpu_free_pages(pages, num_pages); kfree(pages); return NULL; }
    s->resource_id = virtio_gpu_next_resource_id();
    s->width = w; s->height = h; s->format = fmt;
    s->pages = pages; s->num_pages = num_pages;
    s->backing_virt = pages[0].virt;
    s->backing_phys = pages[0].phys;
    s->scanout_set = 0;

    // RESOURCE_CREATE_2D
    // Format: XRGB8888 kita = 0x00RRGGBB, little-endian memory byte order
    // B,G,R,X → VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM (nama virtio = urutan BYTE).
    // Memakai X8R8G8B8 (urutan X,R,G,B) membuat device membaca byte0 sebagai X
    // dan byte3 sebagai B → channel biru hilang (terbukti: gray 30,30,30
    // tampil 30,30,0).
    // ARGB8888 (kursor): byte B,G,R,A → B8G8R8A8_UNORM (alpha di-respect
    // device untuk plane kursor).
    uint32_t vfmt = (fmt == GHAL_FMT_ARGB8888)
                        ? VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM
                        : VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    virtio_gpu_resource_create_2d_t c;
    virtio_gpu_cmd_resource_create_2d(&c, s->resource_id, vfmt, w, h);
    if (vgpu_send_ok(&c, sizeof(c)) != 0) {
        gpu_free_pages(pages, num_pages); kfree(pages); kfree(s);
        return NULL;
    }

    // ATTACH_BACKING: satu entry per halaman.
    // Bangun command buffer dengan array entries.
    uint32_t nr = num_pages;
    size_t cmd_size = sizeof(virtio_gpu_resource_attach_backing_t) + nr * sizeof(virtio_gpu_mem_entry_t);
    virtio_gpu_resource_attach_backing_t* ac =
        (virtio_gpu_resource_attach_backing_t*)kmalloc(cmd_size);
    if (!ac) { virtio_gpu_cmd_resource_unref(&(virtio_gpu_resource_unref_t){0}, s->resource_id); gpu_free_pages(pages,num_pages); kfree(pages); kfree(s); return NULL; }
    virtio_gpu_cmd_attach_backing(ac, s->resource_id, nr);
    for (uint32_t i = 0; i < nr; i++) {
        ac->entries[i].addr = pages[i].phys;
        ac->entries[i].length = 4096;
        ac->entries[i].padding = 0;
    }
    int ok = vgpu_send_ok(ac, (uint32_t)cmd_size);
    kfree(ac);
    if (ok != 0) {
        // cleanup: unref resource
        virtio_gpu_resource_unref_t ur;
        virtio_gpu_cmd_resource_unref(&ur, s->resource_id);
        vgpu_send_ok(&ur, sizeof(ur));
        gpu_free_pages(pages, num_pages); kfree(pages); kfree(s);
        return NULL;
    }

    // SET_SCANOUT sengaja TIDAK dikirim di sini: surface biasa (mis. image
    // kursor 64x64) tidak boleh jadi output. Framebuffer utama compositor
    // lewat virtio_surface_create_scanout(), yang mengirim scanout eksplisit.
    return s;
}

static void virtio_surface_destroy(ghal_surface_t* s) {
    if (!s) return;
    // DETACH_BACKING lalu UNREF.
    virtio_gpu_resource_detach_backing_t det;
    virtio_gpu_cmd_detach_backing(&det, s->resource_id);
    vgpu_send_ok(&det, sizeof(det));
    virtio_gpu_resource_unref_t ur;
    virtio_gpu_cmd_resource_unref(&ur, s->resource_id);
    vgpu_send_ok(&ur, sizeof(ur));
    gpu_free_pages(s->pages, s->num_pages);
    kfree(s->pages);
    kfree(s);
}

// ------------------------------------------------------------
// Scanout
// ------------------------------------------------------------

// SET_SCANOUT untuk seluruh resource ini di (0,0). Rect inilah yang menentukan
// ukuran output — QEMU menyesuaikan console/window ke ukuran surface scanout.
// Return 0 sukses.
static int virtio_set_scanout(ghal_surface_t* s) {
    if (!s) return -1;
    virtio_gpu_set_scanout_t so;
    virtio_gpu_cmd_set_scanout(&so, 0, s->resource_id, 0, 0, s->width, s->height);
    if (vgpu_send_ok(&so, sizeof(so)) != 0) return -1;
    s->scanout_set = 1;
    return 0;
}

// Framebuffer utama compositor (ghal_surface_create_scanout). Ukurannya boleh
// berbeda dari pmodes host — lihat negosiasi di virtio_init().
static ghal_surface_t* virtio_surface_create_scanout(uint32_t w, uint32_t h,
                                                    ghal_format_t fmt) {
    ghal_surface_t* s = virtio_surface_create(w, h, fmt);
    if (!s) return NULL;
    if (virtio_set_scanout(s) != 0) {
        serial_print("[vgpu] SET_SCANOUT gagal ");
        serial_dec(w); serial_print("x"); serial_dec(h);
        serial_print(" — device menolak ukuran surface\n");
    }
    // Disimpan: jalur panic/BSOD menggambar LANGSUNG ke backing resource ini
    // (lihat virtio_scanout_map). Hanya resource scanout yang boleh dipakai —
    // tulisan ke framebuffer Limine tidak pernah terlihat lagi sejak device
    // punya scanout sendiri.
    g_scanout_surface = s;
    return s;
}

// Buktikan device menerima (w,h) sebagai scanout tanpa mengunci mode: resource
// probe → SET_SCANOUT → lepas lagi (scanout probe mati bersama resource;
// compositor membuat surface-nya sendiri segera setelah ini).
static int virtio_probe_scanout(uint32_t w, uint32_t h) {
    ghal_surface_t* s = virtio_surface_create(w, h, GHAL_FMT_XRGB8888);
    if (!s) return -1;
    int ok = virtio_set_scanout(s);
    virtio_surface_destroy(s);
    return ok;
}

static void virtio_surface_upload(ghal_surface_t* s, const uint32_t* src,
                                  uint32_t src_pitch, ghal_rect_t rect) {
    if (!s || !src) return;
    if (rect.x >= s->width || rect.y >= s->height) return;
    uint32_t maxw = s->width - rect.x, maxh = s->height - rect.y;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;
    const uint32_t* srow = src + (uint64_t)rect.y * src_pitch + rect.x;
    uint32_t* drow = s->backing_virt + (uint64_t)rect.y * s->width + rect.x;
    for (uint32_t y = 0; y < rect.h; y++) {
        memcpy(drow, srow, rect.w * 4);
        srow += src_pitch;
        drow += s->width;
    }
}

static void virtio_fill_rect(ghal_surface_t* dst, ghal_rect_t rect, uint32_t argb) {
    if (!dst) return;
    if (rect.x >= dst->width || rect.y >= dst->height) return;
    uint32_t maxw = dst->width - rect.x, maxh = dst->height - rect.y;
    if (rect.w > maxw) rect.w = maxw;
    if (rect.h > maxh) rect.h = maxh;
    uint32_t color = argb & 0xFFFFFF;
    for (uint32_t y = 0; y < rect.h; y++) {
        uint32_t* row = dst->backing_virt + (uint64_t)(rect.y + y) * dst->width + rect.x;
        for (uint32_t x = 0; x < rect.w; x++) row[x] = color;
    }
}

static void virtio_blit(ghal_surface_t* dst, ghal_rect_t dst_rect,
                        ghal_surface_t* src, ghal_rect_t src_rect) {
    if (!dst || !src) return;
    if (dst_rect.w != src_rect.w || dst_rect.h != src_rect.h) return;   // v1 no scaling
    if (dst_rect.x >= dst->width || dst_rect.y >= dst->height) return;
    uint32_t w = dst_rect.w, h = dst_rect.h;
    if (w > dst->width - dst_rect.x) w = dst->width - dst_rect.x;
    if (h > dst->height - dst_rect.y) h = dst->height - dst_rect.y;
    if (src_rect.x >= src->width || src_rect.y >= src->height) return;
    const uint32_t* srow = src->backing_virt + (uint64_t)src_rect.y * src->width + src_rect.x;
    uint32_t* drow = dst->backing_virt + (uint64_t)dst_rect.y * dst->width + dst_rect.x;
    for (uint32_t y = 0; y < h; y++) {
        memcpy(drow, srow, w * 4);
        srow += src->width;
        drow += dst->width;
    }
}

// Fence_id present terakhir yang berhasil dikirim (Phase 2C §9.2).
static uint64_t g_last_present_fence = 0;

// Forward (didefinisikan di bawah, dipakai Fix B di virtio_present).
static int virtio_fence_pending(uint64_t fence);
static void virtio_fence_wait(uint64_t fence);

extern void serial_print(const char* s);

static void virtio_present(ghal_surface_t* s, const ghal_rect_t* rect) {
    if (!s) return;
    ghal_rect_t r;
    if (rect) r = *rect;
    else { r.x = 0; r.y = 0; r.w = s->width; r.h = s->height; }

    // Jaring pengaman: bila surface ini belum berhasil jadi scanout (device
    // menolak saat create / scanout probe belum tergantikan), ulangi tiap
    // frame sampai berhasil. Steady state: sudah beres di
    // virtio_surface_create_scanout() → nol command extra per frame.
    if (!s->scanout_set) (void)virtio_set_scanout(s);

    // Fix B: jangan menumpuk batch baru ke device yang tidak me-reap.
    // Bila fence batch sebelumnya masih outstanding setelah wait terbatas,
    // lewati frame ini — dirty dipertahankan compositor, dicoba lagi flush
    // berikutnya. Menumpuk submit hanya memperdalam wedge (pool habis,
    // tiap flush = timeout penuh).
    if (g_last_present_fence != 0 &&
        virtio_fence_pending(g_last_present_fence)) {
        virtio_fence_wait(g_last_present_fence);
        if (virtio_fence_pending(g_last_present_fence)) {
            static uint32_t skip_n = 0;
            if ((skip_n++ % 64) == 0)
                serial_print("[vgpu] present: skipped, fence outstanding\n");
            return;
        }
    }

    // Phase 2C §9.1+§9.2: TRANSFER + FLUSH dikirim sebagai DUA chain
    // terpisah (spec: satu command per chain — dua ctrl_hdr dalam satu
    // buffer membuat device memproses yang pertama dan membuang sisanya),
    // SATU notify, TANPA wait. Fence dari frame sebelumnya diverifikasi
    // compositor sebelum upload berikutnya (backpressure §9.2), jadi steady
    // state tidak ada busy-poll sama sekali per frame.
    virtio_gpu_transfer_to_host_2d_t t;
    virtio_gpu_resource_flush_t f;
    // pitch backing = s->width * 4 (resource linear; device memakai stride
    // yang sama untuk resource 32bpp, jadi baris sumber cocok dengan backing).
    virtio_gpu_cmd_transfer_to_host(&t, s->resource_id, r.x, r.y, r.w, r.h,
                                    s->width * 4);
    virtio_gpu_cmd_resource_flush(&f, s->resource_id, r.x, r.y, r.w, r.h);
    uint64_t fence = virtio_gpu_dev_submit2(&t, sizeof(t), &f, sizeof(f));
    if (fence != 0) {
        g_last_present_fence = fence;
        g_vgpu.stats.present_count++;
    }
    else serial_print("[vgpu] present: submit batch failed\n");
}

// --- Fence ops (Phase 2C §9.2) ---
static uint64_t virtio_present_fence(void) {
    return g_last_present_fence;
}

static int virtio_fence_pending(uint64_t fence) {
    return virtio_gpu_dev_fence_done(fence) == 0;   // 0 = belum selesai
}

static void virtio_fence_wait(uint64_t fence) {
    (void)virtio_gpu_dev_fence_wait(fence);
}

// --- Hardware cursor (Phase 2C §9.4) ---
// Image kursor = surface 64x64 ARGB8888 milik compositor (owner §6.2);
// backend hanya menyimpan referensi + mengirim command.
static ghal_surface_t* g_cursor_img;   // referensi surface kursor aktif
static uint32_t g_cursor_x, g_cursor_y;

static int virtio_cursor_update(ghal_surface_t* img, int hot_x, int hot_y) {
    (void)hot_x; (void)hot_y;   // semua bentuk kita hot spot (0,0)
    if (!img || img->width != 64 || img->height != 64 ||
        img->format != GHAL_FMT_ARGB8888) return -1;

    // TRANSFER seluruh image backing → resource (sync, controlq).
    // Rect penuh di (0,0) → offset 0; pitch = lebar backing kursor.
    virtio_gpu_transfer_to_host_2d_t t;
    virtio_gpu_cmd_transfer_to_host(&t, img->resource_id, 0, 0, 64, 64,
                                    img->width * 4);
    if (vgpu_send_ok(&t, sizeof(t)) != 0) return -1;

    // UPDATE_CURSOR di cursorq: definisikan plane pada posisi tersimpan.
    virtio_gpu_update_cursor_t c;
    virtio_gpu_cmd_update_cursor(&c, 0, img->resource_id, g_cursor_x, g_cursor_y);
    if (virtio_gpu_dev_cursor_command(&c, sizeof(c)) != 0) return -1;
    g_cursor_img = img;
    return 0;
}

static void virtio_cursor_move(int x, int y) {
    if (!g_cursor_img) return;   // belum ada image — tidak ada yang digerakkan
    // Clamp defensif: koordinat negatif dibungkus uint32 di command → reject
    // device. Mouse driver clamp ke layar, tapi jangan berasumsi.
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    g_cursor_x = (uint32_t)x; g_cursor_y = (uint32_t)y;
    virtio_gpu_update_cursor_t c;
    virtio_gpu_cmd_move_cursor(&c, 0, g_cursor_img->resource_id, g_cursor_x, g_cursor_y);
    (void)virtio_gpu_dev_cursor_command(&c, sizeof(c));
}

// ------------------------------------------------------------
// Scanout darurat (jalur panic/BSOD)
// ------------------------------------------------------------
// Kernel panic menggambar dengan cli, TANPA heap/lock/IRQ. Backend ini
// menyerahkan backing resource scanout supaya BSOD mendarat di tempat yang
// benar-benar tampil, lalu mengirimnya lewat jalur command try-lock.
static int virtio_scanout_map(uint32_t** pixels, uint32_t* width, uint32_t* height,
                              uint32_t* pitch_px) {
    if (!pixels || !width || !height || !pitch_px) return -1;
    ghal_surface_t* s = g_scanout_surface;
    if (!s || !s->backing_virt || !s->scanout_set) return -1;
    if (s->format != GHAL_FMT_XRGB8888) return -1;   // image kursor bukan output
    if (s->width == 0 || s->height == 0) return -1;
    *pixels   = s->backing_virt;
    *width    = s->width;
    *height   = s->height;
    *pitch_px = s->width;         // resource linear: pitch = width * 4 byte
    return 0;
}

// Kirim region yang digambar panic ke device: TRANSFER (guest backing → pixmap
// host) lalu RESOURCE_FLUSH (repaint region). SENGAJA tidak memakai
// virtio_present(): present compositor melewati frame yang fence present-nya
// masih outstanding — di jalur panic melewati frame berarti BSOD tidak pernah
// muncul. Jadi di sini command dikirim langsung, dan kalau lock sedang dipakai
// CPU lain (termasuk CPU yang fault) command DILEWATI tanpa menunggu; piksel
// sudah ada di backing, jadi percobaan berikutnya mengirimnya lagi.
static int virtio_scanout_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    ghal_surface_t* s = g_scanout_surface;
    if (!s || w == 0 || h == 0) return -1;
    if (x >= s->width || y >= s->height) return -1;
    if (w > s->width - x)  w = s->width - x;
    if (h > s->height - y) h = s->height - y;

    virtio_gpu_transfer_to_host_2d_t t;
    virtio_gpu_cmd_transfer_to_host(&t, s->resource_id, x, y, w, h, s->width * 4);
    if (virtio_gpu_dev_command_try(&t, sizeof(t)) != 0) return -1;

    virtio_gpu_resource_flush_t f;
    virtio_gpu_cmd_resource_flush(&f, s->resource_id, x, y, w, h);
    if (virtio_gpu_dev_command_try(&f, sizeof(f)) != 0) return -1;
    return 0;
}

// --- Statistik (Phase 2C §9.6) ---
static int virtio_gpu_stats(ghal_gpu_stats_t* out) {
    const virtio_gpu_stats_t* s = virtio_gpu_dev_stats();
    out->present_count = s->present_count;
    out->cmd_count     = s->cmd_count;
    out->cmd_bytes     = s->cmd_bytes;
    out->notify_count  = s->notify_count;
    out->wait_calls    = s->wait_calls;
    out->wait_ticks    = s->wait_ticks;
    out->err_count     = s->err_count;
    return 0;
}

// --- Display mode (host-controlled; guest tidak punya mode-set di v1) ---
static int virtio_mode_get(display_mode_t* out) {
    if (!out || g_pref_width == 0 || g_pref_height == 0) return -1;
    out->width       = g_pref_width;
    out->height      = g_pref_height;
    out->pitch_bytes = g_pref_width * 4;
    out->bpp         = 32;
    out->format      = DISPLAY_FMT_XRGB8888;
    return 0;
}

static int virtio_mode_enumerate(display_mode_t* out, uint32_t max) {
    if (!out || max == 0 || g_pmode_count == 0) return -1;
    uint32_t n = g_pmode_count < max ? g_pmode_count : max;
    for (uint32_t i = 0; i < n; i++) out[i] = g_pmodes[i];
    return (int)n;
}

// VirtIO-GPU v1 tidak punya command guest mode-set; resolusi ditentukan host.
// Ditambah GHAL_CAP_MODE_SET bila kelak ada jalur resize yang aman.
static int virtio_mode_set(const display_mode_t* mode) { (void)mode; return -1; }

const ghal_backend_ops_t virtio_gpu_backend_ops = {
    .name           = "virtio-gpu",
    // GHAL_CAP_HW_CURSOR SENGAJA tidak diiklankan.
    // Plane kursor virtio digambar frontend HOST (GTK/SDL) di luar scanout
    // guest: tidak ikut screendump dan pada frontend non-X11 (mis. Windows)
    // tidak dirender sama sekali. Compositor yang melihat capability ini
    // berhenti menggambar kursor software → kursor hilang dari layar (regresi
    // nyata di QEMU Windows), sementara device melaporkan semuanya OK.
    // Command kursor sendiri sudah benar (§9.4: struct 56 byte, cursorq tanpa
    // response body) dan siap dipakai begitu plane kursor benar-benar terlihat.
    .capabilities   = GHAL_CAP_PARTIAL_FLUSH | GHAL_CAP_ASYNC_PRESENT,
    .init           = virtio_init,
    .shutdown       = virtio_shutdown,
    .surface_create = virtio_surface_create,
    .surface_destroy= virtio_surface_destroy,
    .surface_create_scanout = virtio_surface_create_scanout,
    .surface_upload = virtio_surface_upload,
    .fill_rect      = virtio_fill_rect,
    .blit           = virtio_blit,
    .present        = virtio_present,
    .present_fence  = virtio_present_fence,
    .fence_pending  = virtio_fence_pending,
    .fence_wait     = virtio_fence_wait,
    .cursor_update  = virtio_cursor_update,
    .cursor_move    = virtio_cursor_move,
    .gpu_stats      = virtio_gpu_stats,
    .mode_get       = virtio_mode_get,
    .mode_enumerate = virtio_mode_enumerate,
    .mode_set       = virtio_mode_set,
    .mode_changed   = NULL,   // host resize belum dilaporkan (lihat dokumentasi)
    // Jalur panic/BSOD: BSOD digambar langsung ke backing scanout lalu dikirim
    // dengan try-lock (tanpa tunggu device/lock). NULL pada backend tanpa
    // scanout sendiri = caller fallback ke framebuffer hardware.
    .scanout_map    = virtio_scanout_map,
    .scanout_flush  = virtio_scanout_flush,
};
