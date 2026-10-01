// system/init.c — init.elf: proses pertama di Ring 3 (PID 1).
//
// Dibangun sebagai ELF user-space (pola system/cat.c, echo.c, zen.c). Kernel
// hanya memuat dan menjalankan file ini; seluruh kebijakan startup ada di sini,
// bukan di kernel image.
//
// Tugas:
//   1. Spawn login.elf (layar login TTY) — UI utama. Gagal = FATAL.
//   2. Supervisi: kalau login mati, catat ke serial dan restart dengan backoff.
//      Ini yang membuat sistem tidak bisa "mati" karena satu app crash.
//
// PENTING — desktop.elf TIDAK di-spawn di sini. Dulu sempat, dan hasilnya race
// layar: login menulis prompt TTY, lalu desktop menimpanya dengan GUI sebelum
// user sempat mengetik (prompt tidak terlihat, sistem tampak "freeze").
// Desktop sekarang di-spawn oleh login.elf SETELAH autentikasi sukses, jadi:
//   - desktop hanya jalan untuk sesi yang sudah terautentikasi (lebih benar
//     secara keamanan), dan
//   - tidak ada dua penulis layar yang berebut di saat yang sama.
//
// Kenapa init dipisah dari kernel (bukan dipanggil dari kernel_main):
//   Kode yang di-link ke kernel tidak bisa berjalan di CPL 3 — heap kernel
//   dipetakan US=0 (kernel/mm/heap.c:139), jadi transisi iretq ke CS=0x1B
//   langsung #PF. Seluruh jalur user harus keluar dari image kernel dulu.
//   Lihat docs/design/ring3-init-migration.md.

#include "userlib.h"

// Anak yang diawasi. Hanya login: desktop adalah tanggung jawab login
// (di-spawn setelah auth), bukan init.
#define SUP_MAX 1

static const char* kChildPath[SUP_MAX] = { "/apps/login.elf" };
static const char* kChildName[SUP_MAX] = { "login"           };
static int         child_pid[SUP_MAX]      = { -1 };
static int         child_restarts[SUP_MAX] = { 0 };

// Backoff restart: 1s, 2s, 4s, ... dibatasi 30s. Tanpa ini, anak yang crash
// saat start akan di-restart ribuan kali per detik dan membanjiri serial.
static uint32_t restart_delay_ms(int restarts) {
    uint32_t d = 1000;
    for (int i = 0; i < restarts && d < 30000; i++) d *= 2;
    return d > 30000 ? 30000 : d;
}

static int spawn_child(int slot) {
    if (!sys_file_exists((char*)kChildPath[slot])) return -1;
    int pid = sys_spawn((char*)kChildPath[slot]);
    child_pid[slot] = pid;
    return pid;
}

void main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Serial-first logging: init berjalan sebelum GUI ada, dan jejaknya harus
    // terlihat di host kalau boot gagal. print() ke TTY tetap dipakai supaya
    // terlihat juga di layar konsol.
    print("[init] KyuzenOS init dimulai\n");

    // Login adalah UI utama — gagal = sistem tidak bisa dipakai sama sekali.
    if (spawn_child(0) < 0) {
        print("[init] FATAL: login.elf tidak bisa di-spawn\n");
        print("[init] pastikan `make apps` sudah dijalankan dan /apps/login.elf ada\n");
        // Tidak ada UI sama sekali. Parkir di loop sleep daripada spin
        // (spin akan membakar CPU tanpa memberi info apa pun ke user).
        for (;;) sys_sleep(60000);
    }
    print("[init] login.elf di-spawn\n");

    // Loop supervisi. Blok di waitpid; setiap anak yang mati dicatat lalu
    // di-restart dengan backoff.
    for (;;) {
        int status = 0;
        int pid = sys_waitpid(-1, &status, 0);
        if (pid < 0) {
            // Tidak ada anak tersisa (semua sudah mati dan di-reap) atau
            // waitpid gagal. Beri napas lalu coba restart apa pun yang hilang.
            sys_sleep(1000);
        } else {
            for (int slot = 0; slot < SUP_MAX; slot++) {
                if (child_pid[slot] != pid) continue;
                child_pid[slot] = -1;
                child_restarts[slot]++;

                print("[init] anak mati: ");
                print((char*)kChildName[slot]);
                print(" (restart ke-");
                char nb[12]; int n = 0;
                int v = child_restarts[slot];
                if (v == 0) nb[n++] = '0';
                else { char r[12]; int k = 0; while (v) { r[k++] = (char)('0' + v % 10); v /= 10; } while (k) nb[n++] = r[--k]; }
                nb[n] = '\0';
                print(nb);
                print(")\n");

                sys_sleep(restart_delay_ms(child_restarts[slot]));
                if (spawn_child(slot) < 0) {
                    print("[init] restart gagal: ");
                    print((char*)kChildName[slot]);
                    print("\n");
                }
                break;
            }
        }
    }
}
