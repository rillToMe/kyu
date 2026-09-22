// kernel/panic/panic.c — orchestrator BSOD KyuzenOS.
//
// Modul panic dipecah empat (lihat kernel/panic/panic_internal.h):
//   panic.c         (file ini) : lockdown, entry point, persistensi, loop operator
//   panic_draw.c               : mesin gambar + target gambar (scanout/framebuffer)
//   panic_hw.c                 : polling PIT2/PS2 tanpa IRQ + reset/reboot/shutdown
//   panic_explain.c            : verdict fault, klasifikasi RIP, backtrace
//
// PRINSIP
//   * Tanpa heap, tanpa lock, tanpa IRQ. Alamat yang dibaca (stack/RBP chain)
//     selalu dicek dulu dengan paging_is_mapped_nolock() supaya tidak
//     double-fault — rinciannya di panic_explain.c.
//   * TARGET GAMBAR = SCANOUT DEVICE, FRAMEBUFFER SEBAGAI FALLBACK. Sejak
//     backend virtio-gpu memegang scanout-nya sendiri, menulis ke framebuffer
//     Limine tidak pernah terlihat lagi (desktop tetap terpampang walau panic
//     sudah "menggambar"). Karena itu target dipilih SEKALI di
//     panic_target_begin(): memori scanout device (ghal_scanout_map) bila ada,
//     kalau tidak → framebuffer hardware. Perubahan pada target scanout
//     dikirim dengan p_flush() — lihat bagian TARGET di panic_draw.c.
//   * LOCKDOWN. panic_lockdown() dipanggil paling awal. Semua penulis scanout
//     lain (compositor_flush, HUD/kursor timer callback, mouse_handler)
//     early-return selama panic_is_locked(). Tanpa ini, di sistem SMP CPU lain
//     tetap merender desktop di atas layar panic — gejalanya "BSOD glitch lalu
//     hilang saat mouse digerakkan" (panic hanya `cli` di CPU yang fault).
//   * HIERARKI INFORMASI (dari atas ke bawah = dari yang paling berguna):
//        PENYEBAB  verdict + petunjuk tindakan
//        DI MANA   task/CPU + RIP (di mana fault terjadi)
//        DETAIL    semua angka mentah (warna redup) — arsip, bukan headline
//   * ANTI-FLICKER. Target gambar adalah scanout (tanpa double buffer), jadi
//     baris statis digambar SEKALI dan hanya bagian yang berubah (baris status,
//     pita panic bersarang) yang ditimpa. Jangan kembali ke pola "clear +
//     redraw baris penuh tiap iterasi loop" — itu terlihat sebagai hilang-timbul
//     (dan di jalur device berarti transfer penuh tiap iterasi).
//   * TANPA IRQ. Waktu di-polling dari PIT channel 2 dan tombol dari controller
//     PS/2 — keduanya di panic_hw.c. Tidak ada timer_get_ms() untuk menghitung
//     mundur dan tidak pernah `sti` lagi.
//   * INTERAKTIF. [R] reboot, [S] shutdown (ACPI S5 → port emulator). Petunjuk
//     tombol = baris PALING BAWAH. TIDAK ada auto-reboot: reboot otomatis
//     membuat bukti crash hilang sebelum dibaca.
//   * PERSISTENSI (dua jalur, saling melengkapi):
//       - RAM : log ringkas di halaman PMM pertama (deterministik lintas
//               warm-reboot) -> dilaporkan panic_check_previous_log() saat boot;
//       - DISK: crashdump raw 4KB ke ekor disk (kernel/debug/crashdump.c) supaya
//               selamat dari power cycle.
//   * GUARD PANIC BERSARANG. Kalau panic terjadi lagi saat handler masih
//     berjalan (mis. I/O crashdump memicu fault), crashdump dibatalkan dan
//     sistem langsung dibekukan — tidak menggambar/menulis apa pun lagi.
//
// HOST TEST
//   Compile dengan -DPANIC_HOST_TEST (lihat test/panic_test.c): keempat file
//   di-include langsung, instruksi privileged di-stub, jam diganti jam palsu.
//   `./test/panic_test --dump` men-decode framebuffer kembali menjadi teks
//   memakai font8x16 asli, jadi tata letak BSOD bisa diperiksa tanpa boot QEMU.
//
// CATATAN STRING: font layar ASCII-only (panic_draw_char memetakan >127 ke
// '?'), jadi JANGAN pakai em dash / karakter box-drawing di string BSOD.

#include <stdint.h>

#include "panic_internal.h"
#include "crashdump.h"  // crashdump_* — snapshot ke sektor disk
#include "serial.h"     // serial_print (mirror COM1)

