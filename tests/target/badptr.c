// ============================================================
// badptr.c — FIX_005 Tahap 2: uji boundary copy syscall.
// Melempar pointer NULL / unmapped / wraparound / ukuran liar ke
// syscall yang sudah dikonversi. Lulus = kernel menolak dengan rapi
// (return gagal / diabaikan) dan TIDAK panic/BOSD.
// Jalankan dari shell: ketik `badptr`.
// ============================================================
#include "userlib.h"

static int pass_count = 0;
static int fail_count = 0;

static void check(const char* name, int ok) {
    print(ok ? "[PASS] " : "[FAIL] ");
    print((char*)name);
    print("\n");
    if (ok) pass_count++; else fail_count++;
}

#define UNMAPPED_LOW ((void*)0x10000)                // user range, tidak mapped
#define WRAP_HIGH    ((void*)0xFFFFFFFFFFFFF000ULL)  // ujung atas address space

void main(void) {
    print("=== badptr: uji boundary copy (FIX_005 Tahap 2) ===\n");

    // --- Grup 1: string copy-in ---
    print(NULL);                        // syscall 1: harus diabaikan
    check("print(NULL) tidak crash", 1);
    print((char*)UNMAPPED_LOW);
    check("print(unmapped) tidak crash", 1);
    check("file_exists(NULL) == 0",     sys_file_exists(NULL) == 0);
    check("file_exists(unmapped) == 0", sys_file_exists((char*)UNMAPPED_LOW) == 0);
    check("file_exists(wrap) == 0",     sys_file_exists((char*)WRAP_HIGH) == 0);
    check("file_size(NULL) == 0",       sys_file_size(NULL) == 0);
    check("ping(NULL) == -1",           sys_ping(NULL) == -1);
    check("open(NULL) == -1",           sys_open(NULL, O_RDONLY) == -1);

    // --- Grup 2: out-pointer ukuran tetap ---
    sys_get_time(NULL);
    check("get_time(NULL) tidak crash", 1);
    get_cpu_string(NULL);
    check("get_cpu_string(NULL) tidak crash", 1);
    check("get_event(NULL) == 0",       sys_get_event(NULL) == 0);
    check("get_event(unmapped) == 0",
          sys_get_event((kyuzen_event_t*)UNMAPPED_LOW) == 0);

    // --- Grup 3: buffer variabel ---
    check("read_keyboard(NULL) == 0 (tidak block)", read_keyboard(NULL, 16) == 0);
    check("read_file_to_buffer(out NULL) == 0",
          sys_read_file_to_buffer("badptr.elf", NULL, 4096) == 0);
    check("get_file_list(NULL) == 0",   sys_get_file_list("/", NULL, 16) == 0);
    check("write_fd(count liar) == -1",
          sys_write_fd(0, (void*)UNMAPPED_LOW, 0x7FFFFFFF) == -1);
    check("send(NULL) == -1",           sys_send(0, NULL, 16) == -1);
    check("recv(unmapped) == -1",       sys_recv(0, UNMAPPED_LOW, 16) == -1);
    check("create_file(data unmapped) == 0",
          sys_create_file("bpx.tmp", (char*)UNMAPPED_LOW, 128) == 0);

    // --- Grup 4: gambar / canvas ---
    sys_draw_image(0, 0, 5000, 5000, (uint32_t*)UNMAPPED_LOW);   // dimensi liar
    check("draw_image(dim liar) tidak crash", 1);
    sys_draw_image(0, 0, 64, 64, (uint32_t*)UNMAPPED_LOW);       // buffer unmapped
    check("draw_image(unmapped) tidak crash", 1);
    sys_draw_string(NULL, 0, 0, 0xFFFFFF);
    check("draw_string(NULL) tidak crash", 1);
    sys_kwm_update_window(0, (uint32_t*)UNMAPPED_LOW);
    check("kwm_update_window(unmapped) tidak crash", 1);

    // --- Grup 6: partial window update (syscall 66 / Phase 3) ---
    // Validasi: ownership + batas rect + overflow + rentang buffer user.
    int wid = sys_kwm_create_window(0, 0, 64, 64);
    check("create_window(64x64) >= 0", wid >= 0);
    if (wid >= 0) {
        uint32_t* canvas = (uint32_t*)sys_alloc(64 * 64 * 4);
        check("alloc canvas", canvas != 0);
        if (canvas) {
            for (int i = 0; i < 64 * 64; i++) canvas[i] = 0xFF204060;
            kwm_rect_update_t req;
            req.win_id = wid;
            req.buffer = canvas;

            req.x = 0;  req.y = 0;  req.width = 64; req.height = 64;
            check("rect full 64x64 == 0", sys_kwm_update_window_rect(&req) == 0);
            req.x = 10; req.y = 20; req.width = 30; req.height = 25;
            check("rect interior == 0",   sys_kwm_update_window_rect(&req) == 0);
            req.x = 63; req.y = 63; req.width = 1;  req.height = 1;
            check("rect 1x1 corner == 0", sys_kwm_update_window_rect(&req) == 0);

            req.x = -1; req.y = 0;  req.width = 8;  req.height = 8;
            check("rect x<0 == -1",       sys_kwm_update_window_rect(&req) == -1);
            req.x = 0;  req.y = -1; req.width = 8;  req.height = 8;
            check("rect y<0 == -1",       sys_kwm_update_window_rect(&req) == -1);
            req.x = 0;  req.y = 0;  req.width = 0;  req.height = 8;
            check("rect width=0 == -1",   sys_kwm_update_window_rect(&req) == -1);
            req.x = 0;  req.y = 0;  req.width = 8;  req.height = 0;
            check("rect height=0 == -1",  sys_kwm_update_window_rect(&req) == -1);
            req.x = 60; req.y = 0;  req.width = 8;  req.height = 8;
            check("rect x+w>W == -1",     sys_kwm_update_window_rect(&req) == -1);
            req.x = 0;  req.y = 60; req.width = 8;  req.height = 8;
            check("rect y+h>H == -1",     sys_kwm_update_window_rect(&req) == -1);
            req.x = 0x7FFFFFFF; req.y = 0; req.width = 0x7FFFFFFF; req.height = 1;
            check("rect overflow x == -1", sys_kwm_update_window_rect(&req) == -1);

            req.x = 0; req.y = 0; req.width = 8; req.height = 8;
            req.buffer = (uint32_t*)UNMAPPED_LOW;
            check("rect unmapped buf == -1", sys_kwm_update_window_rect(&req) == -1);

            req.buffer = canvas;
            req.win_id = -1;
            check("rect win_id<0 == -1",  sys_kwm_update_window_rect(&req) == -1);
            req.win_id = 999;
            check("rect win_id OOB == -1", sys_kwm_update_window_rect(&req) == -1);

            sys_free(canvas);
        }
        sys_kwm_destroy_window(wid);
    }

    // --- Grup 5: kontrol positif — jalur normal harus tetap hidup ---
    check("file_exists(badptr.elf) == 1", sys_file_exists("/apps/badptr.elf") == 1);
    uint32_t t[6];
    t[0] = 0;
    sys_get_time(t);
    check("get_time(valid) menulis tahun", t[0] >= 2000);
    char cpu[64];
    cpu[0] = '\0';
    get_cpu_string(cpu);
    check("get_cpu_string(valid) menulis", cpu[0] != '\0');

    print("=== badptr selesai: ");
    print_num((uint32_t)pass_count);
    print(" PASS, ");
    print_num((uint32_t)fail_count);
    print(" FAIL ===\n");
    sys_exit();
}
