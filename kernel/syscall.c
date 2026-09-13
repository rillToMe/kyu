#include "fs.h"
#include <stdint.h>
#include "userlib.h"
#include "task.h"
#include "paging.h"
#include "smp.h"
#include "vfs.h"
#include "net_socket.h"
#include "elf.h"
#include "usercopy.h"
#include "uheap.h"
#include "smap.h"
#include "ghal.h"   // Phase 2C §9.6: sys_gpu_stats — lewat kontrak HAL, bukan driver core (rule §5.5)

// registers_t is provided by task.h — must match PUSHA64 in isr_macro.inc

extern fs_node_t tty_node;
extern uint32_t string_length(const char* str);
extern uint32_t write_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern uint32_t read_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
extern void tty_clear(void);
extern void yield(void); 

// Impor fungsi KyuzenFS & Heap
extern void kfs_format(void);
extern void kfs_list_files(void);
extern void kfs_read_file(char* filename);
extern void kfs_delete_file(char* filename);
extern void* kmalloc(uint32_t size);
extern void kfree(void* ptr);
extern void* krealloc(void* ptr, uint32_t old_size, uint32_t new_size);
extern int kfs_exists(char* filename);
extern uint32_t kfs_get_file_size(char* filename);
extern int kfs_read_to_buffer(char* filename, char* out_buffer, uint32_t buffer_capacity);
extern int kfs_create_file(char* filename, char* data, uint32_t size);
extern int kfs_get_file_list(char* path, void* buffer, int max_entries);
extern int kfs_create_folder(char* path);

extern void get_cpu_string(char* buffer);
extern uint64_t pmm_get_used_ram(void);
extern uint64_t pmm_get_total_ram(void);
extern uint32_t kfs_get_total_space(void);
extern uint32_t kfs_get_used_space(void);
extern uint32_t get_cpu_usage(void);
// Counter: setiap kali sys_yield dipanggil, tambah counter ini.
// Timer membaca dan mereset setiap tick untuk menentukan apakah CPU idle.
volatile uint32_t yield_counter = 0;

// Impor dari KWM (Kyuzen Window Manager)
extern void draw_pixel(uint32_t x, uint32_t y, uint32_t color);
extern void screen_mark_dirty(int32_t x, int32_t y, uint32_t width, uint32_t height);
extern void draw_image(int start_x, int start_y, int width, int height, uint32_t* buffer);
extern void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t color);
extern void kwm_set_cursor(int kind);  // Phase 9: bentuk kursor global
// Phase 10 — desktop window + taskbar (syscall 59-62)
extern int kwm_create_desktop(void);
extern int kwm_set_title(int win_id, const char* title);
extern int kwm_get_windows(kwm_window_info_t*, int);
extern int kwm_activate_window(int win_id);
// Phase 10 — sys_get_screen_size (syscall 63); didefinisikan di kernel/gfx/fb.c
extern uint32_t fb_width;
extern uint32_t fb_height;

#include "timer.h"  // timer_get_ticks(), timer_get_cpu_usage()

int current_uid = 0; // Definisi global — UID proses yang sedang berjalan

// ========================================================
// HANDLER SYSCALL 64-BIT
// (Dipanggil oleh isr128_stub saat aplikasi melempar int 0x80)
// ========================================================
// Per-address-space cookie generator: increments for each new AS.
// FIX_003: increment atomik (SMP) — dua exec bersamaan tidak boleh
// mendapat nomor cookie yang sama. Identitas cookie itu sendiri disimpan
// PER-TASK (task_t.cookie), bukan di global.
static uint32_t as_cookie_counter = 0;

static inline uint32_t as_cookie_next(void) {
    return __sync_fetch_and_add(&as_cookie_counter, 1) + 1;
}

static task_t* syscall_current_task(void) {
    int task_id = smp_current_task_id();
    if (task_id < 0 || task_id >= task_count) return NULL;
    return &tasks[task_id];
}

