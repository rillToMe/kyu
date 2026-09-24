// Real C renderer/lifecycle, fresh full-scene oracle, deterministic replay.
// Hardware boundary: firmware scanout in RAM and a controllable async presenter.
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "damage_test_lock.h"

static uint64_t test_clock_hz = 1000000000;
static uint64_t test_clock(void) {
#ifdef _WIN32
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
#else
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
#endif
}
static int test_full, test_noopaque, log_frames;
#define KWM_DEBUG_FULL_REDRAW test_full
#define KWM_DEBUG_DISABLE_OPAQUE_OPT test_noopaque
#define KWM_DEBUG_DAMAGE 1
#define KWM_DAMAGE_CLOCK() test_clock()
#define KWM_DAMAGE_CLOCK_HZ test_clock_hz

void* kmalloc(size_t n) {
    void* p = malloc(n);
    if (p) memset(p, 0xA5, n); // expose reads before the first canvas upload
    return p;
}
void kfree(void* p) { free(p); }
int g_smap_enabled;
static int owner = 1, close_events;
int smp_current_task_id(void) { return owner; }
void push_event_to(int task, uint32_t type, int32_t a, int32_t b, int32_t c, int32_t win) {
    (void)task; (void)a; (void)b; (void)c; (void)win;
    if (type == 6) close_events++;
}
void serial_print(const char* s) { if (log_frames) fputs(s, stdout); }
int panic_is_locked(void) { return 0; }
int32_t mouse_x = 2, mouse_y = 2;
const uint8_t cursor_bitmap[16][12] = {
    {2}, {2,1,2}, {2,1,1,2}, {2,1,1,1,2}, {2,1,1,1,1,2},
    {2,1,1,1,1,1,2}, {2,1,1,1,2,2,2}, {2,1,2,1,2}, {2,2,0,2,1,2}
};

#include "../../../kernel/display.c"
#include "../../../kernel/gfx/kwm.c"
#include "../../../graphics/backend/software.c"

static int async_mode, hold_device, fail_submit, inject_reentry, inject_update;
static int backing_hazards, upload_depth, max_upload_depth;
static uint64_t fence_seq, fence_done, wait_calls, errors;
static struct { ghal_surface_t* surface; ghal_rect_t rect; uint64_t fence; } pending[64];
static unsigned pending_count;
static void complete_device(void) {
    for (unsigned i = 0; i < pending_count; i++) {
        sw_present(pending[i].surface, &pending[i].rect);
        fence_done = pending[i].fence;
    }
    pending_count = 0;
}
uint32_t ghal_capabilities(void) { return async_mode ? GHAL_CAP_ASYNC_PRESENT : 0; }
ghal_surface_t* ghal_surface_create(uint32_t w, uint32_t h, ghal_format_t f) {
    return sw_surface_create(w, h, f);
}
ghal_surface_t* ghal_surface_create_scanout(uint32_t w, uint32_t h, ghal_format_t f) {
    return async_mode ? sw_surface_create(w, h, f) : sw_surface_create_scanout(w, h, f);
}
void ghal_surface_destroy(ghal_surface_t* s) { sw_surface_destroy(s); }
void ghal_surface_upload(ghal_surface_t* s, const uint32_t* p, uint32_t stride, ghal_rect_t r) {
    if (pending_count) backing_hazards++;
    upload_depth++;
    if (upload_depth > max_upload_depth) max_upload_depth = upload_depth;
    if (inject_reentry) {
        inject_reentry = 0;
        draw_rect(7, 7, 3, 3, 0xFF123456);
        compositor_flush();
    }
    if (inject_update) { inject_update = 0; draw_rect(9, 9, 3, 3, 0xFF654321); }
    sw_surface_upload(s, p, stride, r);
    upload_depth--;
}
int ghal_present_checked(ghal_surface_t* s, const ghal_rect_t* r) {
    if (fail_submit) { fail_submit--; return -1; }
    if (!async_mode) { sw_present(s, r); return 0; }
    if (pending_count == 64) return -1;
    pending[pending_count].surface = s;
    pending[pending_count].rect = *r;
    pending[pending_count++].fence = ++fence_seq;
    return 0;
}
void ghal_present(ghal_surface_t* s, const ghal_rect_t* r) {
    (void)ghal_present_checked(s, r);
}
uint64_t ghal_present_fence(void) { return async_mode ? fence_seq : 0; }
int ghal_fence_pending(uint64_t f) {
    if (!hold_device) complete_device();
    return f > fence_done;
}
void ghal_fence_wait(uint64_t f) { (void)f; wait_calls++; if (!hold_device) complete_device(); }
int ghal_gpu_stats(ghal_gpu_stats_t* out) {
    *out = (ghal_gpu_stats_t){0}; out->err_count = errors; return 0;
}
int ghal_cursor_update(ghal_surface_t* s, int x, int y) { (void)s; (void)x; (void)y; return -1; }
void ghal_cursor_move(int x, int y) { (void)x; (void)y; }
int ghal_mode_get(display_mode_t* m) { return software_mode_get(m); }
int ghal_mode_enumerate(display_mode_t* m, uint32_t n) { return software_mode_enumerate(m, n); }
int ghal_mode_set(const display_mode_t* m) { (void)m; return -1; }

