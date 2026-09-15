#include <stdint.h>
#include <stddef.h>
#include "timer.h"
#include "limine.h"
#include "fs.h"
#include "tty.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "string.h"
#include "ata.h"
#include "kyuzenfs.h"
#include "vfs.h"
#include "task.h"
#include "shell.h"
#include "lapic.h"
#include "smp.h"
#include "gfx.h"
#include "ghal.h"   

#ifdef STRESS_TEST
#include "pmm_stress.h"
#include "pmm_valid.h"
#endif

#ifdef CONC_TEST
#include "conc_test.h"
#endif

#ifdef HEAP_STRESS_TEST
#include "heap_stress_test.h"
#endif

// LIMINE REQUESTS — Harus di section .requests agar bootloader bisa scan
__attribute__((used, section(".requests_start_marker")))
static volatile uint64_t __limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used)) static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

// HHDM: Limine memetakan SELURUH RAM fisik di offset ini
// Physical address P accessible di hhdm_offset + P
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

// MP/SMP: minta Limine menyiapkan semua CPU dan biarkan AP menunggu entry point.
__attribute__((used, section(".requests")))
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID,
    .revision = 0,
    .flags = 0
};

__attribute__((used, section(".requests_end_marker")))
static volatile uint64_t __limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;
// ============================================================

// HHDM offset: dipakai oleh paging.c untuk convert phys → virt
uint64_t hhdm_offset = 0;


extern void init_gdt();
extern void init_idt();
extern void pic_remap();
extern void init_keyboard();
extern void switch_to_user_mode(void (*user_func)());
extern void user_login();
extern void init_mouse();
extern void kfs_delete_file(char* filename);

// kprint didefinisikan di kernel/kyuzenfs.c (via write_fs & tty_node)
extern void kprint(const char* str);

void kprint_num(uint64_t num) {
    if (num == 0) { kprint("0"); return; }
    char buf[20]; int i = 18; buf[19] = '\0';
    while (num > 0) { buf[i--] = (num % 10) + '0'; num /= 10; }
    kprint(&buf[i + 1]);
}

// ============================================================
// BOOT CONSOLE — presentasi ringkas, diagnostik verbose ke serial.
// ============================================================
#define KYUZEN_VERSION "0.3.1"

extern void serial_print(const char* s);
extern int  kprint_quiet;                    // kernel/kyuzenfs.c
extern int  net_boot_summary(uint32_t* ip);  // kernel/net_init.c

static void boot_u64_str(uint64_t num, char* out);   // definisi di bawah

// Warna marker status (hanya marker yang diwarnai; teks tetap netral).
#define BOOT_C_OK    0x7DDB8A   // hijau
#define BOOT_C_FAIL  0xFF6B6B   // merah
#define BOOT_C_WARN  0xFFCC66   // kuning/amber
#define BOOT_C_INFO  0x7CC7FF   // biru muda (informasional)
#define BOOT_C_TEXT  0xFFFFFF   // teks netral

// Cetak marker "[  OK  ]" dengan warna sesuai state, lalu kembalikan fg netral.
static void boot_marker(const char* state) {
    int i = 0; while (state[i] == ' ') i++;
    uint32_t col = BOOT_C_TEXT;
    switch (state[i]) {
    case 'O': col = BOOT_C_OK;   break;
    case 'F': col = BOOT_C_FAIL; break;
    case 'W': col = BOOT_C_WARN; break;
    case 'I': col = BOOT_C_INFO; break;
    default: break;             // SKIP/dll → netral
    }
    tty_set_fg(col);
    kprint("["); kprint(state); kprint("] ");
    tty_set_fg(BOOT_C_TEXT);
}

static void boot_label(const char* label) {
    kprint(label);
}

