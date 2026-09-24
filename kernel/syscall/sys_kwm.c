#include "syscall.h"
#include <stdint.h>
#include "task.h"
#include "usercopy.h"
#include "userlib.h"   // kyuzen_event_t, kwm_rect_update_t (user ABI)
#include "kwm.h"       // KWM API + kwm_window_info_t (via kwm_abi.h)
#include "gfx.h"       // draw_* primitif + kwm_set_cursor
#include "display.h"   // display_get_mode (sys_get_screen_size)
#include "smap.h"      // user_access_begin/end (pengecualian shared #2)
#include "heap.h"

// pop_event/flush_event_queue (kernel/sync/event.c) tidak punya owner header
// (sync.h hanya mutex/sem/condvar) dan dipakai lintas TU — deklarasi tetap
// lokal di sini; membuat header event.h adalah out-of-scope phase ini.
extern int pop_event(int task_id, kyuzen_event_t* out);
extern void push_event_to(int task_id, uint32_t type, int32_t p1, int32_t p2,
                          int32_t p3, int32_t win_id);

int sys_kwm_handle(registers_t *r, ucopy_ctx_t *uc, uint64_t *ret, task_t *st) {
    (void)st;
    uint64_t syscall_num = r->rax;

    if (syscall_num == 22) { // sys_draw_pixel
        // Phase 3B adopsi: draw_pixel menandai dirty sendiri.
        draw_pixel((uint32_t)r->rbx, (uint32_t)r->rcx, (uint32_t)r->rdx);
    }
    else if (syscall_num == 23) { // sys_draw_image
        // PENGECUALIAN TERDOKUMENTASI #2 (Tahap 2): buffer pixel bisa besar
        // (sampai 64MB) — tidak di-copy; range divalidasi lalu dibaca
        // langsung. Aman: hanya caller yang bisa unmap AS-nya sendiri.
        int w = (int)r->rdx, h = (int)r->rsi;
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096) {
            uint64_t bytes = (uint64_t)w * (uint64_t)h * 4ULL;
            if (user_range_ok(uc, r->rdi, bytes)) {
                // Tahap 4: buffer user dibaca langsung di dalam draw_image
                // (pengecualian shared #2) → jendela SMAP selama blit.
                if (uc->from_user) user_access_begin();
                draw_image((int)r->rbx, (int)r->rcx, w, h, (uint32_t*)r->rdi);
                if (uc->from_user) user_access_end();
            }
        }
    }
    else if (syscall_num == 26) { // sys_draw_string
        // Tahap 2: bukan bagian pengecualian canvas — string kecil, di-copy.
        char kstr[UC_MAX_STR];
        if (strncpy_from_user(uc, kstr, r->rbx, sizeof(kstr)) >= 0) {
            draw_string(kstr, (int)r->rcx, (int)r->rdx, (uint32_t)r->rsi);
        }
    }
    // --- SYSCALL: EVENT QUEUE UNTUK GUI ---
    else if (syscall_num == 29) { // sys_get_event
        // Tahap 2: validasi out-pointer SEBELUM event dikonsumsi (event tidak
        // hilang sia-sia); pop ke buffer kernel, copy-out DI LUAR event_lock.
        if (!user_range_ok(uc, r->rbx, sizeof(kyuzen_event_t))) {
            r->rax = 0;
            return 1;
        }
        kyuzen_event_t kev;

        // Phase 5B: pop dari queue PER-TASK pemanggil — event sudah di-route
        // KWM (keyboard→fokus, mouse/wheel→window di bawah kursor). Fallback
        // sintesis MOUSE_MOVE dihapus: move asli kini terkirim per posisi, dan
        // fallback itu sumber kebocoran event lintas app. Queue kosong →
        // return 0; app tetap polling + sys_yield seperti biasa.
        if (pop_event(smp_current_task_id(), &kev)) {
            copy_to_user(uc, r->rbx, &kev, sizeof(kev));
            r->rax = 1;
            return 1; // Event berhasil diambil dari queue — langsung return
        }
        r->rax = 0;
        return 1;
    }

    // --- SYSCALL BARU UNTUK KWM (Window Manager) ---
    else if (syscall_num == 30) { // sys_kwm_create_window
        *ret = kwm_create_window((int)r->rbx, (int)r->rcx, (uint32_t)r->rdx, (uint32_t)r->rsi);
    }
    else if (syscall_num == 31) { // sys_kwm_update_window
        // PENGECUALIAN TERDOKUMENTASI #1 (FIX_004 + Tahap 2): canvas sampai
        // 16MB/frame — copy per frame mahal, tetap SHARED. Tapi: range source
        // divalidasi di sini, dan owner divalidasi di kwm_update_window.
        uint64_t cbytes = kwm_window_canvas_bytes((int)r->rbx);
        if (cbytes > 0 && user_range_ok(uc, r->rcx, cbytes)) {
            kwm_update_window((int)r->rbx, (uint32_t*)r->rcx);
        }
    }
    else if (syscall_num == 32) { // sys_kwm_destroy_window
        int wid = (int)r->rbx;
        // FIX_004: app hanya boleh menghancurkan window miliknya sendiri.
        if (kwm_window_owner(wid) == smp_current_task_id()) {
            kwm_destroy_window(wid);
        }
    }
    else if (syscall_num == 58) { // sys_kwm_set_cursor — Phase 9: bentuk kursor
        int kind = (int)r->rbx;
        if (kind < 0 || kind > 2) {
            *ret = (uint64_t)-1;
        } else {
            kwm_set_cursor(kind);
            *ret = 0;
        }
    }
    // ============================================================
    // Phase 10 — Desktop window + taskbar (syscall 59-63)
    // ============================================================
    else if (syscall_num == 59) { // sys_kwm_create_desktop
        *ret = (uint64_t)kwm_create_desktop();
    }
    else if (syscall_num == 60) { // sys_kwm_set_title
        char ktitle[32];
        if (strncpy_from_user(uc, ktitle, r->rcx, sizeof(ktitle)) >= 0)
            *ret = (uint64_t)kwm_set_title((int)r->rbx, ktitle);
        else
            *ret = (uint64_t)-1;
    }
    else if (syscall_num == 61) { // sys_kwm_get_windows — enum utk taskbar
        int maxn = (int)r->rcx;
        if (maxn > 16) maxn = 16;
        uint64_t bytes = (uint64_t)maxn * sizeof(kwm_window_info_t);
        if (maxn > 0 && user_range_ok(uc, r->rbx, bytes)) {
            kwm_window_info_t* bounce =
                (kwm_window_info_t*)kmalloc((uint32_t)bytes);
            if (bounce) {
                int count = kwm_get_windows(bounce, maxn);
                if (count > 0 && copy_to_user(uc, r->rbx, bounce,
                        (uint64_t)count * sizeof(kwm_window_info_t)) != 0)
                    count = 0;
                *ret = (uint64_t)count;
                kfree(bounce);
            }
        }
    }
    else if (syscall_num == 62) { // sys_kwm_activate_window — klik taskbar
        *ret = (uint64_t)kwm_activate_window((int)r->rbx);
    }
    else if (syscall_num == 63) { // sys_get_screen_size(uint32_t* w, uint32_t* h)
        // DUA pointer terpisah (rbx=w, rcx=h) — bukan satu array 2 elemen.
        // Menulis 8 byte ke rbx dulu menimpa 4 byte SETELAH variabel w milik
        // app (di settings.c itu variabel lain di stack → pointer widget
        // rusak → #PF). copy_to_user memvalidasi range sendiri.
        // Sumber: mode display authoritative (bukan global fb_*).
        const display_mode_t* m = display_get_mode();
        if (!m) {
            *ret = (uint64_t)-1;
        } else {
            uint32_t w = m->width, h = m->height;
            *ret = (copy_to_user(uc, r->rbx, &w, 4) == 0 &&
                       copy_to_user(uc, r->rcx, &h, 4) == 0) ? 0 : (uint64_t)-1;
        }
    }
    // ============================================================
    // Phase 3 — Partial window update (syscall 66)
    // ============================================================
    else if (syscall_num == 66) { // sys_kwm_update_window_rect(req)
        // req = kwm_rect_update_t di user-space. Copy-in bounded dulu, lalu
        // validasi independen: ownership/batas rect di KWM, dan rentang
        // buffer user untuk span baris yang benar-benar diakses.
        kwm_rect_update_t req;
        if (copy_from_user(uc, &req, r->rbx, sizeof(req)) != 0) {
            *ret = (uint64_t)-1;
        } else if (req.win_id < 0 || req.win_id >= 16 ||
                   req.x < 0 || req.y < 0 ||
                   req.width == 0 || req.height == 0) {
            *ret = (uint64_t)-1;
        } else {
            uint32_t win_w = 0, win_h = 0;
            if (kwm_window_dims(req.win_id, &win_w, &win_h) != 0 ||
                (uint64_t)req.x + (uint64_t)req.width > (uint64_t)win_w ||
                (uint64_t)req.y + (uint64_t)req.height > (uint64_t)win_h) {
                *ret = (uint64_t)-1;
            } else {
                // Span minimum yang mencakup SEMUA byte rect di canvas penuh:
                //   stride(byte) = win_w * 4
                //   span        = (height-1)*stride + width*4
                // Bukan width*height*4: rect tidak kontigu (stride penuh).
                // Semua operand <= 4096 → span <= ~64 MiB, aman di u64.
                uint64_t stride = (uint64_t)win_w * 4u;
                uint64_t span = (uint64_t)(req.height - 1) * stride
                              + (uint64_t)req.width * 4u;
                uint64_t ustart = (uint64_t)req.buffer
                                + (uint64_t)req.y * stride
                                + (uint64_t)req.x * 4u;
                if (!user_range_ok(uc, ustart, span)) {
                    *ret = (uint64_t)-1;
                } else {
                    *ret = (uint64_t)kwm_update_window_rect(
                        req.win_id, req.x, req.y,
                        req.width, req.height, req.buffer);
                }
            }
        }
    }

    // ============================================================
    // Phase 14 — Deklarasi window 100% opaque (syscall 67)
    // Owner-only; kernel memvalidasi seluruh canvas (alpha != 0) sebelum
    // menandai. Hanya mengaktifkan fast-path memcpy compositor.
    // ============================================================
    else if (syscall_num == 67) { // sys_kwm_set_window_opaque(win_id)
        *ret = (uint64_t)kwm_set_window_opaque((int)r->rbx);
    }
    // ============================================================
    // sys_wallpaper_reload (syscall 84, SYS_WALLPAPER_RELOAD di kwm_abi.h).
    // Minta Desktop yang berjalan memuat ulang wallpaper dari konfigurasi
    // persisten. Tanpa argumen (tanpa path user): kernel hanya mengantar
    // SATU event bertipe tetap ke task pemilik window desktop; decode,
    // render, dan damage terjadi di loop normal Desktop (bukan di sini).
    // Return 0 = diterima, -1 = Desktop tak tersedia. Boleh dipanggil task
    // mana pun — tak ada pointer user, framebuffer, atau memori desktop yang
    // tersentuh; reload memakai konfigurasi persisten milik Desktop sendiri.
    // ============================================================
    else if (syscall_num == SYS_WALLPAPER_RELOAD) {
        int owner = kwm_desktop_owner();
        if (owner < 0) {
            *ret = (uint64_t)-1;
        } else {
            push_event_to(owner, EVENT_WALLPAPER_RELOAD, 0, 0, 0, 0);
            *ret = 0;
        }
    }

    return 0;
}
