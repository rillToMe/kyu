// ============================================================
// kernel/timer_callbacks.c — Default Timer Subscribers, Kyuzen OS
//
// STANDAR: Semua timing di sini pakai timer_get_ms() + timestamp target.
//   BUKAN: if (tick % 60 == 0) — fragile, tergantung refresh rate
//   TAPI:  if (now >= next_event) — hardware-agnostic, benar di semua Hz
//
// Subscriber terdaftar:
//   Slot 0 — cb_visual  : spinner + uptime HUD
//   Slot 1 — cb_cursor  : cursor blink TTY
//   Slot 2 — cb_flush   : compositor screen flush
//   Slot 3-7 — reserved untuk driver/fitur baru
// ============================================================

#include <stdint.h>
#include "timer.h"
#include "display.h"

// --- Dependencies dari subsistem lain ---
extern void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
extern void draw_char(char c, uint32_t x, uint32_t y, uint32_t color);
extern void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t color);
extern void tty_blink_cursor(void);
extern void compositor_flush(void);

// --- Network stack dependencies ---
// e1000_poll() dipanggil setiap tick untuk memproses paket masuk dari NIC.
extern void e1000_poll(void);
// sys_check_timeouts() dipanggil setiap 1ms untuk drive lwIP software timers
// (TCP retransmit, ARP expiry, DHCP renewal, dll).
// Di-define oleh lwIP di src/core/timeouts.c
extern void sys_check_timeouts(void);
extern void kfs_sync_all(void);   // KyuzenFS V4 write-back (cb_fsync)

// ============================================================
// CALLBACK 1: Visual HUD — spinner + uptime di pojok kanan atas
//
// Menggunakan timestamp ms, bukan modulo ticks.
// Ini benar di 60Hz, 100Hz, 144Hz — tidak perlu diubah saat ganti refresh rate.
// ============================================================
static uint64_t next_spinner_update = 0;  // ms target: kapan spinner ganti frame
static uint64_t next_uptime_update  = 0;  // ms target: kapan uptime di-refresh


static void cb_visual(uint32_t tick) {
    (void)tick; // Tidak pakai raw tick — pakai ms timestamp
    const display_mode_t* m = display_get_mode();
    if (!m || m->width == 0) return;

    uint64_t now = timer_get_ms();


    // --- Spinner: ganti frame setiap 200ms ---
    if (now >= next_spinner_update) {
        next_spinner_update = now + 200;

        static uint8_t spin_frame = 0;
        const char frames[] = {'|', '/', '-', '\\'};
        spin_frame = (spin_frame + 1) % 4;

        draw_rect(m->width - 18, 3, 10, 16, 0x1E1E1E);
        draw_char(frames[spin_frame], m->width - 18, 3, 0xFFFF00);
    }

    // --- Uptime: update setiap 1000ms (1 detik tepat) ---
    if (now >= next_uptime_update) {
        next_uptime_update = now + 1000;

        uint64_t total_sec = timer_get_seconds();

        uint32_t sec  = total_sec % 60;
        uint32_t min  = (total_sec / 60) % 60;
        uint32_t hour = (total_sec / 3600);

        char buf[] = "UPTIME: 00:00:00";
        buf[8]  = (hour / 10) + '0';
        buf[9]  = (hour % 10) + '0';
        buf[11] = (min  / 10) + '0';
        buf[12] = (min  % 10) + '0';
        buf[14] = (sec  / 10) + '0';
        buf[15] = (sec  % 10) + '0';

        // 16 char × 8px/char = 128px + 8px margin = 136px wide
        draw_rect(m->width - 155, 3, 136, 16, 0x1E1E1E);
        draw_string(buf, m->width - 155, 3, 0x00FF00);
    }
}

// ============================================================
// CALLBACK 2: Cursor blink — setiap 500ms
// ============================================================
static uint64_t next_cursor_blink = 0;


static void cb_cursor(uint32_t tick) {
    (void)tick;
    uint64_t now = timer_get_ms();

    if (now >= next_cursor_blink) {
        next_cursor_blink = now + 500; // Blink setiap 500ms
        tty_blink_cursor();
    }
}

// ============================================================
// CALLBACK 3: Screen flush — setiap tick (default 60fps, preset 60/100/144)
// Untuk hemat CPU, ubah ke setiap 2 tick (25fps): tambah timestamp check.
// ============================================================
static void cb_flush(uint32_t tick) {
    (void)tick;
    compositor_flush();
}

// ============================================================
// CALLBACK 4: Network polling — setiap tick
//
// Dua tugas:
//   A. e1000_poll()         → cek RX ring NIC, copy paket masuk ke lwIP pbuf
//   B. sys_check_timeouts() → drive semua lwIP software timers
//
// Frekuensi:
//   - e1000_poll()         : SETIAP tick (default ~16ms pada 60Hz)
//                            Lebih sering = lebih responsive, tapi refresh rate
//                            yang rendah ini sudah cukup untuk throughput normal.
//   - sys_check_timeouts() : Setiap 1ms (sesuai kontrak lwIP).
//                            Kita gunakan timestamp untuk memastikan dipanggil
//                            minimal 1x per ms meskipun tick rate default 60Hz.
//
// PENTING: Kedua fungsi ini NO-OP jika driver/lwIP belum diinisialisasi,
//          jadi aman dipanggil bahkan sebelum net_init().
// ============================================================
static uint64_t next_lwip_tick = 0; // ms target untuk sys_check_timeouts

// net_lock serializes ALL lwIP entry (socket calls on any CPU + this poll).
// TCP callbacks fire from inside e1000_poll/sys_check_timeouts, so the poll
// must hold the lock too. Held only briefly here — never across a wait.
extern void net_lock_acquire(uint64_t *saved);
extern void net_lock_release(uint64_t saved);

static void cb_network(uint32_t tick) {
    (void)tick;

    uint64_t saved;
    net_lock_acquire(&saved);

    e1000_poll();

    uint64_t now = timer_get_ms();
    if (now >= next_lwip_tick) {
        next_lwip_tick = now + 1;
        sys_check_timeouts();
    }

    net_lock_release(saved);
}

// ============================================================
// CALLBACK 5: Filesystem sync — tulis balik block dirty (bcache) +
// metadata (bitmap/superblock) setiap 3 detik. Write-back interval
// kecil membatasi kehilangan data saat power loss tanpa membuat I/O
// disk konstan. kfs_sync_all no-op bila FS belum termount.
// ============================================================
static uint64_t next_fs_sync = 0;

static void cb_fsync(uint32_t tick) {
    (void)tick;
    uint64_t now = timer_get_ms();
    if (now >= next_fs_sync) {
        next_fs_sync = now + 3000;
        kfs_sync_all();
    }
}

// ============================================================
// ENTRY POINT — dipanggil dari kmain setelah init_timer()
// ============================================================
void timer_callbacks_init(void) {
    timer_register(cb_visual);   // Slot 0: HUD spinner + uptime
    timer_register(cb_cursor);   // Slot 1: TTY cursor blink
    timer_register(cb_flush);    // Slot 2: Compositor screen flush
    timer_register(cb_network);  // Slot 3: e1000 RX poll + lwIP timeouts
    timer_register(cb_fsync);    // Slot 4: KyuzenFS V4 write-back (3s)
    // Slot 5-7: tersedia untuk audio tick, animasi, dll
}