#include "../../../kernel/gfx/compositor.c"

enum { W = 320, H = 240, STRIDE = 336 };
static uint32_t scanout[STRIDE * H], reference[STRIDE * H];
static unsigned failures, frames, mismatch_frames;
static uint64_t mismatches, sum_us, worst_us, sum_damage, sum_composite, sum_upload, sum_present;
static void check(int ok, const char* label) {
    if (!ok) { failures++; printf("FAIL %s\n", label); }
}

// Bypass damage propagation only. Rebuild from source canvas, with all opacity
// shortcuts disabled, into a freshly poisoned destination. Keep partial output.
static uint64_t pixel_diff(void) {
    damage_frame_t metrics = g_damage_frame;
    int opt = test_noopaque;
    test_noopaque = 1;
    uint32_t* saved = backbuffer;
    backbuffer = reference;
    memset(reference, 0xDA, sizeof(reference));
    DisplayBuffer ref = *gfx_back_buffer();
    ref.pixels = reference;
    Rect screen = {0, 0, W, H};
    base_blit_for_region(&ref, gfx_screen_buffer(), screen);
    composite_windows_in_rect(screen, STRIDE);
    const uint8_t (*bm)[12] = g_cursor_kind == 0 ? cursor_bitmap :
                             g_cursor_kind == 1 ? g_ibeam_bitmap : g_hand_bitmap;
    for (int y = 0; y < CURSOR_HEIGHT; y++) for (int x = 0; x < CURSOR_WIDTH; x++) {
        int sx = mouse_x + x, sy = mouse_y + y;
        if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
        if (bm[y][x]) reference[sy * STRIDE + sx] = bm[y][x] == 1 ? 0xFFFFFF : 0;
    }
    backbuffer = saved;
    test_noopaque = opt;
    g_damage_frame = metrics;
    uint64_t bad = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        if ((scanout[y * STRIDE + x] & 0xFFFFFF) != (reference[y * STRIDE + x] & 0xFFFFFF)) bad++;
    return bad;
}
static uint64_t last_damage, last_composite, last_upload, last_present, last_rects, last_us;
static void frame(const char* label) {
    // A real display drains one queue per tick. Retained damage (queue
    // pressure, deferred fence) converges over ticks, not one flush.
    uint64_t du = 0, cu = 0, uu = 0, pu = 0, ru = 0, tu = 0, wu = 0;
    int spins = 0;
    do {
        compositor_flush();
        if (!hold_device) complete_device();
        du += g_damage_frame.damage_pixel_count;
        cu += g_damage_frame.composite_pixel_count;
        uu += g_damage_frame.upload_pixel_count;
        pu += g_damage_frame.present_pixel_count;
        ru += g_damage_frame.present_rect_count;
        tu += g_damage_frame.frame_time_us;
        if (g_damage_frame.frame_time_us > wu) wu = g_damage_frame.frame_time_us;
        spins++;
    } while (!hold_device && compositor_pending() && spins < 80);
    check(spins < 80, "present drain converges");
    last_damage = du; last_composite = cu; last_upload = uu;
    last_present = pu; last_rects = ru; last_us = tu;
    uint64_t bad = pixel_diff();
    if (bad) {
        if (mismatch_frames < 12) printf("DIFF %s pixels=%llu\n", label, (unsigned long long)bad);
        mismatch_frames++; mismatches += bad;
    }
    for (int y = 0; y < H; y++) for (int x = W; x < STRIDE; x++)
        assert(scanout[y * STRIDE + x] == 0xC0FFEE);
    frames++;
    sum_us += tu;
    if (wu > worst_us) worst_us = wu;
    sum_damage += du;
    sum_composite += cu;
    sum_upload += uu;
    sum_present += pu;
}
static uint32_t pixels[W * H];
static void pattern(uint32_t w, uint32_t h, uint32_t seed) {
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
        pixels[y * w + x] = 0xFF000000 | ((seed + x * 271 + y * 997) & 0xFFFFFF);
}
static int window(int x, int y, uint32_t w, uint32_t h, uint32_t seed) {
    int id = kwm_create_window(x, y, w, h);
    assert(id >= 0);
    pattern(w, h, seed);
    kwm_update_window(id, pixels);
    assert(kwm_set_window_opaque(id) == 0);
    return id;
}
static void move(int id, int x, int y) {
    int ox = kwm_windows[id].x, oy = kwm_windows[id].y;
    kwm_process_mouse(ox + 8, oy + 10, 1, 0);
    mouse_x = x + 8; mouse_y = y + 10;
    kwm_process_mouse(mouse_x, mouse_y, 0, 0);
    kwm_process_mouse(mouse_x, mouse_y, 0, 1);
}
// Synthetic transitions exercise scene reconstruction only: no public resize or
// visibility API exists in KWM. Their status must not be reported as ABI tests.
static void synthetic_resize(int id, uint32_t w, uint32_t h) {
    kwm_window_t* win = &kwm_windows[id];
    frame_dirty_area(win->x, win->y, win->width, win->height + KWM_TITLEBAR_H);
    display_buffer_destroy(win->canvas);
    win->canvas = display_buffer_create(w, h, COLOR_FORMAT_XRGB8888);
    assert(win->canvas);
    win->width = w; win->height = h; win->fully_opaque = 0;
    pattern(w, h, 0x406080);
    kwm_update_window(id, pixels);
    frame_dirty_area(win->x, win->y, w, h + KWM_TITLEBAR_H);
}
static void synthetic_visible(int id, int visible) {
    kwm_window_t* win = &kwm_windows[id];
    frame_dirty_area(win->x, win->y, win->width, win->height + KWM_TITLEBAR_H);
    win->active = (uint8_t)visible;
}
static void scenario(void) {
    draw_rect(0, 0, W, H, 0x203040); frame("background");
    int desktop = kwm_create_desktop(); assert(desktop >= 0);
    pattern(W, H, 0x123456); kwm_update_window(desktop, pixels);
    kwm_set_window_opaque(desktop); frame("wallpaper");
    int a = window(20, 35, 130, 80, 0x113355); frame("create A");
    int b = window(95, 85, 140, 90, 0x996633); frame("create B overlapping");
    mouse_x = 140; mouse_y = 45;
    kwm_process_mouse(mouse_x, mouse_y, 0, 0); frame("close hover lower A");
    kwm_process_mouse(mouse_x, mouse_y, 1, 0); frame("close request raises A");
    check(close_events == 1, "close event routed");
    kwm_activate_window(b); frame("activate B");
    move(b, 35, 100); frame("move left over A");
    move(b, 165, 90); frame("move right exposes A");
    move(b, 90, 50); pattern(140, 90, 0x778899);
    kwm_update_window_rect(b, 8, 9, 3, 7, pixels); frame("move + local content");
    pixels[9 * 140 + 8] = 0; // opacity contract must be maintained after upload
    kwm_update_window_rect(b, 8, 9, 1, 1, pixels); frame("opaque to transparent pixel");
    pattern(140, 90, 0x778899); kwm_update_window(b, pixels); frame("full content");
    for (int i = 0; i < 36; i++) {
        pattern(140, 90, (uint32_t)i * 711);
        kwm_update_window_rect(b, (i * 17) % 137, (i * 11) % 87, 3, 3, pixels);
        frame("rapid content");
    }
    for (int i = 0; i < 36; i++) { move(b, 20 + i * 4, 40 + i % 3); frame("rapid move"); }
    for (int i = 0; i < 12; i++) {
        synthetic_resize(b, (uint32_t)(70 + i % 2 * 90), (uint32_t)(50 + i % 3 * 20));
        frame("synthetic resize + content");
    }
    synthetic_visible(b, 0); pattern(130, 80, 0xAABBCC);
    kwm_update_window(a, pixels); frame("synthetic hide/minimize + underlying update");
    synthetic_visible(b, 1); frame("synthetic show/restore");
    kwm_destroy_window(b); pattern(130, 80, 0x112233);
    kwm_update_window(a, pixels); frame("destroy + underlying update");
    int ids[6];
    for (int i = 0; i < 6; i++) { ids[i] = window(10 + i * 27, 10 + i * 17, 90, 60, (uint32_t)i * 45671); frame("5+ overlap"); }
    for (int i = 0; i < 60; i++) {
        int id = ids[i % 6];
        pattern(90, 60, (uint32_t)i * 3341);
        kwm_update_window_rect(id, 2 + i % 40, 4, 2, 12, pixels);
        if (i % 3 == 0) kwm_activate_window(id);
        if (i % 5 == 0) move(id, i % 200, i % 120);
        frame("multi-window rapid combined");
    }
    for (int i = 5; i >= 0; i--) { kwm_destroy_window(ids[i]); frame("destroy overlapping"); }
    kwm_destroy_window(a); frame("destroy last normal");
    int tiny = window(65, 40, 5, 3, 0xA0B0C0); frame("tiny window chrome clip");
    kwm_destroy_window(tiny); frame("tiny window destroy");
    int edge = window(-32, -16, 110, 80, 0x246810); frame("partly outside top left");
    kwm_destroy_window(edge); frame("destroy offscreen");
    edge = window(W - 10, H - 20, 80, 70, 0x102468); frame("partly outside bottom right");
    kwm_destroy_window(edge); frame("destroy edge");
    for (int i = 0; i < 110; i++) {
        int x = (i * 41) % W, y = (i * 17) % H;
        pixels[y * W + x] = 0xFF345678 + (uint32_t)i;
        kwm_update_window_rect(desktop, x, y, 1, 1, pixels);
    }
    frame("fragmentation over capacity");
    pattern(W, H, 0xF1C090); kwm_update_window(desktop, pixels); frame("wallpaper change");
    for (int y = 1; y < 10; y++) {
        memmove(pixels, pixels + W, (H - 1) * W * 4);
        kwm_update_window(desktop, pixels); frame("scroll");
    }
    check(kwm_update_window_rect(desktop, 0, 0, 0, 1, pixels) == -1, "zero width rejected");
    check(kwm_update_window_rect(desktop, 0, 0, 1, 0, pixels) == -1, "zero height rejected");
    check(kwm_update_window_rect(desktop, INT_MAX, 0, UINT_MAX, 1, pixels) == -1, "local overflow rejected");
    for (int i = 0; i < 6; i++) {
        int x = i % 2 ? W - 3 : 1, y = 10 + i * 40;
        pixels[y * W + x] = 0xFF123456;
        kwm_update_window_rect(desktop, x, y, 1, 1, pixels);
    }
    frame("six far apart pixels");
    printf("sparse damage=%llu composite=%llu upload=%llu present=%llu rects=%llu time_us=%llu\n",
        (unsigned long long)last_damage, (unsigned long long)last_composite,
        (unsigned long long)last_upload, (unsigned long long)last_present,
        (unsigned long long)last_rects, (unsigned long long)last_us);
    frame("idle");
}