void syscall_handler(registers_t *r) {
    task_t* syscall_task = syscall_current_task();

    // Nomor Syscall selalu ada di RAX
    uint64_t syscall_num = r->rax;
    uint64_t ret_val = 0; // Default return

    // FIX_005 Tahap 2: konteks boundary copy — pointer dari ring 3
    // divalidasi & di-copy; caller ring 0 (shell/login kernel via int 0x80)
    // lewat jalur bypass (pointer kernel sah).
    ucopy_ctx_t uc;
    ucopy_ctx_init(&uc, r);

    // --- Pemetaan Argumen Standar Kyuzen OS 64-bit ---
    // RAX = Nomor Syscall
    // RBX = Argumen 1
    // RCX = Argumen 2
    // RDX = Argumen 3
    // RSI = Argumen 4
    // RDI = Argumen 5

    if (syscall_num == 1) { // sys_print
        // Tahap 2: copy-in bounded — strlen tak berbatas pada pointer user hilang.
        char kstr[UC_MAX_STR];
        int64_t n = strncpy_from_user(&uc, kstr, r->rbx, sizeof(kstr));
        if (n > 0) write_fs(&tty_node, 0, (uint32_t)n, (uint8_t*)kstr);
    }
    else if (syscall_num == 2) { // sys_clear_screen
        tty_clear();
    } 
    else if (syscall_num == 3) { // sys_read_keyboard
        // Phase 5B: flush kbd_buffer/event queue DIHAPUS dari titik baca ini.
        // Routing keyboard kini eksklusif (window fokus → event queue; tidak
        // ada fokus → TTY), jadi tidak ada lagi "sisa input app lama" yang
        // harus dibersihkan di sini. Flush-per-baca juga race: karakter yang
        // tiba di antara dua panggilan read_keyboard (mis. saat shell sibuk
        // mengeksekusi perintah) terbuang percuma. Transisi lifecycle
        // (exec/spawn/exit) tetap flush di jalurnya masing-masing.
        // Tahap 2: baca ke buffer kernel dulu — tulisan ke pointer user tidak
        // lagi terjadi di dalam kbd_lock (IRQ off). Validasi SEBELUM read
        // yang blocking, supaya input tidak terlanjur dikonsumsi lalu gagal.
        uint32_t want = (uint32_t)r->rcx;
        if (want > UC_MAX_KBD) want = UC_MAX_KBD;
        if (want > 0 && user_range_ok(&uc, r->rbx, want)) {
            uint8_t kbuf[UC_MAX_KBD];
            uint32_t n = read_fs(&tty_node, 0, want, kbuf);
            if (n > 0) copy_to_user(&uc, r->rbx, kbuf, n);
            ret_val = n;
        }
    }

    else if (syscall_num == 4) { // sys_yield
        yield_counter++; // Tandai CPU idle untuk CPU usage tracker
        // FIX_005 Tahap 1: hlt dilakukan di sisi kernel — hlt privileged
        // (CPL=0), app ring-3 yang mengeksekusinya sendiri kena #GP.
        // PENTING: gate int 0x80 (0xEE) adalah INTERRUPT gate — IF dimatikan
        // saat entry. sti WAJIB sebelum hlt atau CPU tidur selamanya.
        __asm__ volatile("sti; hlt");
    }
    else if (syscall_num == 5) { // sys_fs_format
        kfs_format();
    }
    else if (syscall_num == 6) { // sys_fs_list
        kfs_list_files();
    }
    else if (syscall_num == 7) { // sys_fs_read
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0) kfs_read_file(kf);
    }
    else if (syscall_num == 8) { // sys_fs_delete
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0) kfs_delete_file(kf);
    }
    else if (syscall_num == 9) { // sys_alloc
        // FIX_005 Tahap 3: ring 3 → region user range milik AS caller
        // (kernel/uheap.c) — pointer yang lewat boundary bukan lagi alamat
        // heap kernel. Ring 0 (shell/login/zen) tetap kmalloc.
        if (uc.from_user) {
            ret_val = uheap_alloc(syscall_task, r->rbx);
        } else {
            ret_val = (uint64_t)kmalloc((uint32_t)r->rbx);
        }
    }
    else if (syscall_num == 10) { // sys_free
        // Tahap 3: kepemilikan divalidasi — pointer asing/stale diabaikan,
        // metadata allocator hidup di heap kernel (tak tersentuh app).
        if (uc.from_user) {
            uheap_free(syscall_task, r->rbx);
        } else {
            kfree((void*)r->rbx);
        }
    }
    else if (syscall_num == 11) { // sys_file_exists
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0) {
            ret_val = kfs_exists(kf);
        }
    }
    else if (syscall_num == 12) { // sys_file_size
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0) {
            ret_val = kfs_get_file_size(kf);
        }
    }
    else if (syscall_num == 13) { // sys_read_file_to_buffer
        // Tahap 2: baca ke bounce kernel, copy-out di luar fs_lock.
        // Semantik lama dipertahankan: size > capacity → 0; file ada → 1.
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0) {
            uint32_t cap   = (uint32_t)r->rdx;
            uint32_t fsize = kfs_get_file_size(kf);
            if (fsize == 0) {
                // File kosong / tidak ada — buffer user tidak disentuh.
                ret_val = kfs_exists(kf) ? 1 : 0;
            } else if (fsize <= cap && fsize <= UC_MAX_FILE &&
                       user_range_ok(&uc, r->rcx, fsize)) {
                char* bounce = (char*)kmalloc(fsize);
                if (bounce) {
                    if (kfs_read_to_buffer(kf, bounce, fsize) &&
                        copy_to_user(&uc, r->rcx, bounce, fsize) == 0) {
                        ret_val = 1;
                    }
                    kfree(bounce);
                }
            }
        }
    }
    else if (syscall_num == 14) { // sys_uptime → returns ms sejak boot (hardware-agnostic)
        ret_val = timer_get_ms();
    }

    else if (syscall_num == 15) { // sys_total_ram
        ret_val = pmm_get_total_ram();
    }
    else if (syscall_num == 16) { // sys_used_ram
        ret_val = pmm_get_used_ram();
    }
    else if (syscall_num == 17) { // get_cpu_string
        // get_cpu_string menulis tepat 49 byte (48 brand CPUID + NUL).
        char kcpu[64];
        get_cpu_string(kcpu);
        copy_to_user(&uc, r->rbx, kcpu, 49);
    }
    else if (syscall_num == 18) { // sys_create_file
        char kf[UC_MAX_FNAME];
        uint32_t size = (uint32_t)r->rdx;
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0 && size <= UC_MAX_FILE) {
            if (size == 0) {
                char empty = '\0';
                ret_val = kfs_create_file(kf, &empty, 0);
            } else {
                char* bounce = (char*)kmalloc(size);
                if (bounce) {
                    if (copy_from_user(&uc, bounce, r->rcx, size) == 0) {
                        ret_val = kfs_create_file(kf, bounce, size);
                    }
                    kfree(bounce);
                }
            }
        }
    }
    else if (syscall_num == 19) { // sys_realloc
        // Tahap 3 ring 3: ukuran lama dilacak kernel per region — argumen
        // old_size (rcx) dari app tidak dipercaya lagi.
        if (uc.from_user) {
            ret_val = uheap_realloc(syscall_task, r->rbx, r->rdx);
        } else {
            ret_val = (uint64_t)krealloc((void*)r->rbx, (uint32_t)r->rcx, (uint32_t)r->rdx);
        }
    }
    else if (syscall_num == 20) { // sys_get_time
        extern void rtc_read_time(uint32_t*);
        uint32_t ktime[6];   // [year, month, day, hour, min, sec]
        rtc_read_time(ktime);
        copy_to_user(&uc, r->rbx, ktime, sizeof(ktime));
    }
    else if (syscall_num == 21) {
        // Reserved/Unused
    }
    else if (syscall_num == 22) { // sys_draw_pixel
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
            if (user_range_ok(&uc, r->rdi, bytes)) {
                // Tahap 4: buffer user dibaca langsung di dalam draw_image
                // (pengecualian shared #2) → jendela SMAP selama blit.
                if (uc.from_user) user_access_begin();
                draw_image((int)r->rbx, (int)r->rcx, w, h, (uint32_t*)r->rdi);
                if (uc.from_user) user_access_end();
            }
        }
    }
    else if (syscall_num == 24) { // sys_get_file_list(path, buffer, max_entries)
        // Fase 2: argumen path user-space. RBX=path, RCX=buffer, RDX=maxn.
        // Tulis ke bounce kernel, copy-out setelah fs_lock lepas.
        char kpath[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kpath, r->rbx, sizeof(kpath)) >= 0) {
            int maxn = (int)r->rdx;
            if (maxn > (int)UC_MAX_ENTRIES) maxn = (int)UC_MAX_ENTRIES;
            uint64_t bytes = (uint64_t)maxn * sizeof(file_info_t);
            if (maxn > 0 && user_range_ok(&uc, r->rcx, bytes)) {
                file_info_t* bounce = (file_info_t*)kmalloc((uint32_t)bytes);
                if (bounce) {
                    int count = kfs_get_file_list(kpath, bounce, maxn);
                    if (count > 0 &&
                        copy_to_user(&uc, r->rcx, bounce,
                                     (uint64_t)count * sizeof(file_info_t)) != 0) {
                        count = 0;
                    }
                    ret_val = (uint64_t)count;
                    kfree(bounce);
                }
            }
        }
    }
    else if (syscall_num == 25) { // sys_load_elf
        // FIX Tahap 2 (UAF): copy filename SEBELUM AS lama dihancurkan dan
        // CR3 pindah ke AS baru — sebelumnya string user di-deref SETELAH
        // switch, membaca alamat dari AS yang sudah tidak ada.
        char kfname[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kfname, r->rbx, sizeof(kfname)) < 0) {
            r->rax = 0;
            return;
        }

        // Flush KEDUA buffer sebelum app baru jalan (Phase 5B: queue per-task)
        extern void flush_event_queue(int task_id);
        extern void flush_kbd_buffer(void);
        flush_event_queue(smp_current_task_id());
        flush_kbd_buffer();

        // --- PER-PROCESS ISOLATION ---
        // Create a fresh address space for the new user app.
        // current_pml4 ALWAYS stays as kernel PML4. Target PML4 untuk ELF
        // load diteruskan eksplisit ke elf_load_file (FIX_002), lalu CR3
        // di-switch ke AS baru.
        phys_addr_t new_pml4 = vmm_create_address_space();
        if (new_pml4 != PHYS_NULL) {
            // Clean old user pages if this task had a previous AS
            task_t *self = syscall_current_task();
            if (self && self->pml4_phys != 0) {
                vmm_destroy_task_as(self->pml4_phys);
            }

            if (self) {
                // Tahap 3: region heap user mati bersama AS lama — buang
                // metadata + mulai brk segar untuk AS baru.
                uheap_reset(self);
                self->pml4_phys = new_pml4;
                self->cookie = as_cookie_next();
            }

            // Switch CR3 to the user PML4 BEFORE loading. elf_load_file copies
            // segment bytes directly to user virtual addresses (e.g. 0x4000000)
            // via memcpy, which the CPU translates through the *current* CR3.
            // The freshly-allocated pages are mapped into new_pml4, so CR3 must
            // already point there or the copy faults on an unmapped address.
            // Kernel higher-half (code/stack/heap/HHDM) is cloned into new_pml4,
            // so kernel execution continues safely after the switch.
            vmm_switch_pml4(new_pml4);
        }

        // Tahap 3: stack ter-map di user range AS baru — bebas bersama AS,
        // tidak ada lagi tracking kfree.
        uint64_t new_stack_top = 0;
        ret_val = elf_load_file(kfname, &new_stack_top, new_pml4);
    }

    else if (syscall_num == 26) { // sys_draw_string
        // Tahap 2: bukan bagian pengecualian canvas — string kecil, di-copy.
        char kstr[UC_MAX_STR];
        if (strncpy_from_user(&uc, kstr, r->rbx, sizeof(kstr)) >= 0) {
            draw_string(kstr, (int)r->rcx, (int)r->rdx, (uint32_t)r->rsi);
        }
    }
    // --- SYSCALL: MULTI-USER IDENTITY ---
    else if (syscall_num == 27) { // sys_set_uid
        current_uid = r->rbx; 
    }
    else if (syscall_num == 28) { // sys_get_uid
        ret_val = current_uid;   
    }
    // --- SYSCALL: EVENT QUEUE UNTUK GUI ---
    else if (syscall_num == 29) { // sys_get_event
        // Tahap 2: validasi out-pointer SEBELUM event dikonsumsi (event tidak
        // hilang sia-sia); pop ke buffer kernel, copy-out DI LUAR event_lock.
        if (!user_range_ok(&uc, r->rbx, sizeof(kyuzen_event_t))) {
            r->rax = 0;
            return;
        }
        kyuzen_event_t kev;

        // Phase 5B: pop dari queue PER-TASK pemanggil — event sudah di-route
        // KWM (keyboard→fokus, mouse/wheel→window di bawah kursor). Fallback
        // sintesis MOUSE_MOVE dihapus: move asli kini terkirim per posisi, dan
        // fallback itu sumber kebocoran event lintas app. Queue kosong →
        // return 0; app tetap polling + sys_yield seperti biasa.
        extern int pop_event(int task_id, kyuzen_event_t* out);
        if (pop_event(smp_current_task_id(), &kev)) {
            copy_to_user(&uc, r->rbx, &kev, sizeof(kev));
            r->rax = 1;
            return; // Event berhasil diambil dari queue — langsung return
        }
        r->rax = 0;
        return;
    }

    // --- SYSCALL BARU UNTUK KWM (Window Manager) ---
    else if (syscall_num == 30) { // sys_kwm_create_window
        extern int kwm_create_window(int, int, uint32_t, uint32_t);
        ret_val = kwm_create_window((int)r->rbx, (int)r->rcx, (uint32_t)r->rdx, (uint32_t)r->rsi);
    }
    else if (syscall_num == 31) { // sys_kwm_update_window
        // PENGECUALIAN TERDOKUMENTASI #1 (FIX_004 + Tahap 2): canvas sampai
        // 16MB/frame — copy per frame mahal, tetap SHARED. Tapi: range source
        // divalidasi di sini, dan owner divalidasi di kwm_update_window.
        extern void kwm_update_window(int, uint32_t*);
        extern uint64_t kwm_window_canvas_bytes(int);
        uint64_t cbytes = kwm_window_canvas_bytes((int)r->rbx);
        if (cbytes > 0 && user_range_ok(&uc, r->rcx, cbytes)) {
            kwm_update_window((int)r->rbx, (uint32_t*)r->rcx);
        }
    }
    else if (syscall_num == 32) { // sys_kwm_destroy_window
        extern void kwm_destroy_window(int);
        extern int kwm_window_owner(int);
        int wid = (int)r->rbx;
        // FIX_004: app hanya boleh menghancurkan window miliknya sendiri.
        if (kwm_window_owner(wid) == smp_current_task_id()) {
            kwm_destroy_window(wid);
        }
    }
    else if (syscall_num == 33) { // sys_exec — load & jalankan ELF baru, replace current app
        // PENTING: copy filename ke kernel stack DULU sebelum unmap!
        // Tahap 2: lewat strncpy_from_user (tervalidasi); gagal → keluar
        // SEBELUM window/AS caller disentuh.
        char kfname[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kfname, r->rbx, sizeof(kfname)) < 0) {
            r->rax = 0;
            return;
        }
