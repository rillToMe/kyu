#include "mouse.h"
#include "io.h"
#include "spinlock.h"
#include <stdint.h>

// Impor kanvas dan resolusi dari kernel.c
#include "display.h"
// Posisi awal kursor (Tengah layar)
int32_t mouse_x = 512; 
int32_t mouse_y = 384;


// Cetak biru bentuk kursor (0 = Tembus Pandang, 1 = Garis Putih, 2 = Isi Hitam)
const uint8_t cursor_bitmap[16][12] = {
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,1,1,1,1,1,1},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0}
};

// --- LOGIKA HARDWARE PS/2 ---
void mouse_wait(uint8_t a_type) {
    uint32_t timeout = 100000;
    if (a_type == 0) {
        while (timeout--) if ((inb(0x64) & 1) == 1) return; // Tunggu data siap
    } else {
        while (timeout--) if ((inb(0x64) & 2) == 0) return; // Tunggu siap ditulis
    }
}

void mouse_write(uint8_t a_write) {
    mouse_wait(1);
    outb(0x64, 0xD4); // Peringatkan controller: data ini untuk mouse!
    mouse_wait(1);
    outb(0x60, a_write);
}

uint8_t mouse_read() {
    mouse_wait(0);
    return inb(0x60);
}

// Phase 4 (wheel): 1 = mode IntelliMouse aktif — paket 4 byte, byte[3] = Z.
static uint8_t mouse_has_wheel = 0;

void init_mouse() {
    uint8_t status;
    mouse_wait(1); outb(0x64, 0xA8);
    mouse_wait(1); outb(0x64, 0x20);
    mouse_wait(0); status = (inb(0x60) | 2);
    mouse_wait(1); outb(0x64, 0x60);
    mouse_wait(1); outb(0x60, status);
    mouse_write(0xF6); mouse_read();

    // Phase 4: aktifkan scroll wheel — "magic knock" IntelliMouse
    // (sample rate 200 → 100 → 80, lalu baca device ID; 3 = wheel ada).
    mouse_write(0xF3); mouse_read(); mouse_write(200); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(100); mouse_read();
    mouse_write(0xF3); mouse_read(); mouse_write(80);  mouse_read();
    mouse_write(0xF2); mouse_read();               // ACK
    if (mouse_read() == 3) mouse_has_wheel = 1;    // device ID

    mouse_write(0xF4); mouse_read();

    // (HAPUS PEMANGGILAN draw_mouse() DARI SINI KARENA AKAN DITANGANI COMPOSITOR)
}

uint8_t mouse_cycle = 0;
int8_t mouse_byte[4];

// Phase 5B/5C: event dikirim per-task — KWM me-route ke window di bawah
// kursor dan menerjemahkan koordinat ke window-local konten (out_lx/out_ly).
extern void push_event_to(int task_id, uint32_t type, int32_t p1, int32_t p2, int32_t p3, int32_t win_id);
extern int  kwm_route_mouse(int32_t x, int32_t y, int* out_win_id, int32_t* out_lx, int32_t* out_ly);
// KWM V2: intercept mouse events untuk drag & z-index sebelum dikirim ke app
extern int kwm_process_mouse(int32_t mx, int32_t my, uint8_t left_down, uint8_t left_up);

// Pelacak status memori tombol (agar tidak spam klik)
static uint8_t last_left_click  = 0;
static uint8_t last_right_click = 0;

static spinlock_t mouse_state_lock = SPINLOCK_INIT;

