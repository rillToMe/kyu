#include <stdint.h>
#include <stddef.h>
#include "kwm.h"
#include "kwm_internal.h"
#include "gfx.h"
#include "heap.h"
#include "task.h"
#include "spinlock.h"
#include "smap.h"
#include "string.h"   // memcpy — Phase 3 partial window copy

// --- KYUZEN WINDOW MANAGER (KWM) ---
// FIX_004: batas dimensi/ukuran canvas — w*h*4 dari app tidak boleh wrap
// 32-bit (alokasi kecil untuk "canvas raksasa") atau menguras heap.
#define KWM_MAX_DIMENSION    4096U
#define KWM_MAX_CANVAS_BYTES (16U * 1024U * 1024U)

// Phase 5C: tombol close titlebar → EVENT_WIN_CLOSE (6) ke owner task; app
// yang cleanup lalu sys_exit. Nilai harus cocok dengan event.c + userlib.h.
#define EVENT_WIN_CLOSE 6
extern void push_event_to(int task_id, uint32_t type, int32_t p1, int32_t p2,
                          int32_t p3, int32_t win_id);

kwm_window_t kwm_windows[MAX_WINDOWS];
uint32_t next_z_index = 1;
spinlock_t kwm_lock = SPINLOCK_INIT;

// Re-normalisasi z-index (dipanggil dengan kwm_lock DIpegang). Bug 5.3/5.4:
// next_z_index naik monoton tak terbatas → scan compositor O(next_z_index)
// makin lambat, dan bisa wrap (uint32) bentrok dengan desktop z=0. Compact
// z semua window aktif ke 0..N-1 (desktop tetap 0) sambil MEMPERTAHANKAN urutan
// z yang ada (window ber-z terbesar tetap paling atas), lalu reset next_z_index.
static void kwm_normalize_zindex_locked(void) {
    // Urutkan window aktif non-desktop berdasarkan z_index naik (insertion sort
    // — MAX_WINDOWS kecil, O(N²) aman).
    int order[MAX_WINDOWS];
    int n = 0;
    for (int w = 0; w < MAX_WINDOWS; w++) {
        if (kwm_windows[w].active && !(kwm_windows[w].flags & KWM_WIN_DESKTOP))
            order[n++] = w;
    }
    for (int i = 1; i < n; i++) {
        int w = order[i];
        uint32_t z = kwm_windows[w].z_index;
        int j = i - 1;
        while (j >= 0 && kwm_windows[order[j]].z_index > z) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = w;
    }
    for (int i = 0; i < n; i++)
        kwm_windows[order[i]].z_index = (uint32_t)(i + 1);
    next_z_index = (uint32_t)(n + 1);
}

// State global untuk drag session yang sedang aktif
static int     dragged_win_id = -1;  // -1 = tidak ada drag
static int32_t drag_offset_x  = 0;   // Offset klik dalam window (mencegah window "loncat")
static int32_t drag_offset_y  = 0;

// Phase 5B/5C: window pemegang fokus keyboard (-1 = tidak ada → keyboard ke
// TTY). Di-set saat create window & click-to-focus di kwm_process_mouse;
// dibaca compositor.c untuk tint titlebar (fokus vs tidak).
int focused_win_id = -1;

// Window (+1; 0 = tidak ada) yang kursornya di atas tombol close titlebar.
// Dipakai compositor untuk latar close merah saat hover; di-set di
// kwm_process_mouse (tiap event mouse, bukan hanya klik).
int hovered_close_win = 0;

// Frame + margin drop-shadow (compositor menggambar shadow di luar frame).
// Semua repaint frame HARUS lewat sini agar sisa shadow tidak tertinggal.
//
// BUGFIX (Phase 4): shadow di compositor digeser +3px ke bawah (frame_shadow:
// s.y = frame.y - k + 3). Ring terluar (k = KWM_SHADOW_MARGIN) jatuh di baris
// bawah y + h + KWM_SHADOW_MARGIN + 2, sedangkan margin seragam hanya sampai
// y + h + KWM_SHADOW_MARGIN - 1 → 3 baris bawah tidak ter-invalidate dan sisa
// shadow tertinggal saat window pindah. Rentang dirty wajib mencakup bounding
// box visual penuh (frame + shadow), bukan hanya frame + margin simetris.
static void frame_dirty_area(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    int32_t m = KWM_SHADOW_MARGIN;
    screen_mark_dirty(x - m, y - m, w + 2 * (uint32_t)m,
                      h + 2 * (uint32_t)m + 3);
}

// Caller MUST TIDAK memegang kwm_lock (dipanggil setelah unlock). Menandai area
// layar yang ditempati frame window (konten + titlebar) sebagai dirty.
// Dipanggil setiap kali penampilan window berubah di layar (create/move/destroy/fokus).
// Bug 5.2: field window dimutasi di bawah kwm_lock, jadi baca di sini juga harus
// di bawah kwm_lock — menghilangkan data race pada dirty-rect.
static void kwm_frame_dirty(int i) {
    if (i < 0 || i >= MAX_WINDOWS) return;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if (!kwm_windows[i].active) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return;
    }
    // Phase 10: desktop frameless — frame = konten saja (tanpa titlebar).
    uint32_t tb = (kwm_windows[i].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
    int32_t  x  = kwm_windows[i].x;
    int32_t  y  = kwm_windows[i].y;
    uint32_t w  = kwm_windows[i].width;
    uint32_t h  = kwm_windows[i].height + tb;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    frame_dirty_area(x, y, w, h);
}

