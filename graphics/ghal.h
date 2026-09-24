#ifndef GHAL_H
#define GHAL_H

// ============================================================
// KyuzenOS Graphics HAL — kontrak publik (graphics/ghal.h)
//
// Layer ini adalah satu-satunya cara compositor/window manager
// berkomunikasi dengan GPU. Backend (software / virtio-gpu / ...)
// mengimplementasikan ghal_backend_ops_t; HAL memilih backend aktif
// saat ghal_init() dan me-route semua panggilan ke vtable tersebut.
//
// Coding rules (roadmap §5):
//   - ghal_* tidak pernah menyentuh MMIO/register GPU.
//   - compositor tidak pernah memanggil backend secara langsung.
//   - ghal_surface_t adalah OPAQUE: tiap backend mendefinisikan
//     struct konkretnya sendiri di file .c-nya.
//
// API FREEZE (2D project Phase 24) — kontrak stabil, bukan sketsa:
//   - ghal_surface_t = buffer + surface digabung (arsitektur pra-ada,
//     dipertahankan per Rule 3; bukan split gpu_buffer/gpu_surface).
//   - fence = present_fence/fence_pending/fence_wait (HAL) dengan
//     backing internal per-backend (intel_fence_t seqno).
//   - Aturan ubah: append-only (tambah op di akhir struct + NULL-check
//     di dispatch), tanpa reorder field, tanpa bocor tipe Intel ke sini,
//     mirror userlib.h (gpu_stats_t) wajib sinkron — dikunci oleh
//     _Static_assert di kernel/syscall.c.
// ============================================================

#include <stdint.h>
#include "display.h"   // display_mode_t — geometri output kanonik (satu sumber)

// Format piksel. Backend WAJIB handle semua ini, atau menolak
// surface_create dengan error eksplisit (tidak boleh silent fallback).
typedef enum {
    GHAL_FMT_XRGB8888 = 0,   // format native framebuffer software
    GHAL_FMT_ARGB8888 = 1,   // dengan alpha channel (dipakai untuk cursor)
} ghal_format_t;

typedef struct ghal_surface ghal_surface_t;   // opaque — internal per-backend

typedef struct {
    uint32_t x, y, w, h;
} ghal_rect_t;

// Capability flags — compositor query TANPA tahu backend aktif.
#define GHAL_CAP_HW_CURSOR      (1u << 0)   // hardware cursor plane
#define GHAL_CAP_ASYNC_PRESENT  (1u << 1)   // present non-blocking (fence-based)
#define GHAL_CAP_PARTIAL_FLUSH  (1u << 2)   // resource_flush per-rect
#define GHAL_CAP_MODE_SET       (1u << 3)   // backend bisa ganti mode runtime

// Statistik GPU (§9.6). Counter kumulatif sejak boot; konsumen menghitung
// delta per-frame dari dua sampel. Layout ini juga mirror di include/userlib.h
// (gpu_stats_t, ABI syscall 65) — jangan ubah urutan field tanpa sinkron.
typedef struct {
    uint64_t present_count;   // frame present
    uint64_t cmd_count;       // command device terkirim
    uint64_t cmd_bytes;       // byte payload command
    uint64_t notify_count;    // virtq_notify (kick) — metrik batching §9.1
    uint64_t wait_calls;      // panggilan blocking-wait
    uint64_t wait_ticks;      // total waktu blocking (timer ticks)
    uint64_t err_count;       // response error device
} ghal_gpu_stats_t;

