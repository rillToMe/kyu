// kernel/kernel.c — entry point kernel dan urutan boot.
//
// Struktur file ini:
//   1. Limine requests (section .requests — bootloader memindainya)
//   2. kernel_main(): urutan tahap boot, satu baris per fase
//   3. boot_phase_*(): isi tiap tahap
//   4. boot_handoff_to_init(): titik serah boot → init (lihat catatan K-1)
//
// Presentasi status boot ada di kernel/boot_console.c; konsol di kernel/kprint.c.

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
#include "kyuzenfs_v4.h"   // KZFS_CRASHDUMP_SECTORS (area crashdump di ekor disk)
#include "panic.h"          // panic_log_init/check_previous_log + crashdump_init
#include "crashdump.h"
#include "crash_archive.h"   // terbitkan crashdump sebagai berkas /crash-report.txt
#include "acpi.h"
#include "vfs.h"
#include "task.h"
#include "shell.h"
#include "lapic.h"
#include "smp.h"
#include "gfx.h"
#include "ghal.h"
#include "kyuzen_version.h"   // KYUZEN_VERSION kanonis (dipakai banner boot)
#include "kprint.h"
#include "boot_console.h"
#include "serial.h"
#include "elf.h"             // elf_load_file (memuat init.elf)
#include "proc.h"            // as_cookie_next

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

// HHDM offset: dipakai oleh paging.c untuk convert phys → virt
uint64_t hhdm_offset = 0;

extern void init_gdt();
extern void init_idt();
extern void pic_remap();
extern void init_keyboard();
extern void init_mouse();
extern int  kfs_delete_file(char* filename);   // Fase 4: 0 sukses / kode error
extern int  net_boot_summary(uint32_t* ip);    // kernel/net/net_init.c

// Hasil ghal_init() — dilaporkan di boot console, dibaca fase berikutnya.
static int ghal_ok = 0;

// Pesan kegagalan fatal saat boot: tidak ada jalur render / tidak ada RAM map.
// Halt dengan interrupts mati — tidak ada gunanya lanjut tanpa konsol.
static __attribute__((noreturn)) void boot_halt(void) {
    for (;;) { __asm__ volatile("cli; hlt"); }
}

static void serial_num(uint64_t num) {
    char b[24]; boot_u64_str(num, b); serial_print(b);
}

// FASE 1 — Layar, GDT/IDT, memori awal, proteksi CPU

// Tangkap framebuffer Limine + validasi format (bukan asumsi XRGB8888).
static void boot_phase_display(void) {
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count == 0) {
        boot_halt();
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    display_format_desc_t fdesc = {
        .bpp = fb->bpp,
        .memory_model = fb->memory_model,
        .red_size = fb->red_mask_size, .red_shift = fb->red_mask_shift,
        .green_size = fb->green_mask_size, .green_shift = fb->green_mask_shift,
        .blue_size = fb->blue_mask_size, .blue_shift = fb->blue_mask_shift,
    };

    // Serial dini: kegagalan validasi harus terlihat di host, bukan halt bisu.
    serial_init();
    serial_print("[DISPLAY] limine fb ");
    serial_num(fb->width); serial_print("x"); serial_num(fb->height);
    serial_print(" bpp="); serial_num(fb->bpp);
    serial_print(" model="); serial_num(fb->memory_model);
    serial_print(" masks=");
    serial_num(fb->red_mask_size);   serial_print("."); serial_num(fb->red_mask_shift);   serial_print(",");
    serial_num(fb->green_mask_size); serial_print("."); serial_num(fb->green_mask_shift); serial_print(",");
    serial_num(fb->blue_mask_size);  serial_print("."); serial_num(fb->blue_mask_shift);
    serial_print("\n");

    if (display_boot_init((uint32_t *)fb->address, fb->width, fb->height,
                          fb->pitch, &fdesc) != 0) {
        serial_print("[DISPLAY] FATAL: framebuffer format tidak didukung\n");
        boot_halt();
    }
}