// Caller MUST hold kwm_lock. Window aktif teratas (z tertinggi) di titik
// (px, py), atau -1 jika tidak ada.
static int kwm_hit_test_locked(int32_t px, int32_t py) {
    int highest_z = -1;
    int target    = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!kwm_windows[i].active) continue;
        int32_t wx = kwm_windows[i].x;
        int32_t wy = kwm_windows[i].y;
        int32_t ww = (int32_t)kwm_windows[i].width;
        // Phase 5C: frame = konten + titlebar di atasnya.
        // Phase 10: desktop frameless — frame = konten saja.
        int32_t tb = (kwm_windows[i].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
        int32_t wh = (int32_t)kwm_windows[i].height + tb;
        if (px >= wx && px < wx + ww && py >= wy && py < wy + wh) {
            if ((int)kwm_windows[i].z_index > highest_z) {
                highest_z = (int)kwm_windows[i].z_index;
                target    = i;
            }
        }
    }
    return target;
}

// Caller MUST hold kwm_lock. Setelah window fokus hancur, pindahkan fokus ke
// window aktif dengan z tertinggi (-1 jika tidak ada lagi).
static void kwm_refocus_locked(void) {
    int best = -1;
    uint32_t best_z = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        // Phase 10: desktop tak pernah jadi fokus keyboard.
        if (kwm_windows[i].active && !(kwm_windows[i].flags & KWM_WIN_DESKTOP) &&
            kwm_windows[i].z_index > best_z) {
            best_z = kwm_windows[i].z_index;
            best   = i;
        }
    }
    focused_win_id = best;
}

// Caller MUST hold kwm_lock. FIX_004: canvas di-NULL-kan setelah free dan
// drag session ke slot ini diputus — tidak ada pointer/state menggantung.
static void kwm_free_slot(int i) {
    if (kwm_windows[i].canvas) {
        display_buffer_destroy(kwm_windows[i].canvas);
        kwm_windows[i].canvas = NULL;
    }
    kwm_windows[i].active = 0;
    kwm_windows[i].owner_task = -1;
    kwm_windows[i].flags = 0;
    kwm_windows[i].fully_opaque = 0;   // Phase 14: slot bebas = tidak dijamin opaque
    kwm_windows[i].title[0] = '\0';
    if (dragged_win_id == i) dragged_win_id = -1;
    if (hovered_close_win == i + 1) hovered_close_win = 0;
}

int kwm_create_window(int x, int y, uint32_t width, uint32_t height) {
    // FIX_004: validasi dulu. Ukuran dihitung 64-bit agar tidak wrap.
    if (width == 0 || height == 0 ||
        width > KWM_MAX_DIMENSION || height > KWM_MAX_DIMENSION) {
        return -1;
    }
    uint64_t bytes = (uint64_t)width * (uint64_t)height * 4ULL;
    if (bytes > (uint64_t)KWM_MAX_CANVAS_BYTES) return -1;

    int owner = smp_current_task_id();

    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    for(int i = 0; i < MAX_WINDOWS; i++) {
        if(!kwm_windows[i].active) {
            // FIX_004: alokasi DULU — slot ditandai active hanya setelah
            // semua field siap (tidak ada zombie window saat alokasi gagal).
            // Phase 3A: surface window = DisplayBuffer owned.
            DisplayBuffer* canvas =
                display_buffer_create(width, height, COLOR_FORMAT_XRGB8888);
            if (!canvas) {
                spinlock_unlock_irqrestore(&kwm_lock, flags);
                return -1;
            }
            kwm_windows[i].x = x;
            kwm_windows[i].y = y;
            kwm_windows[i].width = width;
            kwm_windows[i].height = height;
            kwm_windows[i].canvas = canvas;
            kwm_windows[i].owner_task = owner;
            kwm_windows[i].z_index = next_z_index++;
            if (next_z_index > MAX_WINDOWS) kwm_normalize_zindex_locked();
            kwm_windows[i].flags = 0;
            kwm_windows[i].fully_opaque = 0;   // Phase 14: default aman — jalur scalar
            kwm_windows[i].title[0] = '\0';
            kwm_windows[i].active = 1;
            // Phase 5B/5C: window baru memegang fokus — yang lama kehilangan
            // tint titlebar-nya.
            int prev_focus = focused_win_id;
            focused_win_id = i;
            int32_t ctx = kwm_windows[i].x;
            int32_t cty = kwm_windows[i].y + KWM_TITLEBAR_H;
            uint32_t cw = kwm_windows[i].width;
            uint32_t ch = kwm_windows[i].height;
            spinlock_unlock_irqrestore(&kwm_lock, flags);
            kwm_frame_dirty(i);
            // BUGFIX (Phase 4): jamin area KONTEN ter-invalidate sejak frame
            // pertama — window baru tidak boleh muncul tanpa dirty region yang
            // menutupi badannya, terlepas dari upload pertama app (frame_dirty
            // sudah mencakupnya; ini mengunci kontrak + menutup race bila flush
            // compositor jatuh di antara create dan upload pertama app).
            screen_mark_dirty(ctx, cty, cw, ch);
            if (prev_focus >= 0 && prev_focus != i) kwm_frame_dirty(prev_focus);
            return i;
        }
    }
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return -1;
}

