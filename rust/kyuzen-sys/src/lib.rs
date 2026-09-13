//! Raw KyuzenOS userspace syscall ABI plus minimal safe wrappers.
//!
//! ABI (kernel/syscall.c:101-107): RAX = syscall number, RBX/RCX/RDX/RSI/RDI
//! = args 1..5, return value in RAX. Invoked via `int $0x80` (DPL=3 gate),
//! identical to the C wrappers in apps/userlib.c.
//!
//! Safety model: the raw `syscallN` entry points are `unsafe` (they can
//! perform privileged kernel actions on arbitrary pointer arguments). The
//! safe wrappers (`print`, `print_u64`, ...) own their buffers and lengths
//! and never trust caller data.

#![no_std]

use core::fmt;
use core::fmt::Write;

// Syscall numbers (single source of truth is kernel/syscall.c).
const SYS_PRINT: u64 = 1;
const SYS_YIELD: u64 = 4;
const SYS_ALLOC: u64 = 9;
const SYS_FREE: u64 = 10;
const SYS_UPTIME: u64 = 14;
const SYS_REALLOC: u64 = 19;
const SYS_GET_EVENT: u64 = 29;
const SYS_KWM_CREATE_WINDOW: u64 = 30;
const SYS_KWM_UPDATE_WINDOW: u64 = 31;
const SYS_KWM_DESTROY_WINDOW: u64 = 32;
const SYS_EXIT: u64 = 34;
const SYS_SLEEP: u64 = 46;
const SYS_KWM_SET_TITLE: u64 = 60;
const SYS_KWM_UPDATE_WINDOW_RECT: u64 = 66;

/// The kernel copies at most UC_MAX_STR (1024) bytes per print.
const PRINT_BUF: usize = 512;

// ---------------------------------------------------------------------------
// Raw syscall layer
// ---------------------------------------------------------------------------

#[inline(always)]
unsafe fn syscall0(num: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "int $0x80",
        inlateout("rax") num => ret,
        out("rcx") _,
        out("rdx") _,
        out("rsi") _,
        out("rdi") _,
        out("r8") _,
        out("r9") _,
        out("r10") _,
        out("r11") _,
        options(nostack)
    );
    ret
}

#[inline(always)]
unsafe fn syscall1(num: u64, a1: u64) -> u64 {
    let ret: u64;
    // LLVM reserves rbx (PIC base / frame chain on some targets), so it
    // cannot appear as an asm operand. Push the callee-saved register
    // manually and load the argument from memory instead.
    core::arch::asm!(
        "push rbx",
        "mov rbx, [{a1_ptr}]",
        "int $0x80",
        "pop rbx",
        a1_ptr = in(reg) &a1,
        inlateout("rax") num => ret,
        out("rcx") _,
        out("rdx") _,
        out("rsi") _,
        out("rdi") _,
        out("r8") _,
        out("r9") _,
        out("r10") _,
        out("r11") _,
    );
    ret
}

#[inline(always)]
unsafe fn syscall3(num: u64, a1: u64, a2: u64, a3: u64) -> u64 {
    let ret: u64;
    // Same rbx treatment as syscall1: manual save/restore, memory load.
    core::arch::asm!(
        "push rbx",
        "mov rbx, [{a1_ptr}]",
        "int $0x80",
        "pop rbx",
        a1_ptr = in(reg) &a1,
        inlateout("rax") num => ret,
        in("rcx") a2,
        in("rdx") a3,
        out("rsi") _,
        out("rdi") _,
        out("r8") _,
        out("r9") _,
        out("r10") _,
        out("r11") _,
    );
    ret
}

/// Writes a NUL-terminated C string to the system console (syscall 1).
///
/// # Safety
/// `ptr` must point to a readable NUL-terminated string inside the caller's
/// address space. The kernel stops after `UC_MAX_STR` bytes.
pub unsafe fn print_cstr(ptr: *const u8) {
    syscall1(SYS_PRINT, ptr as u64);
}

