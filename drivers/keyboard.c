#include "io.h"
#include <stdint.h>
#include "spinlock.h"

// Phase 5B: event dikirim per-task — KWM menentukan tujuan (routing).
extern void push_event_to(int task_id, uint32_t type, int32_t p1, int32_t p2, int32_t p3, int32_t win_id);
extern int  kwm_route_keyboard(int* out_win_id);

// --- Tipe Event (harus cocok dengan userlib.h & kernel/event.c) ---
#define EVENT_KEY_PRESS     1
#define EVENT_KEY_RELEASE   5

// --- Bitmask modifier (harus cocok dengan userlib.h) ---
// Dikirim di P2 setiap EVENT_KEY_PRESS / EVENT_KEY_RELEASE.
#define KEY_MOD_SHIFT   0x01
#define KEY_MOD_CTRL    0x02
#define KEY_MOD_ALT     0x04
#define KEY_MOD_CAPS    0x08

// Tombol modifier yang sedang DITAHAN, dilacak per sisi (kiri/kanan) agar
// lepas-satu tidak membatalkan sisi yang masih ditekan.
#define HELD_LSHIFT     0x01
#define HELD_RSHIFT     0x02
#define HELD_LCTRL      0x04
#define HELD_RCTRL      0x08
#define HELD_LALT       0x10
#define HELD_RALT       0x20
#define HELD_CAPS       0x40

#define KBD_BUFFER_SIZE 256
volatile uint8_t kbd_buffer[KBD_BUFFER_SIZE];
volatile uint32_t kbd_head = 0;
volatile uint32_t kbd_tail = 0;
static spinlock_t kbd_lock = SPINLOCK_INIT;

// State Phase 4 — hanya disentuh di dalam IRQ handler (satu konteks,
// tidak reentrant) sehingga tidak butuh lock.
static uint8_t kbd_held = 0;      // tombol modifier yang sedang ditahan (HELD_*)
static uint8_t kbd_mods = 0;      // bitmask modifier aktif (KEY_MOD_*)
static uint8_t kbd_extended = 0;  // prefix E0 diterima, scancode berikutnya extended

// Tabel Scancode Normal (Tanpa Shift)
const unsigned char kbdus[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
  '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-',
    0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Tabel Scancode Kapital & Simbol (Dengan Shift)
const unsigned char kbdus_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
  '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-',
    0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void pic_remap() {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xF8); outb(0xA1, 0xEF);
}