// ============================================================
// Phase 10 — Desktop window (full-screen, frameless, z=0, no-focus)
// ============================================================

// Buat window desktop. Hanya SATU yang boleh ada. Owner = caller.
// z=0 (paling bawah), ukuran = layar penuh, tanpa titlebar/close,
// klik tidak refokus (tetap diteruskan ke app utk ikon/taskbar).
int kwm_create_desktop(void) {
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (kwm_windows[i].active && (kwm_windows[i].flags & KWM_WIN_DESKTOP)) {
            spinlock_unlock_irqrestore(&kwm_lock, flags);
            return -1;   // desktop sudah ada
        }
    if (fb_width == 0 || fb_height == 0) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!kwm_windows[i].active) {
            DisplayBuffer* canvas =
                display_buffer_create(fb_width, fb_height, COLOR_FORMAT_XRGB8888);
            if (!canvas) {
                spinlock_unlock_irqrestore(&kwm_lock, flags);
                return -1;
            }
            kwm_windows[i].x = 0;
            kwm_windows[i].y = 0;
            kwm_windows[i].width = fb_width;
            kwm_windows[i].height = fb_height;
            kwm_windows[i].canvas = canvas;
            kwm_windows[i].owner_task = smp_current_task_id();
            kwm_windows[i].z_index = 0;          // selalu paling bawah
            kwm_windows[i].flags = KWM_WIN_DESKTOP;
            kwm_windows[i].fully_opaque = 0;   // Phase 14: desktop app tidak mendeklarasikan
            kwm_windows[i].title[0] = '\0';
            kwm_windows[i].active = 1;
            spinlock_unlock_irqrestore(&kwm_lock, flags);
            screen_mark_dirty(0, 0, fb_width, fb_height);
            return i;
        }
    }
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return -1;
}

// Set judul window (titlebar + taskbar). Hanya pemilik. -1 = gagal.
int kwm_set_title(int win_id, const char* title) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !title) return -1;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if (!kwm_windows[win_id].active ||
        kwm_windows[win_id].owner_task != smp_current_task_id()) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }
    int n = 0;
    while (title[n] && n < (int)sizeof(kwm_windows[win_id].title) - 1) n++;
    for (int i = 0; i < n; i++) kwm_windows[win_id].title[i] = title[i];
    kwm_windows[win_id].title[n] = '\0';
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    kwm_frame_dirty(win_id);   // titlebar digambar compositor → repaint frame
    return 0;
}

// Phase 14 — tandai window sebagai 100% opaque (fast-path memcpy compositor).
// Hanya PEMILIK window yang boleh. Kernel MEMVALIDASI seluruh canvas: setiap
// piksel harus punya alpha != 0. Bila ada satu saja piksel alpha == 0 →
// TOLAK, jangan set flag (false negative aman; false positive = korupsi visual
// karena compositor akan memcpy piksel transparan menimpa background).
// Dipanggil platform Rust/Slint setelah frame pertama (canvas sudah terisi).
// CATATAN: upload parsial SESUDAH ini tidak divalidasi ulang — aplikasi
// kooperatif (Rust/Slint, diaudit Phase 13) tidak pernah menulis alpha == 0.
// Return 0 sukses, -1 ditolak.
int kwm_set_window_opaque(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return -1;

    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if (!kwm_windows[win_id].active || !kwm_windows[win_id].canvas ||
        kwm_windows[win_id].owner_task != smp_current_task_id()) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }
    const uint32_t* px = kwm_windows[win_id].canvas->pixels;
    uint32_t n = kwm_windows[win_id].canvas->width * kwm_windows[win_id].canvas->height;
    // Scan di LUAR lock: canvas bisa besar (mis. full-screen); menahan kwm_lock
    // (spinlock, IRQ-off) selama scan memblok timer/compositor. Race benign —
    // hanya owner (caller) yang menulis canvas ini.
    spinlock_unlock_irqrestore(&kwm_lock, flags);

    for (uint32_t i = 0; i < n; i++) {
        if ((px[i] >> 24) == 0) return -1;   // ada piksel transparan → bukan opaque
    }

    flags = spinlock_lock_irqsave(&kwm_lock);
    if (kwm_windows[win_id].active &&
        kwm_windows[win_id].owner_task == smp_current_task_id()) {
        kwm_windows[win_id].fully_opaque = 1;
    }
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return 0;
}