static void lifecycle_checks(void) {
    kwm_destroy_all_windows();
    int a = kwm_create_window(10, 20, 80, 50); assert(a >= 0);
    int initialized = 1;
    for (unsigned i = 0; i < 80 * 50; i++)
        if (kwm_windows[a].canvas->pixels[i] != 0) initialized = 0;
    check(initialized, "new canvas initialized before publication");
    pattern(80, 50, 0x221144); kwm_update_window(a, pixels);
    while (next_z_index < MAX_WINDOWS) kwm_activate_window(a);
    int b = window(60, 60, 80, 50, 0x116633);
    check(kwm_windows[b].z_index <= next_z_index && kwm_windows[b].z_index > kwm_windows[a].z_index,
          "creation normalization includes new slot");
    for (int i = 0; i < 400; i++) kwm_process_mouse(65, 110, 1, 0);
    check(next_z_index <= MAX_WINDOWS + 1, "mouse focus bounds z scan");
    frame("z-order pressure");
    kwm_destroy_all_windows();
    int extreme = kwm_create_window(INT_MAX, INT_MIN, 1, 1);
    check(extreme == -1, "unrepresentable visual bounds rejected");
    if (extreme >= 0) kwm_destroy_window(extreme);
    dirty_region_clear(&g_screen_dirty);
    screen_mark_dirty(INT_MIN, 0, UINT_MAX, H);
    screen_mark_dirty(INT_MAX, INT_MAX, UINT_MAX, UINT_MAX);
    int clipped = g_screen_dirty.count != 0;
    for (uint32_t i = 0; i < g_screen_dirty.count; i++) {
        Rect r = g_screen_dirty.regions[i];
        if (r.x < 0 || r.y < 0 || (uint64_t)r.x + r.width > W ||
            (uint64_t)r.y + r.height > H || !r.width || !r.height) clipped = 0;
    }
    check(clipped, "screen clipping precedes coalescing");
    frame("extreme damage clipped");
}