// =====================================================================
// 0. LOCKDOWN & GUARD PANIC BERSARANG
// =====================================================================
static volatile uint32_t g_panic_lockdown = 0;
// Guard panic bersarang: 1 = handler panic sedang berjalan pada CPU ini.
static volatile uint32_t g_panic_active = 0;

void panic_lockdown(void) { g_panic_lockdown = 1; }
int  panic_is_locked(void) { return (int)g_panic_lockdown; }

#ifdef PANIC_HOST_TEST
// Seam uji: bersihkan seluruh state antar skenario — lockdown & guard milik
// file ini, target gambar milik panic_draw.c, jam/tombol/counter milik
// panic_hw.c. (Di kernel tidak ada yang mereset: satu panic berakhir di
// reboot/power-off.)
void panic_host_test_reset(void) {
    g_panic_lockdown = 0;
    g_panic_active = 0;
    panic_target_reset();
    panic_hw_reset();
}
#endif

// =====================================================================
// 1. PERSISTENSI: log RAM + crashdump disk (keduanya zero-allocation)
// =====================================================================
#define PANIC_DUMP_TEXT_MAX 3072u      // payload crashdump (4KB - header)
static char g_dump_text[PANIC_DUMP_TEXT_MAX];

// Penulis teks minimal ke buffer tetap (tanpa libc, tanpa alokasi).
static uint32_t dt_put(char* b, uint32_t cap, uint32_t at, const char* s) {
    while (*s && at + 1u < cap) b[at++] = *s++;
    b[at] = 0;
    return at;
}

static uint32_t dt_dec(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    char tmp[24];
    uint32_t n = 0;
    if (v == 0) { return dt_put(b, cap, at, "0"); }
    while (v && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && at + 1u < cap) { b[at++] = tmp[--n]; }
    b[at] = 0;
    return at;
}

static uint32_t dt_hex(char* b, uint32_t cap, uint32_t at, uint64_t v) {
    static const char H[] = "0123456789ABCDEF";
    if (at + 19u >= cap) return at;
    b[at++] = '0'; b[at++] = 'x';
    for (int i = 15; i >= 0; i--) b[at++] = H[(v >> (i * 4)) & 0xFull];
    b[at] = 0;
    return at;
}

// Nama task (kalau tabel task belum siap / tid tidak valid -> "").
static void panic_task_name(int tid, char* out, uint32_t cap) {
    out[0] = 0;
    if (cap < 2u || tid < 0) return;
    if ((uint32_t)tid >= MAX_TASKS) return;
    const char* n = tasks[tid].name;
    uint32_t i = 0;
    while (i + 1u < cap && n[i]) { out[i] = n[i]; i++; }
    out[i] = 0;
}