// Isi buffer dgn info window aktif (pemakaian syscall 61, pola kfs_get_file_list).
// Return jumlah window aktif, atau -1. Caller menyediakan buf berkapasitas max.
int kwm_get_windows(kwm_window_info_t* buf, int max) {
    if (!buf || max <= 0) return -1;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
        if (!kwm_windows[i].active) continue;
        kwm_window_info_t* o = &buf[n];
        o->win_id     = (uint32_t)i + 1;
        o->active     = 1;
        o->focused    = (i == focused_win_id) ? 1 : 0;
        o->x          = kwm_windows[i].x;
        o->y          = kwm_windows[i].y;
        o->width      = kwm_windows[i].width;
        o->height     = kwm_windows[i].height;
        o->z_index    = kwm_windows[i].z_index;
        o->owner_task = kwm_windows[i].owner_task;
        o->flags      = kwm_windows[i].flags;
        for (int j = 0; j < 32; j++) {
            o->title[j] = kwm_windows[i].title[j];
            if (!kwm_windows[i].title[j]) break;
        }
        n++;
    }
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return n;
}

// Bawa window ke depan + beri fokus (klik taskbar). Desktop ditolak.
int kwm_activate_window(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return -1;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if (!kwm_windows[win_id].active ||
        (kwm_windows[win_id].flags & KWM_WIN_DESKTOP)) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }
    int old_focus = focused_win_id;
            kwm_windows[win_id].z_index = next_z_index++;
            if (next_z_index > MAX_WINDOWS) kwm_normalize_zindex_locked();   // bring-to-front
            if (next_z_index > MAX_WINDOWS) kwm_normalize_zindex_locked();
    focused_win_id = win_id;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    kwm_frame_dirty(win_id);
    if (old_focus >= 0 && old_focus != win_id) kwm_frame_dirty(old_focus);
    return 0;
}

// Ukuran canvas window dalam byte (0 jika slot kosong/id invalid).
// Ukuran fix sejak create — aman dipakai syscall 31 untuk validasi range
// buffer app SEBELUM kwm_update_window (FIX_005 Tahap 2).
uint64_t kwm_window_canvas_bytes(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return 0;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    uint64_t bytes = kwm_windows[win_id].active
        ? (uint64_t)kwm_windows[win_id].width * (uint64_t)kwm_windows[win_id].height * 4ULL
        : 0;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return bytes;
}

// Owner task dari sebuah window (-1 jika slot kosong/id invalid).
int kwm_window_owner(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return -1;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    int owner = kwm_windows[win_id].active ? kwm_windows[win_id].owner_task : -1;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return owner;
}

void kwm_update_window(int win_id, uint32_t* app_buffer) {
    if(win_id < 0 || win_id >= MAX_WINDOWS) return;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if(!kwm_windows[win_id].active || !kwm_windows[win_id].canvas || !app_buffer) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return;
    }
    // FIX_004: hanya pemilik yang boleh menulis canvas-nya.
    if (kwm_windows[win_id].owner_task != smp_current_task_id()) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return;
    }

    uint32_t size = kwm_windows[win_id].width * kwm_windows[win_id].height;
    uint32_t* dest = kwm_windows[win_id].canvas->pixels;  // stride == width
    // FIX_005 Tahap 4: app_buffer bisa halaman user (pengecualian shared #1,
    // dibaca langsung dengan CR3 caller) → jendela SMAP selama blit.
    user_access_begin();
    __asm__ volatile ("rep movsl" : "+D" (dest), "+S" (app_buffer), "+c" (size) : : "memory");
    user_access_end();
    // Phase 5C: konten murni di bawah titlebar — hanya rect konten yang
    // berubah (titlebar digambar compositor, tidak ikut update).
    // Phase 10: desktop frameless — konten mulai dari y window.
    int32_t tb = (kwm_windows[win_id].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
    int32_t mx = kwm_windows[win_id].x;
    int32_t my = kwm_windows[win_id].y + tb;
    uint32_t mw = kwm_windows[win_id].width, mh = kwm_windows[win_id].height;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    screen_mark_dirty(mx, my, mw, mh);
}

// Phase 3 — geometri konten window. Return 0 sukses (out_w/out_h terisi),
// -1 jika slot invalid/kosong. Caller menyediakan dua pointer out.
int kwm_window_dims(int win_id, uint32_t* out_w, uint32_t* out_h) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !out_w || !out_h) return -1;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if (!kwm_windows[win_id].active) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }
    *out_w = kwm_windows[win_id].width;
    *out_h = kwm_windows[win_id].height;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    return 0;
}