typedef struct {
    const char* name;                        // "software" / "virtio-gpu" / "intel"
    uint32_t    capabilities;                // bitmask GHAL_CAP_*

    int  (*init)(void);                      // 0 sukses, <0 gagal
    void (*shutdown)(void);

    ghal_surface_t* (*surface_create)(uint32_t w, uint32_t h, ghal_format_t fmt);
    void            (*surface_destroy)(ghal_surface_t* s);

    // Buat scanout surface (framebuffer utama). Boleh NULL — HAL jatuh ke
    // surface_create biasa. Backend software memakainya untuk membungkus
    // framebuffer hardware langsung (menghindari double-copy).
    ghal_surface_t* (*surface_create_scanout)(uint32_t w, uint32_t h, ghal_format_t fmt);

    // Upload dari system memory (canvas app) ke surface backend.
    // WAJIB ada di semua backend — software backend = memcpy.
    void (*surface_upload)(ghal_surface_t* s, const uint32_t* src,
                           uint32_t src_pitch, ghal_rect_t rect);

    void (*fill_rect)(ghal_surface_t* dst, ghal_rect_t rect, uint32_t argb);
    void (*blit)(ghal_surface_t* dst, ghal_rect_t dst_rect,
                 ghal_surface_t* src, ghal_rect_t src_rect);

    // Present: surface jadi scanout aktif. rect==NULL = full flush.
    // Pada backend dengan GHAL_CAP_ASYNC_PRESENT, present TIDAK blocking —
    // completion diverifikasi lewat fence (ops di bawah, §9.2).
    void (*present)(ghal_surface_t* s, const ghal_rect_t* rect);

    // Fence async present (opsional — WAJIB NULL pada backend tanpa
    // GHAL_CAP_ASYNC_PRESENT, pola fail-fast yang sama dengan cursor ops).
    //   present_fence: fence_id present terakhir (0 = belum ada).
    //   fence_pending: 1 = masih in-flight di device (poll dulu), 0 = selesai.
    //   fence_wait   : block sampai fence selesai (timeout §6.9 driver core).
    uint64_t (*present_fence)(void);
    int      (*fence_pending)(uint64_t fence);
    void     (*fence_wait)(uint64_t fence);

    // Hardware cursor (opsional — cek GHAL_CAP_HW_CURSOR sebelum panggil).
    // Backend tanpa cap ini WAJIB set ke NULL, bukan no-op silent.
    // cursor_update: definisikan gambar kursor dari surface 64x64 ARGB8888
    //   (resource harus sudah di-upload). Return 0 sukses, <0 gagal — caller
    //   (compositor) fallback ke software cursor bila gagal.
    int  (*cursor_update)(ghal_surface_t* cursor_img, int hot_x, int hot_y);
    void (*cursor_move)(int x, int y);

    // Statistik device (Phase 2C §9.6, opsional — NULL bila tidak ada).
    // Return 0 sukses; <0 bila backend tidak menyediakan.
    int (*gpu_stats)(ghal_gpu_stats_t* out);

    // --- Display mode (satu sumber geometri output; tanpa bocor detail device) ---
    // mode_get: mode aktif backend. 0 sukses / <0.
    // mode_enumerate: isi `out` (maks `max`), return jumlah / <0.
    // mode_set: minta ganti mode. Hanya backend dengan GHAL_CAP_MODE_SET + op
    //   non-NULL yang dipanggil; lainnya -1 (boot-fixed, resource lama utuh).
    // mode_changed: opsional, arah backend→kernel saat host mengubah mode
    //   (mis. virtio-gpu scanout berubah). NULL = tidak dilaporkan.
    int  (*mode_get)(display_mode_t* out);
    int  (*mode_enumerate)(display_mode_t* out, uint32_t max);
    int  (*mode_set)(const display_mode_t* mode);
    void (*mode_changed)(void);

    // --- Acceleration info (2D project Phase 15, opsional) ---
    // acceleration_enabled: 1 = fill/blit jalan di HW engine.
    //   NULL = tidak (software / engine tak tersedia / fallback).
    // engine_name: "BCS" / ... NULL = "none".
    // Pola sama seperti cursor/gpu_stats ops: NULL = tidak ada.
    int         (*acceleration_enabled)(void);
    const char* (*engine_name)(void);

    // --- Scanout darurat (jalur panic/BSOD, opsional) ---
    // Jalur panic tidak boleh mengalokasi, mengunci, atau menunggu: backend
    // yang punya scanout sendiri (virtio-gpu: resource milik compositor)
    // menyerahkan memori scanout-nya supaya layar panic digambar LANGSUNG ke
    // sana, lalu dikirim ke device dengan scanout_flush. Backend yang tidak
    // memiliki scanout sendiri (software: framebuffer hardware dipegang
    // compositor) mengembalikan -1 — caller fallback ke framebuffer Limine.
    //   scanout_map  : 0 = *pixels/*width/*height/*pitch_px valid & boleh ditulis
    //                  (pitch dalam PIKSEl).
    //   scanout_flush: kirim rect (x,y,w,h) yang sudah digambar ke device.
    //                  WAJIB non-blocking & tanpa alokasi; 0 = terkirim,
    //                  <0 = dilewati (caller boleh mencoba lagi) — layar panic
    //                  TIDAK pernah boleh menunggu device/lock.
    int (*scanout_map)(uint32_t** pixels, uint32_t* width, uint32_t* height,
                       uint32_t* pitch_px);
    int (*scanout_flush)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // Append-only: acknowledge SUBMISSION, not completion. The legacy void
    // present cannot report queue pressure/failure, so damage owners use this
    // optional op and retain rejected regions. 0 accepted, <0 retry later.
    int (*present_checked)(ghal_surface_t* s, const ghal_rect_t* rect);
} ghal_backend_ops_t;

// --- API publik dipanggil compositor ---
int  ghal_init(void);                        // pilih & init backend
void ghal_shutdown(void);
const char* ghal_active_backend_name(void);
uint32_t    ghal_capabilities(void);

ghal_surface_t* ghal_surface_create(uint32_t w, uint32_t h, ghal_format_t fmt);

// Buat SCANOUT surface (framebuffer utama compositor). Berbeda dari
// surface_create biasa: backend software MEMBUNGKUS framebuffer hardware
// (tanpa buffer perantara — upload menulis langsung ke layar, present no-op),
// sedangkan backend virtio membuat resource + backing seperti biasa.
//
// Ini menghindari double-copy (backbuffer → surface → framebuffer) yang
// membuat present software 2x lebih mahal dari jalur langsung lama.
ghal_surface_t* ghal_surface_create_scanout(uint32_t w, uint32_t h, ghal_format_t fmt);

void            ghal_surface_destroy(ghal_surface_t* s);
void ghal_surface_upload(ghal_surface_t* s, const uint32_t* src,
                         uint32_t src_pitch, ghal_rect_t rect);