void mouse_handler() {
    uint8_t status = inb(0x64);
    uint8_t packet_size = mouse_has_wheel ? 4 : 3;
    int8_t wheel_z = 0;
    if ((status & 0x01) && (status & 0x20)) {
        // Guard index terlebih dahulu: jangan pernah menulis melewati
        // mouse_byte[3] (bug 6.1 — OOB write di jalur IRQ). Reset pakai '>='
        // supaya akumulator tak bisa meleset melewati packet_size (mis. paket
        // yang hilang mengubah 4→3) dan mengaburkan batas array.
        if (mouse_cycle >= (int)sizeof(mouse_byte)) mouse_cycle = 0;
        mouse_byte[mouse_cycle++] = inb(0x60);
        if (mouse_cycle >= packet_size) {
            mouse_cycle = 0;
            if ((mouse_byte[0] & 0x80) || (mouse_byte[0] & 0x40)) goto end_mouse_irq;
            if (mouse_has_wheel) wheel_z = mouse_byte[3];

            spinlock_lock(&mouse_state_lock);

            mouse_x += mouse_byte[1];
            mouse_y -= mouse_byte[2];

            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            const display_mode_t* m = display_get_mode();
            if (m) {
                if (mouse_x > (int32_t)(m->width - 12))  mouse_x = m->width - 12;
                if (mouse_y > (int32_t)(m->height - 16)) mouse_y = m->height - 16;
            }

            // --- DETEKSI KLIK (edge detect) ---
            uint8_t left_click  = mouse_byte[0] & 0x01;
            uint8_t right_click = (mouse_byte[0] & 0x02) >> 1;

            uint8_t left_down = ( left_click && !last_left_click);  // baru ditekan
            uint8_t left_up   = (!left_click &&  last_left_click);  // baru dilepas

            // --- KWM V2 INTERCEPTION ---
            // kwm_process_mouse menangani drag + Z-index.
            // Return 1 = event "dimakan" KWM, jangan push ke app.
            // Return 0 = teruskan event ke app seperti biasa.
            int kwm_consumed = kwm_process_mouse(mouse_x, mouse_y, left_down, left_up);

            // Phase 5B: semua event mouse di-route ke owner window di bawah
            // kursor (hover tetap jalan). Klik/gerak di area kosong tidak
            // diteruskan ke siapa pun.
            if (!kwm_consumed) {
                // Klik kiri — kirim ke app jika KWM tidak mengonsumsinya
                if (left_click != last_left_click) {
                    // EVENT_MOUSE_CLICK (3) → P1: 0 (kiri), P2: 1=ditekan / 0=dilepas
                    int win = 0, target = kwm_route_mouse(mouse_x, mouse_y, &win, 0, 0);
                    if (target >= 0) push_event_to(target, 3, 0, left_click, 0, win);
                }
            } else if (left_up) {
                // Selalu kirim mouse-up ke app agar state tombol tidak terjebak "pressed"
                int win = 0, target = kwm_route_mouse(mouse_x, mouse_y, &win, 0, 0);
                if (target >= 0) push_event_to(target, 3, 0, 0, 0, win);
            }

            // Klik kanan — selalu teruskan ke app (KWM tidak menggunakannya)
            if (right_click != last_right_click) {
                int win = 0, target = kwm_route_mouse(mouse_x, mouse_y, &win, 0, 0);
                if (target >= 0) push_event_to(target, 3, 1, right_click, 0, win);
                last_right_click = right_click;
            }

            last_left_click = left_click;

            // Pergerakan mouse dikirim ke window di bawah kursor (hover)
            // Phase 5C: koordinat window-local (lx, ly) — app tak perlu lagi
            // konversi layar→lokal.
            {
                int win = 0, lx = 0, ly = 0;
                int target = kwm_route_mouse(mouse_x, mouse_y, &win, &lx, &ly);
                if (target >= 0) push_event_to(target, 2, lx, ly, 0, win); // EVENT_MOUSE_MOVE (2)
            }

            spinlock_unlock(&mouse_state_lock);

            // --- Phase 3D/4/5B: routing scroll wheel (di luar mouse_state_lock —
            // scrollback me-render layar, terlalu berat untuk dipegang lock) ---
            // PS/2: Z = +1 wheel ke bawah, -1 wheel ke atas.
            // Phase 5B: wheel → window DI BAWAH KURSOR; area kosong → scrollback terminal.
            if (wheel_z != 0) {
                extern void tty_scroll_view(int32_t delta_lines);
                // Phase 5C: P2/P3 = koordinat window-local.
                int win = 0, lx = 0, ly = 0;
                int target = kwm_route_mouse(mouse_x, mouse_y, &win, &lx, &ly);
                if (target >= 0) {
                    // EVENT_SCROLL (4) → P1: delta (+1 bawah / -1 atas), P2/P3: posisi
                    push_event_to(target, 4, (int32_t)wheel_z, lx, ly, win);
                } else {
                    // Terminal: wheel atas (Z<0) = masuk riwayat (offset naik)
                    tty_scroll_view((int32_t)(-wheel_z) * 3);
                }
            }
        }
    } else if (status & 0x01) {
        inb(0x60);
    }

end_mouse_irq:
    outb(0xA0, 0x20); outb(0x20, 0x20);
}