#ifdef HEAP_WATCH_DEBUG
        {
            extern void serial_print(const char* s);
            extern void serial_print_hex(uint64_t v);
            serial_print("[EXEC] kfname=[");
            serial_print(kfname);
            serial_print("] rbx=");
            serial_print_hex(r->rbx);
            serial_print(" cs=");
            serial_print_hex(r->cs);
            serial_print("\n");
        }
#endif

        // 0. Destroy KWM windows milik TASK INI saja (FIX_004) — compositor
        //    tetap aman tanpa menghancurkan window milik task lain.
        extern void kwm_destroy_windows_of(int);
        kwm_destroy_windows_of(smp_current_task_id());

        // 1. Destroy current address space and switch back to kernel PML4
        {
            task_t *self = syscall_current_task();

            if (self && self->pml4_phys != 0) {
                vmm_destroy_task_as(self->pml4_phys);
                self->pml4_phys = 0;
            }
            // Tahap 3: stack & heap user hidup di AS — frame ikut bebas di
            // vmm_destroy_task_as; sisa metadata heap user dibuang di sini.
            uheap_reset(self);
            // pml4_phys == 0: task memakai boot/kernel AS. User range PML4 boot
            // milik Limine — membebaskannya mencemari free list PMM dengan
            // halaman reserved/ROM <72MB (akar BOSD heap corruption).
        }

        // 1b. Flush input buffers so new app doesn't inherit old keystrokes
        //     (Phase 5B: event queue per-task — flush milik task ini saja)
        {
            extern void flush_event_queue(int task_id);
            extern void flush_kbd_buffer(void);
            flush_event_queue(smp_current_task_id());
            flush_kbd_buffer();
        }

        // 2. Create fresh AS for the new app being exec'd
        phys_addr_t new_pml4 = PHYS_NULL;
        {
            new_pml4 = vmm_create_address_space();
            if (new_pml4 != PHYS_NULL) {
                task_t *self = syscall_current_task();
                if (self) {
                    self->pml4_phys = new_pml4;
                    self->cookie = as_cookie_next();
                }

                // Switch CR3 to the user PML4 BEFORE loading — elf_load_file
                // copies segment bytes to user virtual addresses via memcpy,
                // translated through the current CR3. The target pages live in
                // new_pml4, so CR3 must point there or the copy page-faults.
                vmm_switch_pml4(new_pml4);
            }
        }

        // 3. Load ELF baru ke slot 0x4000000 — target PML4 eksplisit (FIX_002)
        // Tahap 3: stack di user range AS baru, dibebaskan bersama AS.
        uint64_t new_stack_top = 0;
        uint64_t entry = elf_load_file(kfname, &new_stack_top, new_pml4);

        // 3. Set RIP & RSP untuk IRETQ
        extern uint64_t g_shell_return_rsp;
        if (entry != 0) {
            r->rip = entry;
            // New app runs on its own 256KB stack (deep decode chains overflow
            // the shared shell stack). Fallback to shell RSP if alloc failed.
            r->rsp = (new_stack_top != 0) ? new_stack_top : g_shell_return_rsp;
            // FIX_005 Tahap 1: masuk CPL 3 — iretq memuat segmen user.
            // Kernel tetap bisa dijangkau via int 0x80 (gate DPL=3 + TSS.RSP0).
            r->cs = 0x1B;  // user code (GDT[3] | RPL3)
            r->ss = 0x23;  // user data (GDT[4] | RPL3)
#ifdef HEAP_WATCH_DEBUG
            extern void serial_print(const char* s);
            extern void serial_print_hex(uint64_t v);
            serial_print("[EXEC] ring3 cs=");
            serial_print_hex(r->cs);
            serial_print(" ss=");
            serial_print_hex(r->ss);
            serial_print(" rip=");
            serial_print_hex(r->rip);
            serial_print(" rsp=");
            serial_print_hex(r->rsp);
            serial_print("\n");
#endif
            ret_val = 1;
        } else {
            ret_val = 0;
        }
    }
    else if (syscall_num == 34) { // sys_exit — app selesai, kembali ke shell
        // Destroy KWM windows milik TASK INI saja (FIX_004) — window task lain
        // tidak ikut terhapus; pointer canvas selalu di-NULL-kan.
        extern void kwm_destroy_windows_of(int);
        kwm_destroy_windows_of(smp_current_task_id());

        // Phase 5A: app hasil sys_spawn adalah task independen — tidak ada
        // shell untuk "kembali". task_exit() membereskan sisanya: AS
        // (dead_pml4), uheap, fd terbuka, stack kernel, lalu parkir di idle.
        {
            task_t *self = syscall_current_task();
            if (self && self->kind == TASK_KIND_SPAWNED) {
                task_exit();  // noreturn
            }
        }

        // Destroy address space and switch back to kernel PML4
        {
            task_t *self = syscall_current_task();

            // Tahap 3: buang metadata heap user — frame region & stack app
            // ikut bebas saat AS dihancurkan di bawah.
            uheap_reset(self);

            if (self && self->pml4_phys != 0) {
                vmm_destroy_task_as(self->pml4_phys);
                self->pml4_phys = 0;
            }
            // pml4_phys == 0: task memakai boot/kernel AS. User range PML4 boot
            // milik Limine — membebaskannya mencemari free list PMM dengan
            // halaman reserved/ROM <72MB (akar BOSD heap corruption).
        }

        // Longjmp kembali ke shell: reset RSP dan jump ke user_shell()
        // Ini BYPASS iretq sepenuhnya — langsung ke shell command loop.
        // Aman karena int 0x80 = software interrupt (tidak perlu EOI).
        extern void user_shell(void);
        extern uint64_t g_shell_return_rsp;
        uint64_t safe_rsp = g_shell_return_rsp;
        if (safe_rsp == 0) {
            // Fallback: jika belum pernah launch app dari shell, halt
            for(;;) __asm__ volatile("hlt");
        }
        // Reset stack dan jump langsung ke shell loop.
        // Tidak pakai CALL (yang push return addr dan grow stack).
        // Pakai JMP → shell berjalan di stack level yang sama.
        __asm__ volatile(
            "mov %0, %%rsp\n"
            "xor %%rbp, %%rbp\n"
            "sti\n"                // Re-enable interrupts! (int 0x80 disabled mereka)
            "jmp *%1\n"
            : : "r"(safe_rsp), "r"((uint64_t)user_shell)
            : "memory"
        );
        __builtin_unreachable();
    }
    else if (syscall_num == 35) { // sys_get_total_disk
        ret_val = kfs_get_total_space();
    }
    else if (syscall_num == 36) { // sys_get_used_disk
        ret_val = kfs_get_used_space();
    }
    else if (syscall_num == 37) { // sys_get_cpu_usage
        ret_val = get_cpu_usage();
    }
    else if (syscall_num == 38) { // sys_shutdown
        extern void acpi_poweroff(void);
        acpi_poweroff();
    }
    else if (syscall_num == 39) { // sys_reboot
        extern void system_reboot(void);
        system_reboot();
    }
    else if (syscall_num == 41) { // sys_ping
        // RBX = const char* host (user-space pointer ke string hostname/IP)
        // Return: RTT dalam ms (>=0) jika berhasil, -1 jika timeout/error
        // Tahap 2: copy-in dulu — string lama dipakai lintas preemption
        // berdetik-detik oleh kernel_ping (TOCTOU tertutup).
        extern int kernel_ping(const char *host);
        char khost[UC_MAX_HOST];
        int rtt = -1;
        if (strncpy_from_user(&uc, khost, r->rbx, sizeof(khost)) > 0) {
            rtt = kernel_ping(khost);
        }
        ret_val = (uint64_t)(int64_t)rtt; // sign-extend -1 dengan benar
    }
    else if (syscall_num == 42) { // sys_get_cr3 — return current CR3 physical address
        // Diagnostic syscall for process isolation testing.
        // Returns the physical address of the current PML4 (CR3 value).
        ret_val = (uint64_t)vmm_read_cr3();
    }
    else if (syscall_num == 43) { // sys_get_task_id — return current task ID
        ret_val = (uint64_t)(int64_t)smp_current_task_id();
    }
    else if (syscall_num == 44) { // sys_is_mapped — check if vaddr is mapped
        // Returns 1 if the page containing vaddr is present in the address
        // space of the CALLING TASK (app AS untuk user page; kernel PML4
        // untuk task kernel). Safe: does NOT dereference, only walks tables.
        task_t *self = syscall_current_task();
        phys_addr_t as = (self) ? self->pml4_phys : PHYS_NULL;
        ret_val = (uint64_t)paging_is_mapped_into((uint64_t)r->rbx, as);
    }
    else if (syscall_num == 45) { // sys_get_pid — return per-AS cookie
        // FIX_003: cookie dibaca dari TASK pemanggil, bukan global —
        // global current_as_cookie menampung cookie task yang TERAKHIR
        // exec di CPU mana pun (tertukar di SMP).
        task_t *self = syscall_current_task();
        ret_val = (uint64_t)(self ? self->cookie : 0);
    }
    else if (syscall_num == 46) { // sys_sleep — non-busy sleep RBX ms
        // Task masuk sleep queue (TASK_SLEEPING); CPU bebas jalankan task lain.
        task_sleep_ms((uint32_t)r->rbx);
    }
    else if (syscall_num == 47) { // sys_open(path, flags) -> fd
        char kpath[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kpath, r->rbx, sizeof(kpath)) > 0) {
            ret_val = (uint64_t)(int64_t)vfs_open(kpath, (uint32_t)r->rcx);
        } else {
            ret_val = (uint64_t)(int64_t)-1;
        }
    }
    else if (syscall_num == 48) { // sys_read(fd, buf, count) -> bytes
        // Tahap 2: baca ke bounce kernel, copy-out di luar vfs_lock.
        uint32_t count = (uint32_t)r->rdx;
        if (count > UC_MAX_IO) count = UC_MAX_IO;
        int n = -1;
        if (count == 0) {
            uint8_t dummy;
            n = vfs_read((int)r->rbx, &dummy, 0);   // pertahankan validasi fd
        } else if (user_range_ok(&uc, r->rcx, count)) {
            uint8_t* bounce = (uint8_t*)kmalloc(count);
            if (bounce) {
                n = vfs_read((int)r->rbx, bounce, count);
                if (n > 0) copy_to_user(&uc, r->rcx, bounce, (uint32_t)n);
                kfree(bounce);
            }
        }
        ret_val = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 49) { // sys_write(fd, buf, count) -> bytes
        // Tahap 2: copy-in ke bounce kernel; count > UC_MAX_IO ditolak
        // eksplisit (dulu count liar = OOB read + alokasi tak berbatas).
        uint32_t count = (uint32_t)r->rdx;
        int n = -1;
        if (count == 0) {
            uint8_t dummy = 0;
            n = vfs_write((int)r->rbx, &dummy, 0);
        } else if (count <= UC_MAX_IO && user_range_ok(&uc, r->rcx, count)) {
            uint8_t* bounce = (uint8_t*)kmalloc(count);
            if (bounce) {
                copy_from_user(&uc, bounce, r->rcx, count); // range sudah valid
                n = vfs_write((int)r->rbx, bounce, count);
                kfree(bounce);
            }
        }
        ret_val = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 50) { // sys_lseek(fd, offset, whence) -> pos
        ret_val = (uint64_t)(int64_t)vfs_lseek((int)r->rbx, (int32_t)r->rcx, (int)r->rdx);
    }
    else if (syscall_num == 51) { // sys_close(fd) -> 0/-1
        ret_val = (uint64_t)(int64_t)vfs_close((int)r->rbx);
    }
    else if (syscall_num == 52) { // sys_socket() -> sockfd
        ret_val = (uint64_t)(int64_t)ksock_socket();
    }
    else if (syscall_num == 53) { // sys_connect(sockfd, ip_be, port)
        ret_val = (uint64_t)(int64_t)ksock_connect((int)r->rbx, (uint32_t)r->rcx, (uint16_t)r->rdx);
    }
    else if (syscall_num == 54) { // sys_sock_send(sockfd, buf, len)
        // Tahap 2: copy-in ke bounce — buffer lama dibaca berulang lintas
        // preemption sampai 5 detik oleh ksock_send (TOCTOU tertutup).
        uint32_t len = (uint32_t)r->rdx;
        int n = -1;
        if (len == 0) {
            n = ksock_send((int)r->rbx, NULL, 0);   // semantik lama: 0
        } else if (len <= UC_MAX_SOCK && user_range_ok(&uc, r->rcx, len)) {
            uint8_t* bounce = (uint8_t*)kmalloc(len);
            if (bounce) {
                copy_from_user(&uc, bounce, r->rcx, len); // range sudah valid
                n = ksock_send((int)r->rbx, bounce, len);
                kfree(bounce);
            }
        }
        ret_val = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 55) { // sys_sock_recv(sockfd, buf, len)
        // Tahap 2: validasi out-range SEBELUM data ring dikonsumsi; terima
        // ke bounce kernel, copy-out di luar net_lock.
        uint32_t len = (uint32_t)r->rdx;
        int n = -1;
        if (len == 0) {
            n = ksock_recv((int)r->rbx, NULL, 0);   // semantik lama: 0
        } else {
            if (len > UC_MAX_SOCK) len = UC_MAX_SOCK;  // recv partial itu sah
            if (user_range_ok(&uc, r->rcx, len)) {
                uint8_t* bounce = (uint8_t*)kmalloc(len);
                if (bounce) {
                    n = ksock_recv((int)r->rbx, bounce, len);
                    if (n > 0) copy_to_user(&uc, r->rcx, bounce, (uint32_t)n);
                    kfree(bounce);
                }
            }
        }
        ret_val = (uint64_t)(int64_t)n;
    }
    else if (syscall_num == 56) { // sys_sock_close(sockfd)
        ret_val = (uint64_t)(int64_t)ksock_close((int)r->rbx);
    }
    else if (syscall_num == 57) { // sys_spawn — Phase 5A: ELF sebagai task ring-3 BARU
        // Copy-in path SEBELUM apa pun (pola boundary Tahap 2).
        char kfname[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kfname, r->rbx, sizeof(kfname)) < 0) {
            r->rax = (uint64_t)-1;
            return;
        }

        ret_val = (uint64_t)-1;

        // 1. Address space baru untuk child
        phys_addr_t child_pml4 = vmm_create_address_space();
        if (child_pml4 != PHYS_NULL) {
            // 2. Load ELF ke AS child. Selama load, CR3 harus menunjuk AS
            //    child (memcpy segmen lewat VA user). pml4_phys CALLER
            //    dipinjamkan sementara ke AS child mengikuti pola sys_exec:
            //    kalau task ini di-preempt di tengah load, scheduler
            //    me-restore CR3 dari pml4_phys task saat resume — tanpa ini
            //    resume bisa membawa CR3 basi dan memcpy segmen salah alamat.
            task_t *self = syscall_current_task();
            phys_addr_t saved_as  = self ? self->pml4_phys : PHYS_NULL;
            phys_addr_t saved_cr3 = vmm_read_cr3();
            if (self) self->pml4_phys = child_pml4;
            vmm_switch_pml4(child_pml4);

            uint64_t child_stack_top = 0;
            uint64_t entry = elf_load_file(kfname, &child_stack_top, child_pml4);

            vmm_switch_pml4(saved_cr3);
            if (self) self->pml4_phys = saved_as;

            // 3. Buat task ring-3 — AS + cookie + kind terpasang sebelum READY
            if (entry != 0 && child_stack_top != 0) {
                int tid = create_user_task(entry, child_stack_top, child_pml4,
                                           as_cookie_next(), kfname);
                if (tid >= 0) {
                    ret_val = (uint64_t)tid;
                    child_pml4 = PHYS_NULL; // kepemilikan AS pindah ke task
                }
            }
            // Gagal load / gagal buat task → AS yatim, hancurkan
            if (child_pml4 != PHYS_NULL) {
                vmm_destroy_task_as(child_pml4);
            }
        }
    }
    else if (syscall_num == 58) { // sys_kwm_set_cursor — Phase 9: bentuk kursor
        int kind = (int)r->rbx;
        if (kind < 0 || kind > 2) {
            ret_val = (uint64_t)-1;
        } else {
            kwm_set_cursor(kind);
            ret_val = 0;
        }
    }
    // ============================================================
    // Phase 10 — Desktop window + taskbar (syscall 59-63)
    // ============================================================
    else if (syscall_num == 59) { // sys_kwm_create_desktop
        ret_val = (uint64_t)kwm_create_desktop();
    }
    else if (syscall_num == 60) { // sys_kwm_set_title
        char ktitle[32];
        if (strncpy_from_user(&uc, ktitle, r->rcx, sizeof(ktitle)) >= 0)
            ret_val = (uint64_t)kwm_set_title((int)r->rbx, ktitle);
        else
            ret_val = (uint64_t)-1;
    }
    else if (syscall_num == 61) { // sys_kwm_get_windows — enum utk taskbar
        int maxn = (int)r->rcx;
        if (maxn > 16) maxn = 16;
        uint64_t bytes = (uint64_t)maxn * sizeof(kwm_window_info_t);
        if (maxn > 0 && user_range_ok(&uc, r->rbx, bytes)) {
            kwm_window_info_t* bounce =
                (kwm_window_info_t*)kmalloc((uint32_t)bytes);
            if (bounce) {
                int count = kwm_get_windows(bounce, maxn);
                if (count > 0 && copy_to_user(&uc, r->rbx, bounce,
                        (uint64_t)count * sizeof(kwm_window_info_t)) != 0)
                    count = 0;
                ret_val = (uint64_t)count;
                kfree(bounce);
            }
        }
    }
    else if (syscall_num == 62) { // sys_kwm_activate_window — klik taskbar
        ret_val = (uint64_t)kwm_activate_window((int)r->rbx);
    }
    else if (syscall_num == 63) { // sys_get_screen_size(uint32_t* w, uint32_t* h)
        // DUA pointer terpisah (rbx=w, rcx=h) — bukan satu array 2 elemen.
        // Menulis 8 byte ke rbx dulu menimpa 4 byte SETELAH variabel w milik
        // app (di settings.c itu variabel lain di stack → pointer widget
        // rusak → #PF). copy_to_user memvalidasi range sendiri.
        uint32_t w = fb_width, h = fb_height;
        ret_val = (copy_to_user(&uc, r->rbx, &w, 4) == 0 &&
                   copy_to_user(&uc, r->rcx, &h, 4) == 0) ? 0 : (uint64_t)-1;
    }
    else if (syscall_num == 64) { // sys_mkdir(path) — buat folder KyuzenFS
        char kf[UC_MAX_FNAME];
        if (strncpy_from_user(&uc, kf, r->rbx, sizeof(kf)) >= 0) {
            ret_val = (uint64_t)kfs_create_folder(kf);
        }
    }
    else if (syscall_num == 65) { // sys_gpu_stats(buf) — Phase 2C §9.6
        // buf = ghal_gpu_stats_t (7 x uint64), mirror gpu_stats_t di userlib.h.
        ghal_gpu_stats_t st;
        if (ghal_gpu_stats(&st) != 0) ret_val = (uint64_t)-1;
        else ret_val = (copy_to_user(&uc, r->rbx, &st, sizeof(st)) == 0) ? 0 : (uint64_t)-1;
    }
    // ============================================================
    // Phase 3 — Partial window update (syscall 66)
    // ============================================================
    else if (syscall_num == 66) { // sys_kwm_update_window_rect(req)
        // req = kwm_rect_update_t di user-space. Copy-in bounded dulu, lalu
        // validasi independen: ownership/batas rect di KWM, dan rentang
        // buffer user untuk span baris yang benar-benar diakses.
        extern int kwm_update_window_rect(int, int32_t, int32_t,
                                          uint32_t, uint32_t, uint32_t*);
        extern int kwm_window_dims(int, uint32_t*, uint32_t*);
        kwm_rect_update_t req;
        if (copy_from_user(&uc, &req, r->rbx, sizeof(req)) != 0) {
            ret_val = (uint64_t)-1;
        } else if (req.win_id < 0 || req.win_id >= 16 ||
                   req.x < 0 || req.y < 0 ||
                   req.width == 0 || req.height == 0) {
            ret_val = (uint64_t)-1;
        } else {
            uint32_t win_w = 0, win_h = 0;
            if (kwm_window_dims(req.win_id, &win_w, &win_h) != 0 ||
                (uint64_t)req.x + (uint64_t)req.width > (uint64_t)win_w ||
                (uint64_t)req.y + (uint64_t)req.height > (uint64_t)win_h) {
                ret_val = (uint64_t)-1;
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
                if (!user_range_ok(&uc, ustart, span)) {
                    ret_val = (uint64_t)-1;
                } else {
                    ret_val = (uint64_t)kwm_update_window_rect(
                        req.win_id, req.x, req.y,
                        req.width, req.height, req.buffer);
                }
            }
        }
    }

    // SIMPAN RETURN VALUE KE RAX (Penting untuk aplikasi Ring 3!)
    r->rax = ret_val;
}