// Phase 3 — partial window update. app_buffer adalah canvas PENUH milik app
// (stride = window_width*4); rect (x,y,width,height) adalah koordinat konten
// window-local. Hanya baris rect yang disalin dari canvas app ke surface KWM,
// baris demi baris (rect tidak kontigu di canvas penuh).
//
// Validasi di sini bersifat defense-in-depth: syscall 66 sudah memvalidasi
// rentang buffer user + batas rect, tetapi KWM tetap memvalidasi ownership dan
// batas rect sendiri (jangan pernah percaya user-space).
int kwm_update_window_rect(int win_id, int32_t x, int32_t y,
                           uint32_t width, uint32_t height, uint32_t* app_buffer) {
    if (win_id < 0 || win_id >= MAX_WINDOWS || !app_buffer) return -1;
    if (x < 0 || y < 0 || width == 0 || height == 0) return -1;

    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if (!kwm_windows[win_id].active || !kwm_windows[win_id].canvas) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }
    // FIX_004: hanya pemilik yang boleh menulis canvas-nya.
    if (kwm_windows[win_id].owner_task != smp_current_task_id()) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }

    uint64_t win_w = kwm_windows[win_id].width;
    uint64_t win_h = kwm_windows[win_id].height;
    // Overflow-safe: x,y >= 0 (checked) dan width/height > 0; semua dihitung
    // 64-bit sebelum dibandingkan dengan dimensi window.
    if ((uint64_t)x + (uint64_t)width > win_w ||
        (uint64_t)y + (uint64_t)height > win_h) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return -1;
    }

    // stride == window width (dalam elemen u32) untuk source DAN dest.
    uint32_t* dst = kwm_windows[win_id].canvas->pixels;
    uint32_t* src = app_buffer;
    uint64_t start = (uint64_t)y * win_w + (uint64_t)x;
    uint64_t row_bytes = (uint64_t)width * 4u;

    // FIX_005 Tahap 4: source adalah halaman user (US=1) → jendela SMAP
    // selama copy. Satu jendela untuk seluruh rect, selalu di-close.
    user_access_begin();
    for (uint64_t row = 0; row < height; row++) {
        uint64_t off = start + row * win_w;
        memcpy(&dst[off], &src[off], (size_t)row_bytes);
    }
    user_access_end();

    int32_t tb = (kwm_windows[win_id].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
    int32_t mx = kwm_windows[win_id].x + x;
    int32_t my = kwm_windows[win_id].y + tb + y;
    spinlock_unlock_irqrestore(&kwm_lock, flags);

    // Hanya rect ini yang ditandai dirty (bukan seluruh window).
    screen_mark_dirty(mx, my, width, height);
    return 0;
}

void kwm_destroy_window(int win_id) {
    if(win_id < 0 || win_id >= MAX_WINDOWS) return;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    if(!kwm_windows[win_id].active) {
        spinlock_unlock_irqrestore(&kwm_lock, flags);
        return;
    }
    int32_t mx = kwm_windows[win_id].x, my = kwm_windows[win_id].y;
    uint32_t mw = kwm_windows[win_id].width, mh = kwm_windows[win_id].height;
    int was_focused = (focused_win_id == win_id);
    int new_focus = focused_win_id;
    kwm_free_slot(win_id);
    if (was_focused) {
        kwm_refocus_locked();
        new_focus = focused_win_id;   // Phase 5C: refokus → tint titlebar baru
    }
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    // Frame penuh (konten + titlebar) harus direpaint — window hilang.
    frame_dirty_area(mx, my, mw, mh + KWM_TITLEBAR_H);
    if (was_focused && new_focus >= 0) kwm_frame_dirty(new_focus);
}

// FIX_004: destroy HANYA window milik task_id — dipanggil saat app exit/exec
// menggantikan kwm_destroy_all_windows() agar lifecycle satu task tidak
// menghancurkan window task lain.
void kwm_destroy_windows_of(int task_id) {
    // Phase 6: invalidasikan HANYA visual bounds window yang hancur, bukan
    // seluruh layar. Bounding box semua window yang hancur diakumulasi di
    // dalam kwm_lock (hanya 4 int, TANPA array rect — stack syscall sempit),
    // lalu ditandai SETELAH unlock (screen_mark_dirty tidak boleh diambil di
    // bawah kwm_lock). Compositor merekonstruksi area terekspos dari
    // base_canvas + window yang tersisa. Bounds = frame + titlebar + shadow
    // (margin sama dengan frame_dirty_area).
    const int32_t m = KWM_SHADOW_MARGIN;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    int focus_destroyed = 0, any = 0;
    int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (kwm_windows[i].active && kwm_windows[i].owner_task == task_id) {
            uint32_t tb = (kwm_windows[i].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
            int32_t  x0 = kwm_windows[i].x - m;
            int32_t  y0 = kwm_windows[i].y - m;
            int32_t  x1 = kwm_windows[i].x + (int32_t)kwm_windows[i].width + m;
            int32_t  y1 = kwm_windows[i].y + (int32_t)(kwm_windows[i].height + tb) + m + 3;
            if (!any) { bx0 = x0; by0 = y0; bx1 = x1; by1 = y1; any = 1; }
            else {
                if (x0 < bx0) bx0 = x0;
                if (y0 < by0) by0 = y0;
                if (x1 > bx1) bx1 = x1;
                if (y1 > by1) by1 = y1;
            }
            if (focused_win_id == i) focus_destroyed = 1;
            kwm_free_slot(i);
        }
    }
    // Phase 5B: fokus hanya pindah jika window fokus ikut hancur — fokus
    // kosong yang disengaja (klik desktop) tidak boleh dicuri ulang.
    if (focus_destroyed) kwm_refocus_locked();
    int new_focus = focused_win_id;
    spinlock_unlock_irqrestore(&kwm_lock, flags);

    if (any && bx1 > bx0 && by1 > by0)
        screen_mark_dirty(bx0, by0, (uint32_t)(bx1 - bx0), (uint32_t)(by1 - by0));
    // Window yang mewarisi fokus berubah tint titlebar → repaint frame-nya.
    if (focus_destroyed && new_focus >= 0) kwm_frame_dirty(new_focus);
}