// Simpan snapshot ke RAM log + crashdump disk. Zero-allocation: seluruh buffer
// statis, tanpa heap.
static void panic_persist(uint64_t vector, uint64_t error_code, const registers_t* r,
                          uint64_t cr2, uint64_t cr3, int tid, uint32_t uptime_s,
                          const panic_cause_t* cause, const char* title,
                          const char* desc, int kind) {
    char tname[24];
    panic_task_name(tid, tname, sizeof(tname));

    // 1) Log RAM — deterministik lintas warm-reboot (halaman PMM pertama).
    panic_log_write(vector, error_code, r ? r->rip : 0, cr2, panic_clock_ms(),
                    (uint64_t)(tid < 0 ? 0xFFFFFFFFu : (uint32_t)tid),
                    tname, (uint32_t)kind);

    // 2) Crashdump disk — polling ATA, tanpa IRQ/scheduler.
    if (!crashdump_ready()) return;

    uint32_t at = 0;
    g_dump_text[0] = 0;
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "KyuzenOS panic snapshot\n");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "kind  : ");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at,
                kind ? "kernel_panic() manual" : "exception");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nvector: ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, vector);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  err ");
    at = dt_hex(g_dump_text, sizeof(g_dump_text), at, error_code);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
    if (title) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "cause : ");
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, title);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
    }
    if (cause) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "verdict: ");
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, cause->verdict);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        if (cause->hint1 && cause->hint1[0]) {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "hint1 : ");
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, cause->hint1);
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        }
        if (cause->hint2 && cause->hint2[0]) {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "hint2 : ");
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, cause->hint2);
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        }
    }
    if (desc) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "desc  : ");
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, desc);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
    }
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "task  : #");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, (uint64_t)(tid < 0 ? 0 : (uint32_t)tid));
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, " \"");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, tname[0] ? tname : "(tanpa nama)");
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\"\nuptime: ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, uptime_s);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, " s\n");

    if (r) {
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "rip   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rip);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  cs ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->cs);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  cr3 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, cr3);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  cr2 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, cr2);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nrsp   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rsp);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rbp ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rbp);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rflags ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rflags);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nrax   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rax);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rbx ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rbx);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rcx ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rcx);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rdx ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rdx);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nrsi   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rsi);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  rdi ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->rdi);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r8  ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r8);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r9  ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r9);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nr10   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r10);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r11 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r11);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r12 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r12);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r13 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r13);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\nr14   : ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r14);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  r15 ");
        at = dt_hex(g_dump_text, sizeof(g_dump_text), at, r->r15);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "  int ");
        at = dt_dec(g_dump_text, sizeof(g_dump_text), at, r->int_num);
        at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");

        uint64_t bt[6];
        uint32_t n = panic_bt_collect(r->rbp, r->rsp, r->cs, bt, 6u);
        if (n == 0) {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at,
                        "bt    : (tidak ada frame valid)\n");
        } else {
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "bt    :");
            for (uint32_t i = 0; i < n; i++) {
                at = dt_put(g_dump_text, sizeof(g_dump_text), at, " ");
                if (bt[i] >= KERNEL_VMA) {
                    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "KERNEL+");
                    at = dt_hex(g_dump_text, sizeof(g_dump_text), at, bt[i] - KERNEL_VMA);
                } else {
                    at = dt_hex(g_dump_text, sizeof(g_dump_text), at, bt[i]);
                }
            }
            at = dt_put(g_dump_text, sizeof(g_dump_text), at, "\n");
        }
    }

    at = dt_put(g_dump_text, sizeof(g_dump_text), at, "lastsc: ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at, g_last_syscall_num);
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, " (task ");
    at = dt_dec(g_dump_text, sizeof(g_dump_text), at,
                (uint64_t)(g_last_syscall_task < 0 ? 0 : (uint32_t)g_last_syscall_task));
    at = dt_put(g_dump_text, sizeof(g_dump_text), at, ")\n");

    // Header crashdump: semua field diisi eksplisit (tanpa memset).
    crashdump_hdr_t h;
    h.magic            = CRASHDUMP_MAGIC;
    h.version          = CRASHDUMP_VERSION;
    h.flags            = 0;
    h.length           = 0;
    h.checksum         = 0;
    h.sectors          = 0;
    h.dump_count       = 0;
    h.timestamp_ms     = panic_clock_ms();
    h.exception_vector = vector;
    h.error_code       = error_code;
    h.rip              = r ? r->rip : 0;
    h.cr2              = cr2;
    h.rsp              = r ? r->rsp : 0;
    h.cs               = r ? r->cs : 0;
    h.cr3              = cr3;
    h.task_id          = (uint64_t)(tid < 0 ? 0xFFFFFFFFu : (uint32_t)tid);
    for (uint32_t i = 0; i < sizeof(h.task_name); i++) h.task_name[i] = 0;
    for (uint32_t i = 0; i + 1u < sizeof(h.task_name) && tname[i]; i++) h.task_name[i] = tname[i];

    int rc = crashdump_write_snapshot(&h, g_dump_text, at);
    serial_print("\n[PANIC] crashdump: ");
    switch (rc) {
        case  0: serial_print("tersimpan di ekor disk\n"); break;
        case -2: serial_print("DIBATALKAN (dump lain masih aktif)\n"); break;
        case -3: serial_print("DIBATALKAN (panic bersarang)\n"); break;
        case -4: serial_print("GAGAL verifikasi baca-balik\n"); break;
        default: serial_print("tidak tersedia\n"); break;
    }
}

// =====================================================================
// 2. LOOP INTERAKTIF (tanpa IRQ) — TANPA AUTO-REBOOT
// =====================================================================
// SENGAJA tidak ada hitung mundur / reboot otomatis: sistem berhenti di layar
// BSOD dan menunggu operator memilih tindakan. Reboot otomatis membuat bukti
// crash hilang sebelum dibaca (dan di QEMU menghasilkan boot-loop yang
// menyembunyikan penyebab pertama — persis yang bikin diagnosis lambat).
//
// Baris info (di atas baris tombol): dua varian sesuai hasil sync FS.
static const char INFO_HOLD[]   = "TIDAK ada reboot otomatis - sistem menunggu pilihan tombol";
static const char INFO_NOSYNC[] = "TIDAK ada reboot otomatis - sync FS dilewati (lock dipakai)";
// Petunjuk tombol = baris PALING BAWAH, supaya mata langsung menemukan
// pilihan tindakan.
static const char HINT_KEYS[] = "[R] reboot   [S] shutdown";