void ghal_fill_rect(ghal_surface_t* dst, ghal_rect_t rect, uint32_t argb);
void ghal_blit(ghal_surface_t* dst, ghal_rect_t dst_rect,
               ghal_surface_t* src, ghal_rect_t src_rect);
void ghal_present(ghal_surface_t* s, const ghal_rect_t* rect);
int  ghal_present_checked(ghal_surface_t* s, const ghal_rect_t* rect);

// --- Fence async present (Phase 2C §9.2) ---
// Before writing a new frame, poll ghal_fence_pending(previous frame) and defer
// while pending. A wait may time out: it is never permission to overwrite DMA
// backing without checking pending again. Pada backend
// sync (tanpa GHAL_CAP_ASYNC_PRESENT) semuanya no-op aman.
uint64_t ghal_present_fence(void);            // fence_id present terakhir (0 = tidak ada)
int      ghal_fence_pending(uint64_t fence);  // 1 = masih in-flight
void     ghal_fence_wait(uint64_t fence);     // block sampai selesai

// --- Hardware cursor (Phase 2C §9.4) ---
// ghal_cursor_update: definisikan gambar kursor (surface 64x64 ARGB8888 —
// 0xAARRGGBB per piksel; 0 = transparan). Return 0 sukses, <0 bila backend
// tidak mendukung / gagal — compositor fallback ke software cursor.
// ghal_cursor_move: posisi baru kiri-atas plane kursor (no-op bila image
// belum pernah di-set).
int  ghal_cursor_update(ghal_surface_t* cursor_img, int hot_x, int hot_y);
void ghal_cursor_move(int x, int y);

// --- Statistik GPU (Phase 2C §9.6) ---
// 0 sukses, <0 bila backend tidak menyediakan (software).
int  ghal_gpu_stats(ghal_gpu_stats_t* out);

// Dump statistik ke TTY (shell command `gpu`, awal §9.8). Aman dipanggil
// kapan pun setelah ghal_init.
void ghal_stats_dump(void);

// Diagnostics: pesan error statis dari operasi terakhir yang gagal.
const char* ghal_last_error(void);

// --- Acceleration info (2D project Phase 15) ---
// Backend-agnostic: compositor tak perlu tahu backend aktif.
int         ghal_acceleration_enabled(void);  // 1 bila fill/blit HW aktif
const char* ghal_engine_name(void);           // "BCS" / ... / "none"
// Diagnostik format roadmap (TTY + serial). Aman kapan pun setelah init.
void        ghal_diag_dump(void);

// Ukuran scanout yang dipilih backend aktif (resolusi output). Software backend
// memakai ukuran framebuffer Limine; virtio-gpu memakai pmodes[0]. Compositor
// memakai ini untuk menentukan ukuran main surface.
void ghal_scanout_size(uint32_t* w, uint32_t* h);

// --- Scanout darurat (jalur panic/BSOD) ---
// Kernel panic menggambar layar BSOD TANPA heap, lock, atau IRQ. Pada backend
// yang scanout-nya bukan framebuffer Limine (virtio-gpu: menulis fb_ptr tidak
// pernah terlihat lagi), gambar langsung ke scanout device adalah satu-satunya
// cara BSOD tampil:
//   ghal_scanout_map  : 0 = memori scanout device tersedia & boleh ditulis
//                       langsung (pitch dalam piksel); <0 = tidak ada — caller
//                       fallback ke framebuffer hardware (jalur lama).
//   ghal_scanout_flush: kirim rect yang sudah digambar ke device. 0 = terkirim,
//                       <0 = dilewati (device/lock sedang dipakai CPU lain);
//                       caller mengulanginya di kesempatan berikutnya.
int ghal_scanout_map(uint32_t** pixels, uint32_t* width, uint32_t* height,
                     uint32_t* pitch_px);
int ghal_scanout_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// --- Display mode API (satu jalur, backend-agnostic) ---
// Backend melaporkan geometri lewat display_mode_t; tidak ada detail device
// yang bocor ke atas. display_get_mode()/get_modes()/set_mode() (include/
// display.h) memakai ini di belakang layar.
int ghal_mode_get(display_mode_t* out);                    // 0 / <0
int ghal_mode_enumerate(display_mode_t* out, uint32_t max);// jumlah / <0
int ghal_mode_set(const display_mode_t* mode);             // 0 / <0 unsupported
int ghal_mode_can_set(void);                               // 1 bila backend bisa

// Beri tahu HAL/backend software di mana framebuffer hardware berada.
// WAJIB dipanggil sebelum ghal_init() supaya software backend bisa present.
// (virtio-gpu backend mengabaikan ini — dia punya scanout sendiri.)
void ghal_set_framebuffer(uint32_t* fb, uint32_t width, uint32_t height,
                          uint32_t pitch_bytes);

// Internal: daftarkan backend. Dipanggil dari graphics/select.c sebelum
// ghal_init(). Backend pertama yang init sukses menjadi aktif.
int ghal_register_backend(const ghal_backend_ops_t* ops);

#endif // GHAL_H