static void boot_phase_cpu_tables(void) {
    init_gdt();
    init_idt();
}

// PMM dynamic via Limine + paging + heap + crash log + SMEP/SMAP/WP.
static void boot_phase_memory(void) {
    if (memmap_request.response == NULL) {
        kprint("PANIC: Bootloader tidak mengirim Memory Map!\n");
        boot_halt();
    }
    pmm_init_dynamic(memmap_request.response->entries,
                     memmap_request.response->entry_count);

    init_paging(0); // paging.c membaca CR3 langsung, parameter tidak dipakai
    init_heap();

    // CRASH LOG PERSISTEN (kernel/debug/panic_log.c). Halaman fisik PERTAMA yang
    // dialokasikan PMM: alamatnya deterministik di setiap boot, jadi log dari
    // boot sebelumnya (warm-reboot tidak menghapus DRAM) ketemu di tempat yang
    // sama. Disiapkan di sini supaya panic paling awal pun sudah tercatat.
    panic_log_init(pmm_alloc_page(), 4096u);
}

// FIX_005 Tahap 4: SMEP/SMAP di BSP + pastikan CR0.WP. AP mengaktifkan
// miliknya sendiri di smp_ap_main (CR4/CR0 per-core).
static void boot_phase_cpu_hardening(void) {
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

// FASE 2 — Interrupt controller, PCI, timer

static void boot_phase_interrupts(void) {
    pic_remap();
    lapic_init_bsp();

    extern void pci_probe(void);
    pci_probe();

    init_timer(TIMER_HZ);      // Inisialisasi PIT pada frekuensi dari timer.h
}

// FASE 3 — Graphics HAL, scheduler awal, input

// Graphics HAL diinisialisasi SEBELUM timer_callbacks_init, supaya
// compositor_flush (cb_flush) langsung punya backend + main surface.
// Software backend butuh tahu framebuffer hardware untuk present.
// compositor_ghal_init() membuat main surface DI SINI (konteks task),
// bukan lazy di dalam IRQ — surface_create memakai kmalloc + virtqueue.
static void boot_phase_graphics(void) {
    extern void ghal_set_framebuffer(uint32_t*, uint32_t, uint32_t, uint32_t);
    extern void compositor_ghal_init(void);
    const display_mode_t* dm = display_get_mode();
    ghal_set_framebuffer(fb_ptr, dm->width, dm->height, dm->pitch_bytes);
    ghal_ok = (ghal_init() == 0);
    if (ghal_ok) {
        // Mode authoritative dari backend aktif (software == Limine;
        // virtio-gpu == pmodes[0]). Gagal → mode boot tetap dipakai.
        display_sync_from_backend();
    } else {
        // Fallback: compositor langsung (jalur software). Diagnostik → serial.
        serial_print("[GHAL] init failed — compositor fallback ke jalur langsung\n");
    }
    // Buffer layar seukuran mode. Task context (kmalloc) — wajib sebelum
    // compositor/TTY menggambar. Gagal = fatal (tidak ada jalur render).
    if (display_alloc_buffers() != 0) {
        serial_print("[DISPLAY] FATAL: alokasi buffer layar gagal\n");
        boot_halt();
    }
    if (ghal_ok) compositor_ghal_init();
}

static void boot_phase_input(void) {
    init_mouse();
    init_keyboard();
    init_tty();
}

// ---- Boot console bersih (TTY baru siap di sini) ----
// Header + status tahap yang sudah dilalui. Semua kprint verbose sebelum
// init_tty adalah no-op, jadi layar dimulai dari sini. Status di bawah
// jujur: kernel hanya sampai baris ini bila tahap tsb berhasil (gagal=halt).
static void boot_phase_banner(void) {
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
    serial_init();
    serial_print("\n[SERIAL] ready\n");

    // Laporkan crash dari boot sebelumnya (kalau ada) ke serial + layar, lalu
    // bersihkan flag-nya. Di sini serial & kprint sudah siap dua-duanya.
    if (!panic_check_previous_log())
        serial_print("[PANIC_LOG] tidak ada crash pada boot sebelumnya\n");
}

// FASE 4 — Filesystem, crashdump, ACPI, VFS

static void boot_phase_filesystem(void) {
#ifdef HEAP_WATCH_DEBUG
    // Pasang hardware watchpoint DR0 (per-CPU!) di BSP SEBELUM AP online.
    // Setiap AP memasang DR0-nya sendiri di smp_ap_main().
    extern void heap_watch_set(uint64_t addr, int len_bytes);
    extern uint64_t heap_watch_target_addr;
    heap_watch_set(heap_watch_target_addr, 4);
    serial_print("[HEAP_WATCH] BSP DR0 watch magic @ target (4 bytes)\n");
#endif

    kprint_quiet = 1;   // redam log verbose kfs_init (mis. format disk) ke serial
    kfs_init();          // KyuzenFS V4: bcache + superblock + bitmap mount
    kprint_quiet = 0;
    boot_state("  OK  ", "Filesystem", "KyuzenFS V4 (extent)");

    // CRASHDUMP DISK (kernel/debug/crashdump.c): 8 sektor terakhir disk — area yang
    // SAMA dengan yang disisihkan KyuzenFS (KZFS_CRASHDUMP_SECTORS), jadi
    // snapshot panic tidak pernah menimpa data file. ATA polling murni.
    uint32_t total = ata_get_total_sectors();
    if (total > KZFS_CRASHDUMP_SECTORS) {
        crashdump_init((uint64_t)(total - KZFS_CRASHDUMP_SECTORS), KZFS_CRASHDUMP_SECTORS);
        serial_print("[CRASHDUMP] area siap di LBA ");
        serial_num(total - KZFS_CRASHDUMP_SECTORS);
        serial_print(" (+ " ); serial_num(KZFS_CRASHDUMP_SECTORS);
        serial_print(" sektor)\n");
    } else {
        serial_print("[CRASHDUMP] disk terlalu kecil - dinonaktifkan\n");
    }

    // CRASH ARCHIVE: kalau boot sebelumnya panic, snapshot mentah itu diterbitkan
    // sebagai berkas /crash-report.txt supaya bisa dibuka app GUI (fileman,
    // viewer, notepad) — bukan hanya dibaca di serial/terminal. Idempoten:
    // kalau isinya tidak berubah, tidak ada penulisan disk.
    crash_archive_publish();

    // ACPI minimal: hanya untuk power-off (aksi [S]) — parse FADT sekali di sini.
    acpi_early_init();

    vfs_init();
    vfs_task_init(0);   // P0 Phase 2: task 0 stdio (fd 0/1/2 -> TTY)
    boot_state("  OK  ", "VFS", 0);
}

// FASE 5 — SMP

static void boot_phase_smp(void) {
    // LAPIC BSP sudah aktif sejak lapic_init_bsp(); SMP membawa AP online.
    // Detail (BSP lapic id, cpu_count, AP per-CPU) → serial via smp.c.
    boot_state("  OK  ", "LAPIC", "online");
    kprint_quiet = 1;   // redam sisa log verbose ke serial
    smp_init(mp_request.response);
    kprint_quiet = 0;
    boot_state_num("  OK  ", "SMP", smp_online_cpu_count(), " CPUs");
}

// FASE 6 — Auto-install modul dari Limine

// Console: satu baris ringkas. Detail per-modul + pesan kfs → serial.
static void boot_phase_modules(void) {
    int mod_ok = 0, mod_fail = 0, mod_total = 0;
    kprint_quiet = 1;   // redam "File dihapus"/error kfs selama instalasi

    if (module_request.response != NULL && module_request.response->module_count > 0) {
        mod_total = (int)module_request.response->module_count;

        // Fase 3: pastikan folder /apps ada — app .elf/.app tinggal di sini.
        if (!kfs_exists("/apps")) kfs_create_folder("/apps");

        // Fase 4: pohon direktori standar. Idempotent (mkdir hanya bila belum
        // ada) dan parent-dulu — resolver kini menerima path bertingkat, jadi
        // satu panggilan per folder. Ini fondasi File Manager/Gallery/Text
        // Editor fase berikutnya: mereka cukup memakai API path biasa.
        static const char* const kDefaultDirs[] = {
            "/system", "/system/config", "/system/fonts",
            "/home", "/home/user",
            "/home/user/Documents", "/home/user/Downloads",
            "/home/user/Pictures",  "/home/user/Projects",
        };
        for (unsigned di = 0; di < sizeof(kDefaultDirs) / sizeof(kDefaultDirs[0]); di++)
            if (!kfs_exists((char*)kDefaultDirs[di]))
                kfs_create_folder((char*)kDefaultDirs[di]);

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
}

// FASE 7 — Network

static void boot_phase_network(void) {
    // Aktifkan interrupts SEBELUM masuk ke user code
    // Tanpa sti: timer IRQ tidak pernah fire, keyboard beku, OS freeze!
    __asm__ volatile("sti");

#ifdef GFX_SELFTEST
    // Phase 2A/2B: laporkan backend aktif + ukuran scanout ke serial.
    // ghal_init() sudah dipanggil di atas (sebelum timer_callbacks_init).
    {
        extern const char* ghal_active_backend_name(void);
        extern void ghal_scanout_size(uint32_t*, uint32_t*);
        uint32_t sw = 0, sh = 0;
        ghal_scanout_size(&sw, &sh);
        serial_print("[GFX SELFTEST] backend=");
        serial_print(ghal_active_backend_name());
        serial_print(" scanout=");
        serial_num(sw); serial_print("x"); serial_num(sh);
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
}

// FASE 8 — Serah terima boot → init (PID 1)

// Muat /apps/init.elf dan jalankan sebagai task ring-3 pertama.
//
// Sebelumnya kernel memanggil user_login() LANGSUNG di CPL 0 (K-1): login,
// shell, dan zen berjalan dengan privilege kernel penuh — uc->from_user == 0
// membuat validasi pointer user di-bypass (usercopy.c) dan sys_alloc memberi
// heap kernel, bukan uheap. Sekarang seluruh jalur user sudah keluar dari
// image kernel sebagai ELF (login/shell/zen/init), jadi tidak ada lagi kode
// user yang di-link ke kernel.
//
// Kernel TIDAK bisa menjalankan kode higher-half di CPL 3: heap kernel
// dipetakan US=0 (kernel/mm/heap.c:139), jadi iretq ke CS=0x1B langsung #PF.
// Karena itu init harus ELF terpisah, dimuat ke AS-nya sendiri.
//
// Setelah init berjalan, task 0 (kernel_main) menjadi idle: ia tidak pernah
// kembali ke user code, hanya membiarkan scheduler menjalankan task lain.
static void boot_handoff_to_init(void) {
    const char* kInitPath = "/apps/init.elf";

    if (!kfs_exists((char*)kInitPath)) {
        kprint("\n[BOOT] FATAL: /apps/init.elf tidak ada - tidak ada proses init.\n");
        kprint("[BOOT] Jalankan `make apps` lalu boot ulang.\n");
        boot_halt();
    }

    phys_addr_t init_as = vmm_create_address_space();
    if (init_as == PHYS_NULL) {
        kprint("\n[BOOT] FATAL: alokasi address space untuk init gagal.\n");
        boot_halt();
    }

    // elf_load_file menerjemahkan alamat user lewat CR3 aktif, jadi AS target
    // harus dipasang lebih dulu. Task 0 belum punya pml4_phys (memakai AS boot
    // Limine), jadi cukup switch CR3 — tidak ada state task yang perlu diubah.
    phys_addr_t saved_cr3 = vmm_read_cr3();
    vmm_switch_pml4(init_as);

    uint64_t init_stack_top = 0;
    uint64_t init_entry = elf_load_file((char*)kInitPath, &init_stack_top, init_as);

    vmm_switch_pml4(saved_cr3);

    if (init_entry == 0 || init_stack_top == 0) {
        vmm_destroy_address_space(init_as, 1);
        kprint("\n[BOOT] FATAL: init.elf gagal dimuat (ELF tidak valid?).\n");
        boot_halt();
    }

    // create_user_task memalsukan frame iretq dengan CS=0x1B / SS=0x23, jadi
    // init benar-benar mulai di CPL 3. Cred diwarisi dari task 0 (uid 0 = root),
    // yang memang benar untuk proses init.
    int tid = create_user_task(init_entry, init_stack_top, init_as,
                               as_cookie_next(), "init",
                               0, 0, NULL);
    if (tid < 0) {
        vmm_destroy_address_space(init_as, 1);
        kprint("\n[BOOT] FATAL: create_user_task(init) gagal (slot task penuh?).\n");
        boot_halt();
    }

    boot_state_num("  OK  ", "init", (uint64_t)tid, " (ring 3)");
    kprint("\nKyuzenOS ready.\n\n");
}

// ENTRY POINT
void kernel_main(void) {
    // 0. AMBIL HHDM OFFSET — WAJIB SEBELUM APA PUN (dipakai oleh paging.c)
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    } else {
        // Fallback: Limine default HHDM biasanya di 0xFFFF800000000000
        hhdm_offset = 0xFFFF800000000000ULL;
    }

    boot_phase_display();          // framebuffer + validasi format
    boot_phase_cpu_tables();       // GDT + IDT
    boot_phase_memory();           // PMM + paging + heap + crash log
    boot_phase_cpu_hardening();    // SMEP/SMAP/WP
    boot_phase_interrupts();       // PIC + LAPIC + PCI + PIT

    // Graphics HAL & surface harus siap SEBELUM timer_callbacks_init, karena
    // cb_flush (compositor) langsung dipakai timer. tasking_init mendaftarkan
    // kernel_main sebagai task 0.
    boot_phase_graphics();
    timer_callbacks_init();    // Daftarkan subscriber default (visual, cursor, flush)
    tasking_init();            // Daftarkan kmain sebagai task awal scheduler
    boot_phase_input();        // mouse + keyboard + TTY
    boot_phase_banner();       // header + status tahap yang sudah dilalui

    boot_phase_filesystem();   // KyuzenFS + crashdump + ACPI + VFS
    boot_phase_smp();          // AP online
    boot_phase_modules();      // auto-install dari Limine
    boot_phase_network();      // sti + lwIP + DHCP + socket layer

    boot_handoff_to_init();    // spawn init.elf (PID 1, ring 3) + banner "ready"

    // Task 0 (kernel_main) selesai. Ia TIDAK boleh halt dengan `cli; hlt`:
    // init.elf di-spawn ke runqueue CPU 0, jadi mematikan interrupt di CPU ini
    // akan membekukan timer -> scheduler tidak pernah preempt task 0 -> init
    // tidak pernah dijadwalkan (gejala: banner "ready" tampil lalu freeze).
    //
    // Jalur yang benar sama dengan AP di smp_ap_main (kernel/smp/smp.c:205):
    // pindah ke idle stack permanen CPU ini, lalu masuk scheduler_idle_loop()
    // yang memasang cpu_current_task[cpu] = -1 dan `sti; hlt` — interrupt tetap
    // hidup sehingga CPU bisa mengambil task begitu timer/IPI menjadwalkannya.
    // Keduanya noreturn.
    task_switch_to_idle_stack(task_idle_stack_top(0));
}