static void panic_interactive(void) {
    // Target dipilih di sini juga (bukan hanya di jalur penggambar layar):
    // panic_interactive dipanggil walau tidak ada yang bisa digambar, dan
    // pemilihan target bersifat idempotent.
    (void)panic_target_begin();
    int have_ui = (panic_screen_h() >= PANIC_UI_MIN_HEIGHT && panic_target_ready());

    // Input sepenuhnya milik loop ini: matikan IRQ keyboard/mouse (CPU lain
    // masih melayani keduanya dan akan menelan scancode-nya), baru buang sisa
    // byte lama. Urutan ini penting — drain dulu = byte baru ikut dibuang.
    panic_input_irq_mask();
    panic_monotonic_reset();
    panic_keys_drain();

    // Sync FS SEKALI di sini (best-effort, try-lock). Tanpa auto-reboot,
    // inilah jaring pengaman data: operator bisa mematikan daya kapan saja
    // setelah membaca layar. Kalau CPU yang fault memegang lock FS, sync
    // dilewati — jangan pernah menunggu lock di jalur panic.
    int fs_ok = kfs_sync_all_try();
    if (!fs_ok)
        serial_print("[PANIC] sync FS dilewati (lock sedang dipakai) - menunggu tombol\n");

    if (have_ui) {
        // Dua baris statis digambar SEKALI (scanout tanpa double buffer:
        // menggambar ulang tiap iterasi tampak berkedip) lalu dikirim ke device.
        p_line(panic_screen_h() - 48u, fs_ok ? INFO_HOLD : INFO_NOSYNC, C_KEY);
        p_line(panic_screen_h() - 28u, HINT_KEYS, C_FG);
        p_flush();
    }

#ifdef PANIC_HOST_TEST
    uint32_t host_loops = 0;
#endif

    for (;;) {
        // Input pengguna (polling PS/2, tanpa IRQ). Aksi tidak kembali di
        // kernel; `return` di bawah hanya tercapai di host test.
        int k = panic_read_key();
        if (k == 'r') { panic_action_reboot();   return; }
        if (k == 's') { panic_action_shutdown(); return; }

        // Kalau pengiriman sebelumnya dilewati (cmd_lock sedang dipegang CPU
        // lain — bisa jadi CPU yang fault), coba lagi di kesempatan ini.
        // No-op begitu kotak kotor kosong (device sudah menerima semuanya).
        p_flush();
        panic_idle_slice();     // ~5-10 ms sambil mem-polling PIT2

#ifdef PANIC_HOST_TEST
        // Di kernel loop ini terminal. Host test butuh jalan keluar setara
        // supaya bisa memverifikasi "tanpa tombol = tidak ada reboot".
        if (++host_loops >= PANIC_HOST_IDLE_LOOPS) return;
#endif
    }
}

// =====================================================================
// 3. SERIAL MIRROR
// =====================================================================
static void ser_task_context(void) {
    int tid = smp_current_task_id();
    serial_print("\nTASK: ");
    if (tid >= 0 && tid < MAX_TASKS) {
        ser_dec((uint64_t)tid);
        serial_print(" (");
        serial_print(tasks[tid].name[0] ? tasks[tid].name : "(tanpa nama)");
        serial_print(") ");
        serial_print(tasks[tid].kind == TASK_KIND_SPAWNED ? "user" : "kernel");
    } else {
        serial_print("idle/ISR");
    }
    serial_print("  UPTIME(s): ");
    ser_dec(panic_clock_ms() / 1000u);
    serial_print("  LAST SYSCALL: ");
    ser_dec(g_last_syscall_num);
    serial_print(" (task ");
    ser_dec((uint64_t)(g_last_syscall_task < 0 ? 0 : g_last_syscall_task));
    serial_print(")");
}

static void ser_regs(const registers_t* r) {
    serial_print("\nRIP: "); ser_hex(r->rip);
    serial_print("  RSP: "); ser_hex(r->rsp);
    serial_print("  CS: ");  ser_hex(r->cs);
    serial_print("  SS: ");  ser_hex(r->ss);
    serial_print("\nRAX: "); ser_hex(r->rax);
    serial_print("  RBX: "); ser_hex(r->rbx);
    serial_print("  RCX: "); ser_hex(r->rcx);
    serial_print("  RDX: "); ser_hex(r->rdx);
    serial_print("\nRSI: "); ser_hex(r->rsi);
    serial_print("  RDI: "); ser_hex(r->rdi);
    serial_print("  RBP: "); ser_hex(r->rbp);
    serial_print("  R8:  "); ser_hex(r->r8);
    serial_print("\nR9:  "); ser_hex(r->r9);
    serial_print("  R10: "); ser_hex(r->r10);
    serial_print("  R11: "); ser_hex(r->r11);
    serial_print("  R12: "); ser_hex(r->r12);
    serial_print("\nR13: "); ser_hex(r->r13);
    serial_print("  R14: "); ser_hex(r->r14);
    serial_print("  R15: "); ser_hex(r->r15);
    serial_print("\nCR3: "); ser_hex(panic_read_cr3());
    serial_print("\n");
}