/// Yields the CPU until the next interrupt (syscall 4).
pub unsafe fn yield_cpu() {
    syscall0(SYS_YIELD);
}

/// Allocates a uheap region (syscall 9).
///
/// Returns a page-aligned pointer or null on failure (size 0, >64MiB, OOM).
/// The kernel zero-fills and guard-pages every region.
pub unsafe fn alloc(size: usize) -> *mut u8 {
    syscall1(SYS_ALLOC, size as u64) as *mut u8
}

/// Frees a uheap region previously returned by `alloc` (syscall 10).
/// Foreign/stale pointers are validated (not dereferenced) and ignored.
pub unsafe fn free(ptr: *mut u8) {
    syscall1(SYS_FREE, ptr as u64);
}

/// Resizes a uheap region (syscall 19). May move the block; the old block is
/// freed on success. Returns null on failure with the old block intact.
pub unsafe fn realloc(ptr: *mut u8, old_size: usize, new_size: usize) -> *mut u8 {
    syscall3(SYS_REALLOC, ptr as u64, old_size as u64, new_size as u64) as *mut u8
}

/// Milliseconds since boot (syscall 14).
pub unsafe fn uptime() -> u64 {
    syscall0(SYS_UPTIME)
}

/// Sleeps for `ms` milliseconds without busy-waiting (syscall 46).
pub unsafe fn sleep(ms: u32) {
    syscall1(SYS_SLEEP, ms as u64);
}

/// Terminates this task / returns to the shell (syscall 34). Never returns.
pub unsafe fn exit() -> ! {
    syscall0(SYS_EXIT);
    loop {}
}

// ---------------------------------------------------------------------------
// Safe console layer
// ---------------------------------------------------------------------------

/// Stack-backed writer that flushes to the system console via syscall 1.
struct ConsoleWriter {
    buf: [u8; PRINT_BUF],
    len: usize,
}

impl ConsoleWriter {
    fn new() -> Self {
        Self { buf: [0; PRINT_BUF], len: 0 }
    }

    fn flush(&mut self) {
        if self.len == 0 {
            return;
        }
        self.buf[self.len] = 0;
        // Safety: buf is a local stack array, so the pointer is valid and the
        // NUL terminator is always written before calling.
        unsafe { print_cstr(self.buf.as_ptr()) };
        self.len = 0;
    }
}

impl Write for ConsoleWriter {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for byte in s.bytes() {
            if self.len >= PRINT_BUF - 1 {
                self.flush();
            }
            self.buf[self.len] = byte;
            self.len += 1;
        }
        Ok(())
    }
}

/// Prints a string to the system console.
pub fn print(s: &str) {
    let mut out = ConsoleWriter::new();
    let _ = out.write_str(s);
    out.flush();
}

/// Prints formatted output (e.g. from `info.message()` in a panic handler).
pub fn print_fmt(args: fmt::Arguments) {
    let mut out = ConsoleWriter::new();
    let _ = out.write_fmt(args);
    out.flush();
}

/// Prints an unsigned decimal integer.
pub fn print_u64(value: u64) {
    print_fmt(format_args!("{value}"));
}

// ---------------------------------------------------------------------------
// Event types and KWM window syscalls
// ---------------------------------------------------------------------------

/// Event type constants matching kernel/event.c and include/userlib.h.
pub const EVENT_NONE: u32 = 0;
pub const EVENT_KEY_PRESS: u32 = 1;
pub const EVENT_MOUSE_MOVE: u32 = 2;
pub const EVENT_MOUSE_CLICK: u32 = 3;
pub const EVENT_SCROLL: u32 = 4;
pub const EVENT_KEY_RELEASE: u32 = 5;
pub const EVENT_WIN_CLOSE: u32 = 6;

/// Keyboard modifier bitmask (matches KEY_MOD_* in include/userlib.h).
pub const KEY_MOD_SHIFT: u8 = 0x01;
pub const KEY_MOD_CTRL: u8 = 0x02;
pub const KEY_MOD_ALT: u8 = 0x04;
pub const KEY_MOD_CAPS: u8 = 0x08;