static void synchronization_checks(void) {
    kwm_destroy_all_windows();
    hold_device = 0; complete_device();
    frame("sync initial");
    backing_hazards = 0; wait_calls = 0;
    hold_device = 1;
    draw_rect(80, 90, 2, 2, 0x123456); compositor_flush();
    draw_rect(130, 40, 3, 3, 0x654321); compositor_flush();
    check(backing_hazards == 0, "pending DMA backing not overwritten");
    check(wait_calls == 0, "pending device does not busy-wait in frame");
    hold_device = 0; complete_device(); frame("backpressure retry");

    fail_submit = 1;
    draw_rect(140, 140, 3, 3, 0x224488); compositor_flush();
    frame("submit failure retries damage");

    draw_rect(150, 150, 3, 3, 0x884422); compositor_flush();
    pending_count = 0; fence_done = fence_seq; errors++; // rejected by device
    frame("device error retries damage");

    max_upload_depth = 0; inject_reentry = 1;
    draw_rect(45, 45, 2, 2, 0x446688); compositor_flush();
    frame("reentrant flush deferred");
    check(max_upload_depth == 1, "single owner from composition through upload");

    inject_update = 1;
    draw_rect(50, 50, 2, 2, 0x668844); compositor_flush();
    frame("producer damage during present retained");

    complete_device();
    spinlock_lock(&kwm_lock);
    draw_rect(70, 70, 2, 2, 0x448866);
    compositor_flush();
    check(g_screen_dirty.count != 0, "busy scene lock preserves damage");
    spinlock_unlock(&kwm_lock);
    frame("busy scene lock retry");
}