// Destroy ALL KWM windows — hanya untuk path kernel/test, BUKAN syscall.
// Tanpa ini, compositor akan membaca memori bebas saat render.
void kwm_destroy_all_windows(void) {
    // Phase 6: sama seperti kwm_destroy_windows_of — bounding box visual bounds
    // window yang hancur, ditandai setelah unlock. (Desktop full-screen, bila
    // ikut hancur, memang menutupi seluruh layar — hasilnya tetap benar.)
    const int32_t m = KWM_SHADOW_MARGIN;
    uint64_t flags = spinlock_lock_irqsave(&kwm_lock);
    int any = 0;
    int32_t bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (kwm_windows[i].active) {
            uint32_t tb = (kwm_windows[i].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
            int32_t  x0 = kwm_windows[i].x - m;
            int32_t  y0 = kwm_windows[i].y - m;
            int32_t  x1 = kwm_windows[i].x + (int32_t)kwm_windows[i].width + m;
            int32_t  y1 = kwm_windows[i].y + (int32_t)(kwm_windows[i].height + tb) + m + 3;
            if (!any) { bx0 = x0; by0 = y0; bx1 = x1; by1 = y1; any = 1; }
            else {
                if (x0 < bx0) bx0 = x0;
                if (y0 < by0) by0 = y0;
                if (x1 > bx1) bx1 = x1;
                if (y1 > by1) by1 = y1;
            }
            kwm_free_slot(i);
        }
    }
    focused_win_id = -1;
    spinlock_unlock_irqrestore(&kwm_lock, flags);
    if (any && bx1 > bx0 && by1 > by0)
        screen_mark_dirty(bx0, by0, (uint32_t)(bx1 - bx0), (uint32_t)(by1 - by0));
}

// ============================================================
// KWM V2 — Drag & Drop + Z-Index Dinamis
// ============================================================

// Bawa window ke depan (Z-index tertinggi)
// Dipanggil saat user klik pada window manapun.
static void kwm_bring_to_front(int win_id) {
    if (win_id < 0 || win_id >= MAX_WINDOWS) return;
    // Caller MUST hold kwm_lock
    kwm_windows[win_id].z_index = next_z_index++;
}