/// Kyuzen event structure (matches kyuzen_event_t in include/userlib.h).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KyuzenEvent {
    pub type_: u32,
    pub param1: i32,
    pub param2: i32,
    pub param3: i32,
    pub win_id: i32,
}

// Syscall 4 needs special handling for passing a pointer via rbx.
#[inline(always)]
unsafe fn syscall4(num: u64, a1: u64, a2: u64, a3: u64, a4: u64) -> u64 {
    let ret: u64;
    core::arch::asm!(
        "push rbx",
        "mov rbx, [{a1_ptr}]",
        "int $0x80",
        "pop rbx",
        a1_ptr = in(reg) &a1,
        inlateout("rax") num => ret,
        in("rcx") a2,
        in("rdx") a3,
        in("rsi") a4,
        out("rdi") _,
        out("r8") _,
        out("r9") _,
        out("r10") _,
        out("r11") _,
    );
    ret
}

/// Gets the next event from the per-task event queue (syscall 29).
/// Returns true if an event was retrieved, false if the queue was empty.
pub unsafe fn get_event(out: *mut KyuzenEvent) -> bool {
    if out.is_null() { return false; }
    syscall1(SYS_GET_EVENT, out as u64) != 0
}

/// Creates a KWM window at (x, y) with given width and height (syscall 30).
/// Returns the window ID (>=0) or -1 on failure.
pub unsafe fn kwm_create_window(x: i32, y: i32, width: u32, height: u32) -> i32 {
    syscall4(
        SYS_KWM_CREATE_WINDOW,
        x as u64,
        y as u64,
        width as u64,
        height as u64,
    ) as i32
}

/// Updates a KWM window's canvas with the provided XRGB8888 pixel buffer
/// (syscall 31: RBX=win_id, RCX=buffer). Returns the kernel validates that
/// the buffer belongs to the calling task and matches the window's canvas
/// dimensions before copying it into the KWM surface.
pub unsafe fn kwm_update_window(win_id: i32, buffer: *const u32) {
    if buffer.is_null() { return; }
    syscall3(SYS_KWM_UPDATE_WINDOW, win_id as u64, buffer as u64, 0);
}

/// Destroys a KWM window (syscall 32).
pub unsafe fn kwm_destroy_window(win_id: i32) {
    syscall1(SYS_KWM_DESTROY_WINDOW, win_id as u64);
}

/// Phase 3 — request for the partial window update (syscall 66). Layout MUST
/// match `kwm_rect_update_t` in include/userlib.h (x86_64, no packing):
/// i32, i32, i32, u32, u32, then an 8-byte pointer (4 bytes padding).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct KwmRectUpdate {
    pub win_id: i32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub buffer: *const u32,
}

/// Partial window update (syscall 66): copies only the request's rectangle
/// from the caller's full-stride canvas into the KWM window surface, row by
/// row. `buffer` is the full application canvas (`stride = window_width * 4`).
/// Returns 0 on success, -1 on rejection (bad window/owner/rect/buffer).
pub unsafe fn kwm_update_window_rect(req: *const KwmRectUpdate) -> i32 {
    syscall1(SYS_KWM_UPDATE_WINDOW_RECT, req as u64) as i32
}

/// Sets the title of a KWM window (syscall 60: RBX=win_id, RCX=NUL-terminated
/// title string). The kernel copies the string with its own bound. Returns 0 on
/// success, -1 on failure (invalid window or not the owner).
pub unsafe fn kwm_set_title(win_id: i32, title: *const u8) -> i32 {
    syscall3(SYS_KWM_SET_TITLE, win_id as u64, title as u64, 0) as i32
}

/// Gets the screen size in pixels (syscall 63). Returns 0 on success, -1 on failure.
pub unsafe fn kwm_get_screen_size(w: *mut u32, h: *mut u32) -> i32 {
    syscall3(63, w as u64, h as u64, 0) as i32
}