static void coverage_checks(void) {
    static uint8_t wanted[W * H];
    uint32_t random = 0x12345678;
    int covered = 1, bounded = 1;
    for (int pass = 0; pass < 32; pass++) {
        memset(wanted, 0, sizeof(wanted));
        dirty_region_clear(&g_screen_dirty);
        for (int n = 0; n < 160; n++) {
            random = random * 1664525u + 1013904223u;
            int x = (int)(random % (W + 24)) - 12;
            random = random * 1664525u + 1013904223u;
            int y = (int)(random % (H + 24)) - 12;
            unsigned w = 1 + random % 5, h = 1 + (random >> 8) % 5;
            screen_mark_dirty(x, y, w, h);
            for (unsigned yy = 0; yy < h; yy++) for (unsigned xx = 0; xx < w; xx++) {
                int px = x + (int)xx, py = y + (int)yy;
                if (px >= 0 && px < W && py >= 0 && py < H) wanted[py * W + px] = 1;
            }
        }
        if (g_screen_dirty.count > MAX_DIRTY_REGIONS) bounded = 0;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (!wanted[y * W + x]) continue;
            int found = 0;
            for (uint32_t i = 0; i < g_screen_dirty.count; i++) {
                Rect r = g_screen_dirty.regions[i];
                if (x >= r.x && y >= r.y && x < r.x + (int)r.width && y < r.y + (int)r.height)
                    found = 1;
            }
            if (!found) covered = 0;
        }
    }
    check(covered && bounded, "5120 fragmented rectangles retain complete bounded coverage");
    frame("coalescing coverage");

    kwm_destroy_all_windows();
    int a = window(20, 35, 160, 100, 0x446688);
    frame("opaque full-upload setup");
    pixels[5 * 160 + 5] = 0;
    kwm_update_window(a, pixels);
    frame("full upload revokes opaque hint");
    kwm_destroy_window(a);
    for (int i = 0; i < 16; i++)
        window(5 + (i % 4) * 70, 4 + (i / 4) * 44, 54, 36, (uint32_t)i * 751);
    frame("all 16 slots, fragmented opaque covers");
    kwm_destroy_windows_of(owner);
    frame("destroy 16 owned windows");
}