// Satu baris status: "[  OK  ] Label" atau "[  OK  ] Label detail"
// (satu spasi biasa; tanpa kolom detail fixed-width). Marker berwarna.
static void boot_state(const char* state, const char* label, const char* detail) {
    boot_marker(state);
    boot_label(label);
    if (detail && detail[0]) { kprint(" "); kprint(detail); }
    kprint("\n");
}

static void boot_state_num(const char* state, const char* label, uint64_t v,
                           const char* suffix) {
    boot_marker(state);
    boot_label(label);
    kprint(" ");
    kprint_num(v);
    if (suffix) kprint(suffix);
    kprint("\n");
}

// IPv4 network byte order → "a.b.c.d" ke buffer (pola sama dengan net_init.c).
static void boot_ip_str(uint32_t a, char* out) {
    const char* sep = ".";
    int q = 0;
    for (int sh = 0; sh <= 24; sh += 8) {
        char nb[4]; boot_u64_str((uint64_t)((a >> sh) & 0xFF), nb);
        for (int j = 0; nb[j]; j++) out[q++] = nb[j];
        if (sh < 24) out[q++] = sep[0];
    }
    out[q] = '\0';
}

static void boot_u64_str(uint64_t num, char* out) {
    if (num == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[24]; int i = 0;
    while (num > 0 && i < 23) { tmp[i++] = (char)('0' + (num % 10)); num /= 10; }
    int j = 0; while (i > 0) out[j++] = tmp[--i]; out[j] = '\0';
}
static void serial_num(uint64_t num) {
    char b[24]; boot_u64_str(num, b); serial_print(b);
}

// ----> ENTRY POINT 64-BIT BERSIH <---
void kernel_main(void) {
    // 0. AMBIL HHDM OFFSET — WAJIB SEBELUM APA PUN (dipakai oleh paging.c)
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    } else {
        // Fallback: Limine default HHDM biasanya di 0xFFFF800000000000
        hhdm_offset = 0xFFFF800000000000ULL;
    }

    int ghal_ok = 0;   // hasil ghal_init(), dilaporkan di boot console

    // 1. TANGKAP LAYAR DARI LIMINE
    if (framebuffer_request.response != NULL && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        fb_ptr = (uint32_t *)fb->address;
        fb_width = fb->width;
        fb_height = fb->height;
        fb_pitch = fb->pitch;
    } else {
        while(1) { __asm__ volatile("hlt"); }
    }

    init_gdt();
    init_idt();

    // 2. PMM DYNAMIC VIA LIMINE
    if (memmap_request.response != NULL) {
        pmm_init_dynamic(memmap_request.response->entries, memmap_request.response->entry_count);
    } else {
        kprint("PANIC: Bootloader tidak mengirim Memory Map!\n");
        while(1) { __asm__ volatile("hlt"); }
    }

    init_paging(0); // paging.c membaca CR3 langsung, parameter tidak dipakai
    init_heap();

    // FIX_005 Tahap 4: SMEP/SMAP di BSP + pastikan CR0.WP. AP mengaktifkan
    // miliknya sendiri di smp_ap_main (CR4/CR0 per-core).
    {
        extern void cpu_enable_smap_smep(void);
        extern int  cpu_verify_wp(void);
        extern int  g_smap_enabled, g_smep_enabled;
        cpu_enable_smap_smep();
        int wp = cpu_verify_wp();
        kprint("[CPU] SMEP=");  kprint(g_smep_enabled ? "on" : "off");
        kprint(" SMAP=");       kprint(g_smap_enabled ? "on" : "off");
        kprint(" WP=");         kprint(wp ? "on" : "off");
        kprint("\n");
    }

    pic_remap();
    lapic_init_bsp();

    extern void pci_probe(void);
    pci_probe();

    init_timer(TIMER_HZ);      // Inisialisasi PIT pada frekuensi dari timer.h

    // Phase 2B: inisialisasi Graphics HAL SEBELUM timer_callbacks_init, supaya
    // compositor_flush (cb_flush) langsung punya backend + main surface.
    // Software backend butuh tahu framebuffer hardware untuk present.
    // compositor_ghal_init() membuat main surface DI SINI (konteks task),
    // bukan lazy di dalam IRQ — surface_create memakai kmalloc + virtqueue.
    {
        extern void ghal_set_framebuffer(uint32_t*, uint32_t, uint32_t, uint32_t);
        extern void compositor_ghal_init(void);
        ghal_set_framebuffer(fb_ptr, fb_width, fb_height, fb_pitch);
        ghal_ok = (ghal_init() == 0);
        if (ghal_ok) {
            compositor_ghal_init();
        } else {
            // Fallback: compositor langsung (jalur software). Diagnostik → serial.
            serial_print("[GHAL] init failed — compositor fallback ke jalur langsung\n");
        }
    }

    timer_callbacks_init();    // Daftarkan subscriber default (visual, cursor, flush)
    tasking_init();            // Daftarkan kmain sebagai task awal scheduler

    init_mouse();
    init_keyboard();
    init_tty();

    // ---- Boot console bersih (TTY baru siap di sini) ----
    // Header + status tahap yang sudah dilalui. Semua kprint verbose sebelum
    // init_tty adalah no-op, jadi layar dimulai dari sini. Status di bawah
    // jujur: kernel hanya sampai baris ini bila tahap tsb berhasil (gagal=halt).
    kprint("KyuzenOS " KYUZEN_VERSION " x86_64\n\n");
    boot_state("  OK  ", "CPU", 0);
    boot_state_num("  OK  ", "Memory", pmm_get_total_ram() / 1024 / 1024, " MB");
    boot_state("  OK  ", "Paging", 0);
    boot_state("  OK  ", "Heap", 0);
    boot_state("  OK  ", "Interrupts", 0);
    if (ghal_ok) boot_state("  OK  ", "Graphics", ghal_active_backend_name());
    else         boot_state(" WARN ", "Graphics", "fallback");
    boot_state("  OK  ", "Timer", 0);
    boot_state("  OK  ", "Input", 0);

    // Serial COM1 selalu diinit — panic dump (panic.c) dan watchdog memakainya,
    // bukan hanya mode HEAP_WATCH. Aman dipanggil kapan pun. Diinit SEBELUM
    // kfs/smp supaya kprint_quiet bisa mem-mirror diagnostik ke COM1.
    extern void serial_init(void);
    serial_init();
    serial_print("\n[SERIAL] ready\n");
#ifdef HEAP_WATCH_DEBUG
    // Pasang hardware watchpoint DR0 (per-CPU!) di BSP SEBELUM AP online.
    // Setiap AP memasang DR0-nya sendiri di smp_ap_main().
    extern void heap_watch_set(uint64_t addr, int len_bytes);
    extern uint64_t heap_watch_target_addr;
    heap_watch_set(heap_watch_target_addr, 4);
    serial_print("[HEAP_WATCH] BSP DR0 watch magic @ target (4 bytes)\n");
#endif

    kprint_quiet = 1;   // redam log verbose kfs_init (mis. format disk) ke serial
    kfs_init();
    kprint_quiet = 0;
    boot_state("  OK  ", "Filesystem", 0);

    vfs_init();
    vfs_task_init(0);   // P0 Phase 2: task 0 stdio (fd 0/1/2 -> TTY)
    boot_state("  OK  ", "VFS", 0);

    // LAPIC BSP sudah aktif sejak lapic_init_bsp(); SMP membawa AP online.
    // Detail (BSP lapic id, cpu_count, AP per-CPU) → serial via smp.c.
    boot_state("  OK  ", "LAPIC", "online");
    kprint_quiet = 1;   // redam sisa log verbose ke serial
    smp_init(mp_request.response);
    kprint_quiet = 0;
    boot_state_num("  OK  ", "SMP", smp_online_cpu_count(), " CPUs");

    // 3. AUTO-INSTALL MODUL DARI LIMINE
    // Console: satu baris ringkas. Detail per-modul + pesan kfs → serial.
    int mod_ok = 0, mod_fail = 0, mod_total = 0;
    kprint_quiet = 1;   // redam "File dihapus"/error kfs selama instalasi
    if (module_request.response != NULL && module_request.response->module_count > 0) {
        mod_total = (int)module_request.response->module_count;

        // Fase 3: pastikan folder /apps ada — app .elf/.app tinggal di sini.
        if (!kfs_exists("/apps")) kfs_create_folder("/apps");

        for (uint64_t i = 0; i < module_request.response->module_count; i++) {
            struct limine_file *mod = module_request.response->modules[i];
            uint64_t size = mod->size;

            char* raw_name = mod->path;
            if (raw_name == NULL) { mod_fail++; continue; }

            char clean_name[24];
            int k = 0;
            char* last_slash = raw_name;
            for (int j = 0; raw_name[j] != '\0'; j++) {
                if (raw_name[j] == '/') last_slash = &raw_name[j + 1];
            }
            for (int j = 0; last_slash[j] != '\0' && last_slash[j] != ' ' &&
                            last_slash[j] != '\n' && k < 22; j++) {
                clean_name[k] = last_slash[j]; k++;
            }
            clean_name[k] = '\0';
            if (k == 0) { mod_fail++; continue; }

            // Fase 3: .elf / .app → /apps/, yang lain (png, dll) tetap root.
            int nlen = k;
            int is_app = nlen > 4 &&
                         ((clean_name[nlen-3] == 'e' && clean_name[nlen-2] == 'l' && clean_name[nlen-1] == 'f') ||
                          (clean_name[nlen-3] == 'a' && clean_name[nlen-2] == 'p' && clean_name[nlen-1] == 'p'));
            char dest[32];
            int d = 0;
            if (is_app) { const char* ap = "/apps/"; for (int j = 0; ap[j] && d < 31; j++) dest[d++] = ap[j]; }
            for (int j = 0; clean_name[j] && d < 31; j++) dest[d++] = clean_name[j];
            dest[d] = '\0';

            if (kfs_exists(dest)) kfs_delete_file(dest);

            int res = kfs_create_file(dest, (char*)mod->address, size);
            if (res) mod_ok++; else mod_fail++;

            // Jejak lengkap → serial saja.
            serial_print("  [mod] ");
            serial_num(i + 1); serial_print("/"); serial_num(mod_total);
            serial_print(" "); serial_print(dest);
            serial_print(" "); serial_num(size); serial_print("B ");
            serial_print(res ? "OK\n" : "FAIL\n");
        }
    } else {
        serial_print("[mod] Limine tidak mengirim modul sama sekali\n");
        mod_fail = 1;
    }
    kprint_quiet = 0;

    // Status instalasi jujur (bukan selalu OK).
    if (mod_total == 0) {
        boot_state(" FAIL ", "Applications", "no modules received");
    } else if (mod_fail == 0) {
        boot_state_num("  OK  ", "Applications", (uint64_t)mod_ok, " modules");
    } else if (mod_ok > 0) {
        char det[40]; int q = 0;
        const char* msg = "failed: ";
        for (int j = 0; msg[j] && q < 24; j++) det[q++] = msg[j];
        char nb[16]; boot_u64_str((uint64_t)mod_fail, nb);
        for (int j = 0; nb[j] && q < 33; j++) det[q++] = nb[j];
        det[q++] = '/';
        boot_u64_str((uint64_t)mod_total, nb);
        for (int j = 0; nb[j] && q < 38; j++) det[q++] = nb[j];
        det[q] = '\0';
        boot_state(" WARN ", "Applications", det);
    } else {
        boot_state(" FAIL ", "Applications", "module installation failed");
    }

    // Aktifkan interrupts SEBELUM masuk ke user code
    // Tanpa sti: timer IRQ tidak pernah fire, keyboard beku, OS freeze!
    __asm__ volatile("sti");

#ifdef GFX_SELFTEST
    // Phase 2A/2B: laporkan backend aktif + ukuran scanout ke serial.
    // ghal_init() sudah dipanggil di atas (sebelum timer_callbacks_init).
    {
        extern const char* ghal_active_backend_name(void);
        extern void ghal_scanout_size(uint32_t*, uint32_t*);
        extern void serial_print(const char* s);
        uint32_t sw = 0, sh = 0;
        ghal_scanout_size(&sw, &sh);
        serial_print("[GFX SELFTEST] backend=");
        serial_print(ghal_active_backend_name());
        serial_print(" scanout=");
        {
            char tmp[8]; uint32_t v = sw; int idx = 0;
            if (v == 0) tmp[idx++] = '0';
            else { char r[8]; int n = 0; while (v) { r[n++] = (char)('0' + v % 10); v /= 10; } while (n) tmp[idx++] = r[--n]; }
            tmp[idx] = '\0'; serial_print(tmp); serial_print("x");
            v = sh; idx = 0;
            if (v == 0) tmp[idx++] = '0';
            else { char r[8]; int n = 0; while (v) { r[n++] = (char)('0' + v % 10); v /= 10; } while (n) tmp[idx++] = r[--n]; }
            tmp[idx] = '\0'; serial_print(tmp);
        }
        serial_print("\n");
    }
#endif

    // Inisialisasi network stack (lwIP + e1000 + DHCP).
    // WAJIB setelah sti karena DHCP wait loop menggunakan hlt
    // dan membutuhkan PIT IRQ untuk drive timer callbacks.
    // Log verbose [net]/[e1000] diredam ke serial; console cukup 2 baris.
    extern void net_init(void);
    kprint_quiet = 1;
    net_init();
    kprint_quiet = 0;

    uint32_t net_ip = 0;
    int nst = net_boot_summary(&net_ip);
    if (nst == 0) {
        boot_state(" FAIL ", "Network", "initialization failed");
    } else {
        boot_state("  OK  ", "Network", "e1000");
        char ipbuf[24]; boot_ip_str(net_ip, ipbuf);
        if (nst == 1) {
            boot_state("  OK  ", "DHCP", ipbuf);
        } else {
            char det[32]; int k = 0;
            while (ipbuf[k]) { det[k] = ipbuf[k]; k++; }
            const char* sfx = " (static)";
            for (int j = 0; sfx[j] && k < 30; j++) det[k++] = sfx[j];
            det[k] = '\0';
            boot_state(" INFO ", "DHCP", det);
        }
    }

    extern void ksock_init(void);
    ksock_init();
    boot_state("  OK  ", "Socket layer", 0);

    kprint("\nKyuzenOS ready.\n\n");

#ifdef STRESS_TEST
    valid_start();
    stress_start();
    while (1) __asm__ volatile("hlt");
#endif

#ifdef CONC_TEST
    conc_test_run();
    while (1) __asm__ volatile("hlt");
#endif

#ifdef HEAP_STRESS_TEST
    test_heap_stress_run_all();
    while (1) __asm__ volatile("hlt");
#endif

    switch_to_user_mode(user_login);

    __asm__ volatile("cli");
    while (1) { __asm__ volatile("hlt"); }
}

extern fs_node_t tty_node;
uint32_t string_length(const char* str) {
    uint32_t len = 0;
    while (str[len]) len++;
    return len;
}
void print_hex(uint32_t num) { kprint_num(num); }


void switch_to_user_mode(void (*user_func)()) {
    // Ring 3 belum diimplementasikan — panggil langsung di Ring 0
    // (Syscall via int $0x80 tetap bekerja karena IDT sudah di-setup)
    if (user_func) user_func();
}