void keyboard_handler() {
    uint8_t status = inb(0x64);

    // Tambahkan pelindung: JANGAN BACA jika Bit 5 (Mouse) menyala!
    if ((status & 0x01) && !(status & 0x20)) {
        uint8_t scancode = inb(0x60);

        // Prefix extended (E0): tandai — scancode sebenarnya menyusul di IRQ
        // berikutnya. Bit 8 pada key_id menandai tombol extended (mis. Ctrl
        // kanan, arrow keys) agar tidak tertukar dengan padanan non-extended.
        if (scancode == 0xE0) {
            kbd_extended = 1;
            outb(0x20, 0x20);
            return;
        }

        uint8_t released = (scancode & 0x80) != 0;
        uint8_t code     = scancode & 0x7F;
        uint16_t key_id  = code | (kbd_extended ? 0x100 : 0);
        kbd_extended = 0;

        // 1. Update status modifier (dilacak per sisi)
        switch (key_id) {
            case 0x2A:   // Shift kiri
                if (released) kbd_held &= ~HELD_LSHIFT; else kbd_held |= HELD_LSHIFT;
                break;
            case 0x36:   // Shift kanan
                if (released) kbd_held &= ~HELD_RSHIFT; else kbd_held |= HELD_RSHIFT;
                break;
            case 0x1D:   // Ctrl kiri
                if (released) kbd_held &= ~HELD_LCTRL; else kbd_held |= HELD_LCTRL;
                break;
            case 0x11D:  // Ctrl kanan (E0)
                if (released) kbd_held &= ~HELD_RCTRL; else kbd_held |= HELD_RCTRL;
                break;
            case 0x38:   // Alt kiri
                if (released) kbd_held &= ~HELD_LALT; else kbd_held |= HELD_LALT;
                break;
            case 0x138:  // Alt kanan (E0)
                if (released) kbd_held &= ~HELD_RALT; else kbd_held |= HELD_RALT;
                break;
            case 0x3A:   // CapsLock — toggle SEKALI per tekan (tahan ≠ toggle ulang)
                if (!released) {
                    if (!(kbd_held & HELD_CAPS)) {
                        kbd_mods ^= KEY_MOD_CAPS;
                        kbd_held |= HELD_CAPS;
                    }
                } else {
                    kbd_held &= ~HELD_CAPS;
                }
                break;
        }

        // Derive bitmask modifier publik dari tombol yang sedang ditahan
        kbd_mods &= KEY_MOD_CAPS;
        if (kbd_held & (HELD_LSHIFT | HELD_RSHIFT)) kbd_mods |= KEY_MOD_SHIFT;
        if (kbd_held & (HELD_LCTRL  | HELD_RCTRL))  kbd_mods |= KEY_MOD_CTRL;
        if (kbd_held & (HELD_LALT   | HELD_RALT))   kbd_mods |= KEY_MOD_ALT;

        // 2. Terjemahkan ke ASCII
        uint8_t ascii = kbdus[code];
        if (!released) {
            // Press: shift memilih tabel; CapsLock hanya memengaruhi huruf
            // (XOR dengan shift — perilaku standar).
            uint8_t upper = (kbd_mods & KEY_MOD_SHIFT) != 0;
            if ((kbd_mods & KEY_MOD_CAPS) && ascii >= 'a' && ascii <= 'z')
                upper = !upper;
            if (upper) ascii = kbdus_shift[code];
        }
        // Release: P1 = ASCII dasar (tanpa shift/caps) sebagai identitas
        // tombol — stabil terhadap urutan pelepasan modifier. Pairing
        // press↔release yang pasti memakai P3 (key_id), bukan P1.

        // 3. Phase 5D: shortcut WM (Alt-Tab) di-intercept sebelum routing.
        extern int kwm_handle_shortcut(uint8_t mods, uint8_t released,
                                       uint16_t key_id);
        int shortcut = kwm_handle_shortcut(kbd_mods, released, key_id);

        // 4. Routing Phase 5B (keputusan #6): ada window fokus → keystroke
        //    HANYA menjadi event ke queue task pemilik fokus; tidak ada
        //    window fokus → HANYA masuk TTY buffer (shell). Klik area kosong
        //    mengosongkan fokus = jalan kembali ke shell.
        if (!shortcut) {
            int kbd_win = 0;
            int kbd_target = kwm_route_keyboard(&kbd_win);

            if (kbd_target >= 0) {
                // Push event untuk SEMUA tombol (printable maupun tidak).
                //    Sengaja di luar kbd_lock dan TIDAK tergantung sisa ruang
                //    TTY buffer — dulu event ikut mati setelah 256 keystroke
                //    saat app GUI berjalan (buffer TTY penuh tak terbaca).
                push_event_to(kbd_target,
                              released ? EVENT_KEY_RELEASE : EVENT_KEY_PRESS,
                              ascii, kbd_mods, key_id, kbd_win);
            } else if (!released && ascii != 0 && !(kbd_mods & (KEY_MOD_CTRL | KEY_MOD_ALT))) {
                // 5. TTY buffer: hanya teks murni. Kombinasi Ctrl/Alt dianggap
                //    shortcut, bukan ketikan.
                spinlock_lock(&kbd_lock);
                uint32_t next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
                if (next_head != kbd_tail) {
                    kbd_buffer[kbd_head] = ascii;
                    kbd_head = next_head;
                }
                spinlock_unlock(&kbd_lock);
            } else if (!released && key_id == 0x2E && (kbd_mods & KEY_MOD_CTRL) &&
                       !(kbd_mods & KEY_MOD_ALT)) {
                // P0 Phase 6A: Ctrl+C dimasak jadi ETX (0x03) untuk TTY —
                // satu-satunya kombo Ctrl yang masuk buffer (scancode 0x2E =
                // tombol C fisik, kiri/kanan Ctrl sama; Caps tak relevan).
                // GUI tidak lewat sini (dapat event + mods via antrian KWM).
                spinlock_lock(&kbd_lock);
                uint32_t next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
                if (next_head != kbd_tail) {
                    kbd_buffer[kbd_head] = 0x03;
                    kbd_head = next_head;
                }
                spinlock_unlock(&kbd_lock);
            }
        }
    }
    outb(0x20, 0x20); // End of Interrupt
}

uint32_t keyboard_read(uint8_t *buffer, uint32_t size) {
    uint64_t flags = spinlock_lock_irqsave(&kbd_lock);
    uint32_t bytes_read = 0;
    while (bytes_read < size && kbd_head != kbd_tail) {
        buffer[bytes_read] = kbd_buffer[kbd_tail];
        kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
        bytes_read++;
    }
    spinlock_unlock_irqrestore(&kbd_lock, flags);
    return bytes_read;
}

// Buang semua karakter yang menunggu di keyboard TTY buffer.
// Dipanggil bersamaan dengan flush_event_queue() saat ganti app,
// agar ketikan di app lama tidak bocor ke app berikutnya.
void flush_kbd_buffer(void) {
    uint64_t flags = spinlock_lock_irqsave(&kbd_lock);
    kbd_head = 0;
    kbd_tail = 0;
    spinlock_unlock_irqrestore(&kbd_lock, flags);
}


void init_keyboard() {
    // Reset status buffer keyboard
    // CATATAN: IDT untuk IRQ1 (INT 33) sudah didaftarkan di arch/x86/idt.c
    // dengan pointer 64-bit yang benar. JANGAN re-register di sini karena akan
    // overwrite dengan pointer yang truncated (uint32_t).
    kbd_head = 0;
    kbd_tail = 0;
    kbd_held = 0;
    kbd_mods = 0;
    kbd_extended = 0;
}