int main(int argc, char** argv) {
#ifdef _WIN32
    LARGE_INTEGER hz; QueryPerformanceFrequency(&hz); test_clock_hz = (uint64_t)hz.QuadPart;
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--full")) test_full = 1;
        if (!strcmp(argv[i], "--no-opaque")) test_noopaque = 1;
        if (!strcmp(argv[i], "--async")) async_mode = 1;
        if (!strcmp(argv[i], "--log")) log_frames = 1;
    }
    for (unsigned i = 0; i < sizeof(scanout) / sizeof(*scanout); i++) scanout[i] = 0xC0FFEE;
    display_format_desc_t fmt = {32, 1, 8, 16, 8, 8, 8, 0};
    assert(display_boot_init(scanout, W, H, STRIDE * 4, &fmt) == 0);
    assert(display_alloc_buffers() == 0);
    software_backend_set_fb(scanout, W, H, STRIDE * 4);
    compositor_ghal_init();
    scenario();
    // Record identical-scene performance separately from fault injection.
    printf("replay avg_us=%llu worst_us=%llu avg_damage=%llu avg_composite=%llu avg_upload=%llu avg_present=%llu\n",
        (unsigned long long)(sum_us / frames), (unsigned long long)worst_us,
        (unsigned long long)(sum_damage / frames), (unsigned long long)(sum_composite / frames),
        (unsigned long long)(sum_upload / frames), (unsigned long long)(sum_present / frames));
    lifecycle_checks();
    if (async_mode) synchronization_checks();
    coverage_checks();
    printf("mode=%s opaque=%s backend=%s frames=%u pixel_mismatches=%llu mismatch_frames=%u checks_failed=%u\n",
        test_full ? "full" : "partial", test_noopaque ? "off" : "on", async_mode ? "async-model" : "software",
        frames, (unsigned long long)mismatches, mismatch_frames, failures);
    printf("perf avg_us=%llu worst_us=%llu avg_damage=%llu avg_composite=%llu avg_upload=%llu avg_present=%llu\n",
        (unsigned long long)(sum_us / frames), (unsigned long long)worst_us,
        (unsigned long long)(sum_damage / frames), (unsigned long long)(sum_composite / frames),
        (unsigned long long)(sum_upload / frames), (unsigned long long)(sum_present / frames));
    return failures || mismatches ? 1 : 0;
}
