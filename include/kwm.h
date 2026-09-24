#ifndef KWM_H
#define KWM_H

#include <stdint.h>
#include "kwm_abi.h"   // kwm_window_info_t kanonis (shared kernel<->user, layout frozen)

// --- KYUZEN WINDOW MANAGER (KWM) — kernel/gfx/kwm.c ---

int  kwm_create_window(int x, int y, uint32_t width, uint32_t height);
void kwm_update_window(int win_id, uint32_t* app_buffer);
void kwm_destroy_window(int win_id);

// Phase 3 — update HANYA sebuah rect konten window. app_buffer tetap canvas
// penuh app (stride = window_width*4); hanya baris/kolom rect yang disalin,
// baris demi baris. Caller syscall sudah memvalidasi rentang buffer user untuk
// span baris ini; fungsi ini memvalidasi ulang ownership + batas rect.
// Return 0 sukses, -1 ditolak.
int  kwm_update_window_rect(int win_id, int32_t x, int32_t y,
                            uint32_t width, uint32_t height, uint32_t* app_buffer);

// Phase 3 — geometri konten window (0 sukses, -1 slot invalid). Dipakai
// syscall 66 untuk menghitung stride canvas user sebelum validasi range.
int  kwm_window_dims(int win_id, uint32_t* out_w, uint32_t* out_h);

// Owner task dari sebuah window (-1 jika slot kosong/id invalid).
int  kwm_window_owner(int win_id);

// Owner task dari window desktop aktif (-1 bila tak ada desktop).
// Dipakai syscall 84 untuk mengantar event reload ke task yang tepat.
int  kwm_desktop_owner(void);

// Ukuran canvas window dalam byte (0 jika slot kosong/id invalid).
uint64_t kwm_window_canvas_bytes(int win_id);

// Destroy HANYA window milik task_id — dipanggil saat app exit/exec.
void kwm_destroy_windows_of(int task_id);

// Destroy ALL KWM windows — hanya untuk path kernel/test, BUKAN syscall.
void kwm_destroy_all_windows(void);

// Intercept mouse event sebelum dikirim ke user-space.
// Return: 1 = event dimakan KWM, 0 = teruskan ke app.
int  kwm_process_mouse(int32_t mouse_px, int32_t mouse_py,
                       uint8_t left_down, uint8_t left_up);

// Phase 5B — routing input per-task (dipanggil dari IRQ keyboard/mouse).
// out_win_id menerima nilai untuk field win_id event (slot KWM + 1;
// 0 = tidak relevan). Return: owner task id, atau -1 jika tidak ada target.
int  kwm_route_keyboard(int* out_win_id);                 // owner window fokus
// Phase 5C: out_lx/out_ly = koordinat window-local konten (jika NULL, tidak
// ditulis). Hit-test & translasi dilakukan di sini.
int  kwm_route_mouse(int32_t x, int32_t y, int* out_win_id,
                     int32_t* out_lx, int32_t* out_ly);   // owner window di bawah kursor

// Phase 5D: shortcut WM (Alt-Tab) di-intercept dari IRQ keyboard SEBELUM
// routing. Return 1 = dikonsumsi KWM (jangan di-route/ke TTY), 0 = lanjut.
int  kwm_handle_shortcut(uint8_t mods, uint8_t released, uint16_t key_id);

// ============================================================
// Phase 10 — Desktop window + taskbar support
// ============================================================
// kwm_window_info_t: definisi kanonis di kwm_abi.h (di-include di atas).
// Window desktop: full-screen, frameless, z=0, no-focus. Satu-satunya.
int  kwm_create_desktop(void);
// Set judul window (titlebar + taskbar). Hanya pemilik. 0 / -1.
int  kwm_set_title(int win_id, const char* title);
// Isi buffer dgn window aktif. Return jumlah / -1.
int  kwm_get_windows(kwm_window_info_t* buf, int max);
// Bring-to-front + fokus (klik taskbar). Desktop ditolak. 0 / -1.
int  kwm_activate_window(int win_id);
// Phase 14 — deklarasi canvas window 100% opaque (syscall 67, kernel/gfx/kwm.c).
// Owner-only; kernel memvalidasi seluruh canvas sebelum menandai. 0 / -1.
int  kwm_set_window_opaque(int win_id);

#endif