// Intercept mouse event sebelum dikirim ke user-space.
//
// Dipanggil dari mouse_handler() SEBELUM push_event().
// Return: 1 = event "dimakan" oleh KWM (jangan kirim ke app)
//         0 = teruskan event ke app seperti biasa
//
// Params:
//   mouse_px, mouse_py  = posisi kursor saat ini
//   left_down = 1 saat tombol kiri baru ditekan (edge detect)
//   left_up   = 1 saat tombol kiri baru dilepas (edge detect)
int kwm_process_mouse(int32_t mouse_px, int32_t mouse_py,
                      uint8_t left_down, uint8_t left_up) {

    // 0. Hover tombol close — diperbarui tiap event (gerak & klik), bukan hanya
    //    saat klik, agar chrome bisa memerah tanpa menunggu tombol ditekan.
    //    Window di-repaint hanya ketika status hover benar-benar berubah.
    {
        spinlock_lock(&kwm_lock);
        int hov = 0;
        int t = kwm_hit_test_locked(mouse_px, mouse_py);
        if (t >= 0 && !(kwm_windows[t].flags & KWM_WIN_DESKTOP) &&
            mouse_py < kwm_windows[t].y + KWM_TITLEBAR_H &&
            mouse_px >= kwm_windows[t].x + (int32_t)kwm_windows[t].width -
                         KWM_CLOSE_BTN_W) {
            hov = t + 1;
        }
        int prev = hovered_close_win;
        hovered_close_win = hov;
        spinlock_unlock(&kwm_lock);
        if (prev != hov) {
            if (prev > 0) kwm_frame_dirty(prev - 1);
            if (hov  > 0) kwm_frame_dirty(hov  - 1);
        }
    }

    // 1. Mouse Up — akhiri drag session
    if (left_up) {
        spinlock_lock(&kwm_lock);
        dragged_win_id = -1;
        spinlock_unlock(&kwm_lock);
        return 0; // Kirim event "release" ke app juga
    }

    // 2. Sedang dalam Drag — update posisi window mengikuti kursor
    // Bug 5.1: cek `dragged_win_id != -1` HARUS dilakukan di dalam kwm_lock.
    // CPU lain (destroy) bisa men-set dragged_win_id = -1 di antara cek di luar
    // lock dan deref di dalam lock → kwm_windows[-1] OOB. Baca + validasi ulang
    // nilai di dalam lock, dan cek window masih active.
    spinlock_lock(&kwm_lock);
    int drag_win = dragged_win_id;
    if (drag_win >= 0 && drag_win < MAX_WINDOWS &&
        kwm_windows[drag_win].active) {
        int32_t new_x = mouse_px - drag_offset_x;
        int32_t new_y = mouse_py - drag_offset_y;

        // Phase 5C: clamp terhadap FRAME (konten + titlebar).
        uint32_t fw = kwm_windows[drag_win].width;
        uint32_t fh = kwm_windows[drag_win].height + KWM_TITLEBAR_H;

        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if (new_x + (int32_t)fw > (int32_t)fb_width)
            new_x = (int32_t)fb_width  - (int32_t)fw;
        if (new_y + (int32_t)fh > (int32_t)fb_height)
            new_y = (int32_t)fb_height - (int32_t)fh;

        int32_t old_x = kwm_windows[drag_win].x;
        int32_t old_y = kwm_windows[drag_win].y;
        kwm_windows[drag_win].x = new_x;
        kwm_windows[drag_win].y = new_y;
        spinlock_unlock(&kwm_lock);
        // BUGFIX (Phase 4): invalidasikan bounding box visual LAMA dan BARU.
        // frame_dirty_area kini mencakup seluruh frame + drop-shadow (termasuk
        // offset +3px shadow). Compositor me-recomposite area lama dari scene
        // (base_canvas + desktop + window di bawahnya), bukan menyalin window.
        frame_dirty_area(old_x, old_y, fw, fh);
        frame_dirty_area(new_x, new_y, fw, fh);
        return 1; // Konsumsi event — jangan sampai app salah deteksi klik
    }
    spinlock_unlock(&kwm_lock);

    // 3. Mouse Down — hit-test, click-to-focus, Z-bring-to-front, dekorasi WM
    if (left_down) {
        int old_focus, target_win, close_owner = -1, start_drag = 0;
        {
            spinlock_lock(&kwm_lock);
            target_win  = kwm_hit_test_locked(mouse_px, mouse_py);
            old_focus   = focused_win_id;
            // Phase 10: desktop tak pernah refokus & tak bring-to-front —
            // klik wallpaper tetap diteruskan ke desktop (return 0).
            int is_desktop = (target_win >= 0 &&
                              (kwm_windows[target_win].flags & KWM_WIN_DESKTOP));
            if (is_desktop) {
                focused_win_id = old_focus;
            } else {
                // Phase 5B: click-to-focus. Klik area kosong = fokus -1 → keyboard
                // kembali ke TTY (shell).
                focused_win_id = target_win;
            }

            if (target_win != -1 && !is_desktop) {
                kwm_bring_to_front(target_win);

                int32_t fx = kwm_windows[target_win].x;
                int32_t fy = kwm_windows[target_win].y;
                int32_t fw = (int32_t)kwm_windows[target_win].width;

                // Phase 5C: klik di titlebar.
                if (mouse_py >= fy && mouse_py < fy + KWM_TITLEBAR_H) {
                    if (mouse_px >= fx + fw - KWM_CLOSE_BTN_W) {
                        // Tombol close → minta app menutup (EVENT_WIN_CLOSE),
                        // BUKAN destroy paksa. App yang memutuskan.
                        close_owner = kwm_windows[target_win].owner_task;
                    } else {
                        // Drag dimulai dari titlebar (selain tombol close).
                        dragged_win_id = target_win;
                        drag_offset_x  = mouse_px - fx;
                        drag_offset_y  = mouse_py - fy;
                        start_drag = 1;
                    }
                }
            }
            spinlock_unlock(&kwm_lock);
        }

        if (close_owner >= 0) {
            push_event_to(close_owner, EVENT_WIN_CLOSE, 0, 0, 0, target_win + 1);
            return 1;
        }

        // Repaint: z-order target berubah (bring-to-front); fokus lama
        // kehilangan tint titlebar jika bergeser. Di luar kwm_lock.
        if (target_win >= 0) kwm_frame_dirty(target_win);
        if (old_focus != target_win && old_focus >= 0) kwm_frame_dirty(old_focus);

        if (start_drag) return 1;   // Klik titlebar — konsumsi, jangan ke app
        return 0;                   // Klik konten / area kosong — teruskan
    }

    return 0; // Klik di area kosong — teruskan
}

// ============================================================
// Phase 5B — Routing Input Per-Task
// Dipanggil dari IRQ keyboard/mouse SEBELUM push_event_to().
// out_win_id = nilai untuk field win_id event (slot KWM + 1; 0 = tidak ada).
// ============================================================

// Keyboard → owner task dari window fokus. -1 = tidak ada fokus (driver
// mengirim keystroke ke TTY buffer, bukan event queue).
int kwm_route_keyboard(int* out_win_id) {
    spinlock_lock(&kwm_lock);   // konteks IRQ — konsisten dengan kwm_process_mouse
    int owner = -1;
    int enc   = 0;
    if (focused_win_id >= 0 && focused_win_id < MAX_WINDOWS &&
        kwm_windows[focused_win_id].active) {
        owner = kwm_windows[focused_win_id].owner_task;
        enc   = focused_win_id + 1;
    }
    spinlock_unlock(&kwm_lock);
    if (out_win_id) *out_win_id = enc;
    return owner;
}