// =====================================================================
// 4. LAYAR BSOD — EXCEPTION
// =====================================================================
void exception_handler(registers_t* r) {
    if (!r) return;

    panic_cli();
    serial_enter_panic_mode();   // TX UART dibatasi: tidak boleh menggantung

#ifdef HEAP_WATCH_DEBUG
    // Vector 1 (#DB): hardware watchpoint dari heap_watch — log ke serial dan
    // lanjutkan eksekusi (stub akan iretq), bukan BSOD.
    if (r->int_num == 1) {
        extern void heap_watch_db_handler(registers_t* r);
        heap_watch_db_handler(r);
        return;
    }
#endif

    // Jejak paling awal ke COM1 (dikirim SEBELUM lockdown/persistensi). Kalau
    // sistem membeku tanpa layar BSOD, baris ini di serial.log yang memisahkan
    // "fault yang menggantung di dalam handler" dari "hang biasa — tidak ada
    // fault sama sekali". Satu baris, tanpa lock, aman di konteks panic.
    serial_print("\n[PANIC] handler masuk (cpu ");
    ser_dec((uint64_t)smp_current_cpu_index());
    serial_print(")\n");

    // GUARD PANIC BERSARANG. Handler ini bisa dipanggil lagi selagi yang
    // sebelumnya masih berjalan (contoh: penulisan crashdump memicu fault baru).
    // Dalam kondisi itu jangan menggambar/menulis apa pun lagi: batalkan
    // crashdump lalu bekukan sistem supaya layar BSOD tetap utuh dibaca.
    if (g_panic_active) {
        crashdump_abort();
        serial_print("\n[PANIC] panic BERSARANG - crashdump dibatalkan, freeze.\n");
        // Jangan freeze secara SENYAP: layar panic pertama tetap ada, tapi
        // beri tanda di layar supaya jelas kenapa tidak ada dump/reboot.
        panic_overlay_banner("PANIC BERSARANG - crashdump dibatalkan (lihat serial COM1)");
        panic_freeze();
        return;
    }
    g_panic_active = 1;

    int repeat = panic_is_locked();
    panic_lockdown();                     // hentikan semua penulis scanout lain
    serial_print("\n[P1] lockdown aktif - laporan serial mulai\n");

    uint64_t int_num    = r->int_num;
    uint64_t error_code = r->error_code;
    uint64_t cr2        = (int_num == 14) ? panic_read_cr2() : 0;
    uint64_t cr3        = panic_read_cr3();
    uint64_t uptime_s   = panic_clock_ms() / 1000u;

    // --- mirror serial (selalu verbose; ini tempat dump lengkap) ---
    serial_print("\n\n==== KERNEL PANIC (serial dump) ====\nEXCEPTION: ");
    serial_print(int_num < 15 ? exception_names[int_num] : "Unknown");
    serial_print("\nINT: ");
    ser_dec(int_num);
    serial_print("  ERR: ");
    ser_hex(error_code);
    if (int_num == 14) {
        serial_print("\nCR2: ");
        ser_hex(cr2);
        serial_print("  PF: ");
        serial_print(paging_is_mapped_nolock(cr2) ? "PROT" : "NONP");
        serial_print(error_code & 2u ? " WRITE" : " READ");
        serial_print(error_code & 4u ? " USER" : " KERNEL");
    }
    ser_regs(r);
    ser_task_context();
    serial_print("\n");

    panic_cause_t cause = panic_explain(int_num, error_code, cr2, r->rip);

    // --- URUTAN PENTING: GAMBAR DULU, SIMPAN BELAKANGAN -------------------
    // Persistensi menyentuh disk (ATA) dan RAM reserved; keduanya bisa lambat
    // atau macet di hardware bermasalah. Kalau persistensi dijalankan lebih
    // dulu, kegagalan/macet di situ membuat sistem tampak "freeze tanpa
    // panic": layar desktop tetap terpampang, BSOD tidak pernah muncul, dan
    // tidak ada reboot. Dengan gambar-dulu, laporan visual SELALU ada; artefak
    // crash (RAM log + crashdump disk) tetap ditulis sebelum loop tombol.
    // Target gambar: scanout device (virtio) dulu, framebuffer sebagai
    // fallback. Tidak ada target sama sekali = lapor ke serial saja.
    if (panic_target_begin() != 0) {                // tidak bisa menggambar
        serial_print("[P2] tidak ada target gambar - lapor ke serial saja\n");
        panic_persist(int_num, error_code, r, cr2, cr3, smp_current_task_id(),
                      (uint32_t)uptime_s, &cause, NULL, NULL, 0);
        serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");
        panic_interactive();          // tanpa layar: hanya tombol
        g_panic_active = 0;           // hanya tercapai di host test
        return;
    }

    fill_screen(C_BG);
    p_home();
    p_banner(repeat);
    p_newline();

    // --- PENYEBAB (headline) -----------------------------------------
    p_label("PENYEBAB");
    p_str(cause.verdict, C_TITLE);
    p_newline();
    if (cause.hint1) { p_cont_begin(); p_str(cause.hint1, C_KEY); p_newline(); }
    if (cause.hint2) { p_cont_begin(); p_str(cause.hint2, C_DIM); p_newline(); }
    p_newline();

    // --- DI MANA ------------------------------------------------------
    p_label("DI MANA");
    int tid = smp_current_task_id();
    if (tid >= 0 && tid < MAX_TASKS) {
        p_str("task #", C_DIM);
        p_dec((uint64_t)tid, C_KEY);
        if (tasks[tid].name[0]) {
            p_str(" \"", C_DIM);
            p_str(tasks[tid].name, C_FG);
            p_str("\" (", C_DIM);
            p_str(tasks[tid].kind == TASK_KIND_SPAWNED ? "user" : "kernel", C_DIM);
            p_str(")", C_DIM);
        } else {
            p_str(" (tanpa nama)", C_DIM);
        }
    } else {
        p_str("(tidak ada task aktif - konteks idle/ISR)", C_DIM);
    }
    p_str("   CPU ", C_DIM);
    p_dec(smp_current_cpu_index(), C_KEY);
    p_str("/", C_DIM);
    p_dec(smp_online_cpu_count(), C_FG);
    p_newline();

    p_cont_begin();
    p_str("RIP ", C_DIM);
    p_hex(r->rip, C_KEY);
    p_str(panic_rip_class(r->rip, r->cs), panic_rip_class_color(r->rip, r->cs));
    p_str("   uptime ", C_DIM);
    p_dec(uptime_s, C_FG);
    p_str("s", C_DIM);
    p_newline();
    p_newline();

    // --- DETAIL (arsip; sengaja redup) --------------------------------
    p_rule("DETAIL (buka hanya bila perlu)");

    p_label("FAULT");
    p_str(int_num < 15 ? exception_names[int_num] : "Unknown", C_FG);
    p_str(" (INT ", C_DIM);
    p_dec(int_num, C_FG);
    p_str(")  ERR ", C_DIM);
    p_hex(error_code, C_FG);
    p_str("  [P=", C_DIM);
    p_str((error_code & 1u) ? "1" : "0", C_FG);
    p_str(" W=", C_DIM);
    p_str((error_code & 2u) ? "1" : "0", C_FG);
    p_str(" U=", C_DIM);
    p_str((error_code & 4u) ? "1" : "0", C_FG);
    p_str(" RSVD=", C_DIM);
    p_str((error_code & 8u) ? "1" : "0", C_FG);
    p_str(" ID=", C_DIM);
    p_str((error_code & 16u) ? "1" : "0", C_FG);
    p_str("]", C_DIM);
    p_newline();

    if (int_num == 14) {
        p_label("TARGET");
        p_str("CR2 ", C_DIM);
        p_hex(cr2, C_KEY);
        p_str("   ", C_DIM);
        if (!panic_is_canonical(cr2))
            p_str("NON-KANONIKAL (alamat mustahil)", C_TITLE);
        else if (error_code & 1u)
            p_str("PROTECTION VIOLATION (halaman ada, akses ditolak)", C_FG);
        else
            p_str("NOT PRESENT (alamat belum termap)", C_FG);
        p_str("   ", C_DIM);
        p_str((error_code & 4u) ? "USER" : "KERNEL/HIGHER-HALF", C_DIM);
        if (error_code & 16u)      p_str("  INSTRUCTION FETCH", C_DIM);
        else if (error_code & 2u)  p_str("  OP: WRITE", C_DIM);
        else                       p_str("  OP: READ", C_DIM);
        p_newline();

        // Bit P dari CPU itu otoritatif; beda dengan paging_is_mapped() bisa
        // berarti TLB/CR3 tidak sinkron (atau alamat milik AS task lain).
        if (paging_is_mapped_nolock(cr2) != (int)(error_code & 1u)) {
            p_cont_begin();
            p_str("catatan: paging_is_mapped() menilai berbeda dari bit P (TLB/CR3?)", C_FAINT);
            p_newline();
        }
    } else if (int_num == 13) {
        p_label("GPF");
        if (error_code == 0) {
            p_str("null selector atau alamat non-kanonik", C_TITLE);
            p_newline();
        } else {
            uint64_t tbl = (error_code >> 1) & 3u;
            p_str("selector index ", C_DIM);
            p_dec((error_code >> 3) & 0x1FFFu, C_KEY);
            p_str(tbl == 0 ? " (GDT)" : (tbl == 2 ? " (LDT)" : " (IDT)"), C_DIM);
            p_newline();
        }
    } else {
        p_label("TARGET");
        p_str("CS ", C_DIM);
        p_hex(r->cs, C_FG);
        p_str("   RFLAGS ", C_DIM);
        p_hex(r->rflags, C_FG);
        p_newline();
    }

    p_backtrace(r->rbp, r->rsp, r->cs);

    // --- REGISTERS (4 baris x 4 register) -----------------------------
    p_label("REGISTERS");
    p_str("RAX ", C_DIM); p_hex(r->rax, C_FG);
    p_str(" RBX ", C_DIM); p_hex(r->rbx, C_FG);
    p_str(" RCX ", C_DIM); p_hex(r->rcx, C_FG);
    p_str(" RDX ", C_DIM); p_hex(r->rdx, C_FG);
    p_newline();

    p_cont_begin();
    p_str("RSI ", C_DIM); p_hex(r->rsi, C_FG);
    p_str(" RDI ", C_DIM); p_hex(r->rdi, C_FG);
    p_str(" RBP ", C_DIM); p_hex(r->rbp, C_FG);
    p_str(" R8  ", C_DIM); p_hex(r->r8, C_FG);
    p_newline();

    p_cont_begin();
    p_str("R9 ", C_DIM);  p_hex(r->r9, C_FG);
    p_str(" R10 ", C_DIM); p_hex(r->r10, C_FG);
    p_str(" R11 ", C_DIM); p_hex(r->r11, C_FG);
    p_str(" R12 ", C_DIM); p_hex(r->r12, C_FG);
    p_newline();

    p_cont_begin();
    p_str("R13 ", C_DIM); p_hex(r->r13, C_FG);
    p_str(" R14 ", C_DIM); p_hex(r->r14, C_FG);
    p_str(" R15 ", C_DIM); p_hex(r->r15, C_FG);
    p_str(" RSP ", C_DIM); p_hex(r->rsp, C_FG);
    p_newline();

    // --- STATE & MEM ---------------------------------------------------
    p_label("STATE");
    p_str("CR3 ", C_DIM);  p_hex(cr3, C_FG);
    p_str(" CS ", C_DIM);  p_hex(r->cs, C_FG);
    p_str("   tasks ", C_DIM); p_dec((uint64_t)task_count, C_FG);
    if (tid >= 0 && tid < MAX_TASKS && tasks[tid].parent_id >= 0) {
        p_str("   parent #", C_DIM);
        p_dec((uint64_t)tasks[tid].parent_id, C_FG);
    }
    p_str("   last syscall ", C_DIM);
    p_dec(g_last_syscall_num, C_KEY);
    p_str(" (task ", C_DIM);
    p_dec((uint64_t)(g_last_syscall_task < 0 ? 0 : g_last_syscall_task), C_FG);
    p_str(")", C_DIM);
    p_newline();

    p_label("MEM");
    p_dec(pmm_get_used_pages_nolock(), C_KEY);
    p_str(" / ", C_DIM);
    p_dec(pmm_get_total_pages_nolock(), C_FG);
    p_str(" pages (", C_DIM);
    p_dec((pmm_get_used_pages_nolock() * 4096u) / 1024u, C_KEY);
    p_str(" / ", C_DIM);
    p_dec((pmm_get_total_pages_nolock() * 4096u) / 1024u, C_FG);
    p_str(" KB)", C_DIM);
    p_newline();

    // Layar sudah utuh (BSOD; baris info & tombol digambar panic_interactive).
    // Kirim ke device bila targetnya scanout device (no-op di jalur
    // framebuffer). Dijalankan SEBELUM persistensi supaya laporan visual tidak
    // pernah menunggu disk.
    p_flush();
    serial_print("[P2] layar BSOD selesai digambar\n");

    // Persistensi setelah layar: RAM log + crashdump disk (zero-allocation).
    panic_persist(int_num, error_code, r, cr2, cr3, smp_current_task_id(),
                  (uint32_t)uptime_s, &cause, NULL, NULL, 0);
    serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");

    // Loop operator: [R] reboot / [S] shutdown via polling PS/2.
    // Di kernel rutin ini TIDAK PERNAH kembali (selalu berakhir reboot/freeze);
    // baris setelahnya hanya tercapai di host test.
    panic_interactive();
    g_panic_active = 0;
}

