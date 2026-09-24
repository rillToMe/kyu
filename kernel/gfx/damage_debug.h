#ifndef KWM_DAMAGE_DEBUG_H
#define KWM_DAMAGE_DEBUG_H

#include "display.h"
#include "serial.h"

// Debug-only controls. Production builds keep partial damage and opaque culling.
#ifndef KWM_DEBUG_FULL_REDRAW
#define KWM_DEBUG_FULL_REDRAW 0
#endif
#ifndef KWM_DEBUG_DISABLE_OPAQUE_OPT
#define KWM_DEBUG_DISABLE_OPAQUE_OPT 0
#endif
#ifndef KWM_DEBUG_DAMAGE
#define KWM_DEBUG_DAMAGE 0
#endif
#ifndef KWM_DEBUG_RECTS
#define KWM_DEBUG_RECTS 0
#endif

#if KWM_DEBUG_DAMAGE
typedef struct {
    uint64_t frame_id, damage_rect_count, damage_pixel_count, screen_pixel_count;
    uint64_t composite_pixel_count, base_pixel_count, content_pixel_count;
    uint64_t upload_pixel_count, present_rect_count, present_pixel_count;
    uint64_t deferred, frame_time_us, start;
} damage_frame_t;
static damage_frame_t g_damage_frame;

#ifndef KWM_DAMAGE_CLOCK
#include "io.h"
static uint64_t g_damage_clock_hz;
static uint64_t damage_clock(void) {
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
// PIT2 one-shot calibration, once in boot/task context, never in a frame IRQ.
// No guessed CPU frequency; a failed calibration is reported as clock_valid=0.
static void damage_debug_init(void) {
    const uint32_t divisor = 11932; // ~10 ms at 1193182 Hz
    uint8_t saved = inb(0x61);
    outb(0x61, saved & (uint8_t)~3u);
    outb(0x43, 0xB0);
    outb(0x42, (uint8_t)divisor);
    outb(0x42, (uint8_t)(divisor >> 8));
    uint64_t start = damage_clock();
    outb(0x61, (saved & (uint8_t)~2u) | 1u);
    uint32_t budget = 10000000;
    while (!(inb(0x61) & 0x20) && --budget) __asm__ volatile("pause");
    uint64_t elapsed = damage_clock() - start;
    outb(0x61, saved);
    if (budget) g_damage_clock_hz = elapsed * 1193182 / divisor;
}
#define KWM_DAMAGE_CLOCK() damage_clock()
#define KWM_DAMAGE_CLOCK_HZ g_damage_clock_hz
#else
static void damage_debug_init(void) {}
#endif

#define DAMAGE_ADD(field, value) (g_damage_frame.field += (uint64_t)(value))
static void damage_debug_begin(void) {
    uint64_t id = g_damage_frame.frame_id + 1;
    g_damage_frame = (damage_frame_t){0};
    g_damage_frame.frame_id = id;
    g_damage_frame.start = KWM_DAMAGE_CLOCK();
}
#else
#define DAMAGE_ADD(field, value) ((void)0)
static inline void damage_debug_init(void) {}
static inline void damage_debug_begin(void) {}
#endif

#if KWM_DEBUG_DAMAGE || KWM_DEBUG_RECTS
static void damage_debug_num(uint64_t n) {
    char s[21];
    int i = 20;
    s[i] = 0;
    do { s[--i] = (char)('0' + n % 10); n /= 10; } while (n);
    serial_print(s + i);
}
static void damage_debug_field(const char* name, uint64_t n) {
    serial_print(name);
    damage_debug_num(n);
}
#endif

static inline void damage_debug_rect(const char* stage, Rect r) {
#if KWM_DEBUG_RECTS
    serial_print(stage); // All logged rectangles have already been screen-clipped.
    damage_debug_field(" x=", (uint32_t)r.x);
    damage_debug_field(" y=", (uint32_t)r.y);
    damage_debug_field(" w=", r.width);
    damage_debug_field(" h=", r.height);
    serial_print("\n");
#else
    (void)stage; (void)r;
#endif
}

static inline void damage_debug_end(void) {
#if KWM_DEBUG_DAMAGE
    uint64_t elapsed = KWM_DAMAGE_CLOCK() - g_damage_frame.start;
    if (KWM_DAMAGE_CLOCK_HZ)
        g_damage_frame.frame_time_us = elapsed * 1000000 / KWM_DAMAGE_CLOCK_HZ;
    damage_debug_field("[damage-frame] frame=", g_damage_frame.frame_id);
    damage_debug_field(" damage_rects=", g_damage_frame.damage_rect_count);
    damage_debug_field(" damage_pixels=", g_damage_frame.damage_pixel_count);
    damage_debug_field(" screen_pixels=", g_damage_frame.screen_pixel_count);
    damage_debug_field(" composite_pixels=", g_damage_frame.composite_pixel_count);
    damage_debug_field(" base_pixels=", g_damage_frame.base_pixel_count);
    damage_debug_field(" content_pixels=", g_damage_frame.content_pixel_count);
    damage_debug_field(" upload_pixels=", g_damage_frame.upload_pixel_count);
    damage_debug_field(" present_rects=", g_damage_frame.present_rect_count);
    damage_debug_field(" present_pixels=", g_damage_frame.present_pixel_count);
    damage_debug_field(" deferred=", g_damage_frame.deferred);
    damage_debug_field(" frame_time_us=", g_damage_frame.frame_time_us);
    damage_debug_field(" clock_valid=", KWM_DAMAGE_CLOCK_HZ != 0);
    serial_print("\n");
#endif
}

#endif