// Mouse move/click/wheel → owner task dari window di bawah kursor.
// -1 = kursor di area kosong (event tidak diteruskan ke siapa pun).
// Phase 5C: out_lx/out_ly = koordinat WINDOW-LOCAL konten (y=0 = baris isi
// pertama, di bawah titlebar) — diterjemahkan di sini, app tidak perlu lagi
// konversi layar→lokal (sys_get_window_pos dihapus).
int kwm_route_mouse(int32_t sx, int32_t sy, int* out_win_id,
                    int32_t* out_lx, int32_t* out_ly) {
    spinlock_lock(&kwm_lock);
    int w     = kwm_hit_test_locked(sx, sy);
    int owner = -1;
    int enc   = 0;
    if (w >= 0) {
        owner = kwm_windows[w].owner_task;
        enc   = w + 1;
        if (out_lx) *out_lx = sx - kwm_windows[w].x;
        // Phase 10: desktop frameless — konten mulai di y window.
        uint32_t tb = (kwm_windows[w].flags & KWM_WIN_DESKTOP) ? 0 : KWM_TITLEBAR_H;
        if (out_ly) *out_ly = sy - (kwm_windows[w].y + (int32_t)tb);
    }
    spinlock_unlock(&kwm_lock);
    if (out_win_id) *out_win_id = enc;
    return owner;
}

// ============================================================
// Phase 5D — Shortcut WM: Alt-Tab (siklus fokus + bring-to-front)
// ============================================================

// bitmask modifier (cocok userlib.h) — lokal, kwm.c tidak include userlib.h
#define KEY_MOD_ALT 0x04
// scancode set-1 Tab = 0x0F (tidak extended; key_id tanpa bit 0x100)
#define KBD_SCAN_TAB 0x0F

// 1 = sesi Alt-Tab aktif (Alt ditahan setelah Alt+Tab). Sampai Alt dilepas,
// semua Tab press/release ditelan WM — tidak bocor sebagai Tab ke app/shell.
static int alt_tab_active = 0;

// Caller MUST hold kwm_lock. Pindahkan fokus ke window aktif berikutnya
// (z-order naik; wrap ke paling bawah) dan bawa ke depan. Tanpa fokus → ambil
// window teratas (z tertinggi).
static void kwm_cycle_focus_locked(void) {
    int cur   = focused_win_id;
    int cur_z = -1;
    if (cur >= 0 && cur < MAX_WINDOWS && kwm_windows[cur].active)
        cur_z = (int)kwm_windows[cur].z_index;

    int target = -1;
    int next_z = (int)next_z_index;   // > semua z window aktif
    int lowest_z = (int)next_z_index;
    int lowest_win = -1;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!kwm_windows[i].active) continue;
        if (kwm_windows[i].flags & KWM_WIN_DESKTOP) continue;   // Phase 10
        int z = (int)kwm_windows[i].z_index;
        if (z < lowest_z) { lowest_z = z; lowest_win = i; }
        if (cur_z >= 0 && z > cur_z && z < next_z) { next_z = z; target = i; }
    }

    if (cur_z < 0) {
        // Tidak ada fokus → window teratas (bukan desktop).
        int hi = -1;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (kwm_windows[i].active &&
                !(kwm_windows[i].flags & KWM_WIN_DESKTOP) &&
                (int)kwm_windows[i].z_index > hi)
                { hi = (int)kwm_windows[i].z_index; target = i; }
    } else if (target < 0) {
        target = lowest_win;   // wrap: fokus kembali ke paling bawah
    }

    if (target >= 0) {
            kwm_windows[target].z_index = next_z_index++;   // bring-to-front
            if (next_z_index > MAX_WINDOWS) kwm_normalize_zindex_locked();
        focused_win_id = target;
    } else {
        focused_win_id = -1;
    }
}

// Intercept shortcut WM dari IRQ keyboard, SEBELUM routing normal.
// Return 1 = event dikonsumsi KWM (jangan di-route/di-TTY), 0 = lanjut normal.
// Identitas tombol via key_id (scancode), bukan ascii — Tab bisa terbaca sama
// dengan kombinasi lain di table shift.
int kwm_handle_shortcut(uint8_t mods, uint8_t released, uint16_t key_id) {
    // Alt dilepas → akhiri sesi Alt-Tab, telan event modifier.
    if (alt_tab_active && !(mods & KEY_MOD_ALT)) {
        alt_tab_active = 0;
        return 1;
    }
    // Alt+Tab: siklus fokus pada press; press & release ditelan.
    if ((mods & KEY_MOD_ALT) && key_id == KBD_SCAN_TAB) {
        if (!released) {
            int old_focus, new_focus;
            spinlock_lock(&kwm_lock);
            old_focus = focused_win_id;
            kwm_cycle_focus_locked();
            new_focus = focused_win_id;
            spinlock_unlock(&kwm_lock);
            // Repaint: window target naik z-order; fokus lama kehilangan tint.
            if (new_focus >= 0) kwm_frame_dirty(new_focus);
            if (old_focus >= 0 && old_focus != new_focus) kwm_frame_dirty(old_focus);
            alt_tab_active = 1;
        }
        return 1;
    }
    return 0;
}