// =====================================================================
// 5. GENERIC KERNEL PANIC (dipanggil manual dari kode kernel)
// =====================================================================
__attribute__((weak))
void kernel_panic(const char* title, const char* desc, uint64_t code) {
    panic_cli();                          // seluruh jalur panic: IRQ mati
    serial_enter_panic_mode();            // TX UART dibatasi (lihat serial.h)

    serial_print("\n[PANIC] handler masuk (kernel_panic, cpu ");
    ser_dec((uint64_t)smp_current_cpu_index());
    serial_print(")\n");

    // GUARD PANIC BERSARANG (sama seperti exception_handler): kalau handler
    // masih berjalan, jangan menggambar/menulis apa pun lagi.
    if (g_panic_active) {
        crashdump_abort();
        serial_print("\n[PANIC] panic BERSARANG - crashdump dibatalkan, freeze.\n");
        panic_overlay_banner("PANIC BERSARANG - crashdump dibatalkan (lihat serial COM1)");
        panic_freeze();
        return;
    }
    g_panic_active = 1;

    int repeat = panic_is_locked();
    panic_lockdown();
    serial_print("\n[P1] lockdown aktif - laporan serial mulai\n");

    // --- mirror serial ---
    serial_print("\n\n==== KERNEL PANIC (serial dump) ====\nREASON: ");
    serial_print(title ? title : "(null)");
    serial_print("\nDETAILS: ");
    serial_print(desc ? desc : "(null)");
    serial_print("\nCODE: ");
    ser_hex(code);
    serial_print("\n");
    ser_task_context();
    serial_print("\n");

    // (Gambar-dulu, simpan-belakangan: lihat catatan panjang di
    //  exception_handler. Panic manual tidak punya registers_t, jadi artefak
    //  berisi judul + detail + konteks task saja.)
    if (panic_target_begin() != 0) {
        serial_print("[P2] tidak ada target gambar - lapor ke serial saja\n");
        panic_persist(0xFFFFu, code, 0, 0, panic_read_cr3(), smp_current_task_id(),
                      (uint32_t)(panic_clock_ms() / 1000u), 0, title, desc, 1);
        serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");
        panic_interactive();
        g_panic_active = 0;           // hanya tercapai di host test
        return;
    }

    fill_screen(C_BG);
    p_home();
    p_banner(repeat);
    p_newline();

    p_label("PENYEBAB");
    p_str(title ? title : "(tanpa judul)", C_TITLE);
    p_newline();
    p_cont_begin();
    p_str(desc ? desc : "(tanpa keterangan)", C_KEY);
    p_newline();
    p_newline();

    p_label("DI MANA");
    int tid = smp_current_task_id();
    if (tid >= 0 && tid < MAX_TASKS && tasks[tid].name[0]) {
        p_str("task #", C_DIM);
        p_dec((uint64_t)tid, C_KEY);
        p_str(" \"", C_DIM);
        p_str(tasks[tid].name, C_FG);
        p_str("\"", C_DIM);
    } else {
        p_str("(tidak ada task aktif - konteks idle/ISR)", C_DIM);
    }
    p_str("   CPU ", C_DIM);
    p_dec(smp_current_cpu_index(), C_KEY);
    p_str("/", C_DIM);
    p_dec(smp_online_cpu_count(), C_FG);
    p_newline();
    p_newline();

    p_rule("DETAIL (buka hanya bila perlu)");

    p_label("CODE");
    p_hex(code, C_KEY);
    p_newline();

    p_label("PETUNJUK");
    p_str("dump stack trace lengkap ada di serial COM1 (-serial stdio)", C_DIM);
    p_newline();

    // Tidak ada registers_t: tidak ada yang bisa ditelusuri (lihat
    // panic_explain.c — penghitung frame tetap milik file itu).
    panic_backtrace_unavailable();

    p_label("STATE");
    p_str("CR3 ", C_DIM);
    p_hex(panic_read_cr3(), C_FG);
    p_str("   tasks ", C_DIM);
    p_dec((uint64_t)task_count, C_FG);
    p_str("   last syscall ", C_DIM);
    p_dec(g_last_syscall_num, C_KEY);
    p_newline();

    p_label("MEM");
    p_dec(pmm_get_used_pages_nolock(), C_KEY);
    p_str(" / ", C_DIM);
    p_dec(pmm_get_total_pages_nolock(), C_FG);
    p_str(" pages terpakai", C_DIM);
    p_newline();

    p_flush();   // kirim ke scanout device bila targetnya device (lihat panic_draw.c)
    serial_print("[P2] layar BSOD selesai digambar\n");
    panic_persist(0xFFFFu, code, 0, 0, panic_read_cr3(), smp_current_task_id(),
                  (uint32_t)(panic_clock_ms() / 1000u), 0, title, desc, 1);
    serial_print("[P3] persist selesai\n[P4] loop tombol/countdown\n");

    // Loop operator: [R] reboot / [S] shutdown via polling PS/2.
    // Di kernel rutin ini TIDAK PERNAH kembali (selalu berakhir reboot/freeze);
    // baris setelahnya hanya tercapai di host test.
    panic_interactive();
    g_panic_active = 0;
}

// =====================================================================
// 6. SHORTCUT / KOMPATIBILITAS
// =====================================================================
void page_fault_handler(registers_t* r, uint64_t fault_addr) {
    (void)fault_addr;   // CR2 sudah dibaca langsung di exception_handler
    exception_handler(r);
}
