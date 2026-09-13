//! KyuzenOS Slint platform adapter.
//!
//! Bridges Slint's embedded (`no_std`) platform API onto the Kyuzen userspace
//! ABI. The architecture is deliberately thin:
//!
//! ```text
//!   Slint UI  ->  SoftwareRenderer (partial)  ->  persistent XRGB8888 canvas
//!        |                                              |
//!        | damage region                    sys_kwm_update_window_rect (66)
//!        v                                              v
//!   KyuzenPlatform                       kernel KWM -> compositor -> display
//!        ^
//!   sys_get_event (syscall 29)  <- per-task event queue
//! ```
//!
//! Phase 3: `SoftwareRenderer::render` already redraws only Slint's dirty
//! region into the reused canvas and returns that region as a `PhysicalRegion`.
//! We upload only those rectangles via syscall 66; syscall 31 (full canvas) is
//! kept as a fallback and remains unchanged for the C/C++ apps.
//!
//! KWM stays authoritative for window identity, geometry, focus, z-order,
//! decorations and close. This crate owns none of that: it only creates a KWM
//! window, renders Slint into the matching content-sized canvas and translates
//! Kyuzen events into `slint::platform::WindowEvent`s.

#![no_std]

extern crate alloc;

use alloc::boxed::Box;
use alloc::rc::Rc;
use alloc::vec::Vec;
use core::cell::{Cell, RefCell};
use core::time::Duration;

use kyuzen_sys as sys;
use slint::platform::software_renderer::{
    MinimalSoftwareWindow, PhysicalRegion, PremultipliedRgbaColor, RepaintBufferType, TargetPixel,
};
use slint::platform::{Key, Platform, PlatformError, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{LogicalPosition, PhysicalSize, SharedString};

/// Upper bound on damage rectangles submitted per frame. Slint coalesces its
/// repaint region to at most `DirtyRegion::MAX_COUNT` (3) boxes, but
/// `PhysicalRegion::iter()` may split them into a few non-overlapping rects.
/// Beyond this we fall back to a single full-canvas update (still correct,
/// just less optimal).
const MAX_DAMAGE_RECTS: usize = 8;

/// One XRGB8888 pixel in Kyuzen's window-canvas byte order (u32 little-endian:
/// B, G, R, X). The high byte is set to 0xFF to match libgui/libui canvases —
/// Kyuzen's compositor treats a non-zero high byte as opaque.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Xrgb8888(pub u32);

impl TargetPixel for Xrgb8888 {
    #[inline]
    fn blend(&mut self, color: PremultipliedRgbaColor) {
        let a = (u8::MAX - color.alpha) as u32;
        let r = ((self.0 >> 16) & 0xFF) * a / 255 + color.red as u32;
        let g = ((self.0 >> 8) & 0xFF) * a / 255 + color.green as u32;
        let b = (self.0 & 0xFF) * a / 255 + color.blue as u32;
        self.0 = 0xFF00_0000
            | ((r & 0xFF) << 16)
            | ((g & 0xFF) << 8)
            | (b & 0xFF);
    }

    #[inline]
    fn from_rgb(red: u8, green: u8, blue: u8) -> Self {
        Self(0xFF00_0000 | ((red as u32) << 16) | ((green as u32) << 8) | (blue as u32))
    }

    #[inline]
    fn background() -> Self {
        Self(0xFF00_0000)
    }
}

/// A Slint window backed by a Kyuzen KWM window.
///
/// Owns the KWM window id, the content-sized XRGB8888 canvas (allocated by the
/// global kyuzen-alloc), and the `MinimalSoftwareWindow` that supplies the
/// `SoftwareRenderer`. All `WindowAdapter` calls are delegated to the inner
/// Slint window; rendering and event translation are Kyuzen-specific.
pub struct KyuzenWindow {
    inner: Rc<MinimalSoftwareWindow>,
    kmw_id: i32,
    width: u32,
    height: u32,
    canvas: RefCell<Vec<Xrgb8888>>,
    /// Last window-local pointer position. Kyuzen's MOUSE_CLICK event carries
    /// the button/edge state only, not coordinates, so click positions reuse
    /// the most recent move.
    last_pos: Cell<(f32, f32)>,
    close_requested: Cell<bool>,
}

impl KyuzenWindow {
    /// Creates a KWM window (content size `width x height`) and the matching
    /// Slint software-rendered canvas.
    pub fn new(x: i32, y: i32, width: u32, height: u32) -> Result<Rc<Self>, PlatformError> {
        if width == 0 || height == 0 {
            return Err(PlatformError::Other("zero-sized window".into()));
        }
        let px = (width as usize)
            .checked_mul(height as usize)
            .ok_or_else(|| PlatformError::Other("window size overflow".into()))?;

        let kmw_id = unsafe { sys::kwm_create_window(x, y, width, height) };
        if kmw_id < 0 {
            return Err(PlatformError::Other("kwm_create_window failed".into()));
        }

        let inner = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);
        inner.set_size(PhysicalSize::new(width, height));

        let mut canvas = Vec::new();
        canvas.resize(px, Xrgb8888(0xFF00_0000));

        Ok(Rc::new(Self {
            inner,
            kmw_id,
            width,
            height,
            canvas: RefCell::new(canvas),
            last_pos: Cell::new((0.0, 0.0)),
            close_requested: Cell::new(false),
        }))
    }

    /// KWM window id (needed by the app for teardown / title).
    pub fn kmw_id(&self) -> i32 {
        self.kmw_id
    }

    /// Sets the KWM-owned titlebar/taskbar title (syscall 60).
    pub fn set_title(&self, title: &[u8]) -> i32 {
        // NUL-terminate defensively; the kernel copies with its own bound.
        let mut buf = [0u8; 32];
        let n = title.len().min(buf.len() - 1);
        buf[..n].copy_from_slice(&title[..n]);
        unsafe { sys::kwm_set_title(self.kmw_id, buf.as_ptr()) }
    }

    /// True once a `CloseRequested` has been received from KWM.
    pub fn close_requested(&self) -> bool {
        self.close_requested.get()
    }

    /// Renders only if Slint marked the scene dirty, then uploads only the
    /// damaged rectangles to KWM via syscall 66. Returns true if a frame was
    /// drawn.
    ///
    /// `SoftwareRenderer::render` with `RepaintBufferType::ReusedBuffer`
    /// already redraws only the dirty region into the persistent canvas and
    /// returns that region as a `PhysicalRegion`; the canvas outside the
    /// returned rects is left untouched. Idle frames render nothing and copy
    /// nothing.
    pub fn draw_if_needed(&self) -> bool {
        let mut region: Option<PhysicalRegion> = None;
        let drew = {
            let mut canvas = self.canvas.borrow_mut();
            let width = self.width as usize;
            self.inner.draw_if_needed(|renderer| {
                region = Some(renderer.render(canvas.as_mut_slice(), width));
            })
        };
        if !drew {
            return false;
        }

        let canvas = self.canvas.borrow();
        let buffer = canvas.as_ptr() as *const u32;
        match region {
            Some(region) => self.upload_region(&region, buffer),
            // Drew but no region reported: safest is a full upload.
            None => self.upload_full(buffer),
        }
        true
    }

    /// Uploads the damage region rectangle-by-rectangle via syscall 66. Falls
    /// back to the unchanged full syscall-31 update when the region is too
    /// fragmented to be worth many small copies.
    fn upload_region(&self, region: &PhysicalRegion, buffer: *const u32) {
        let mut rects = [(0i32, 0i32, 0u32, 0u32); MAX_DAMAGE_RECTS];
        let mut count = 0usize;
        let mut partial_bytes = 0u64;
        for (pos, size) in region.iter() {
            if count >= MAX_DAMAGE_RECTS {
                self.upload_full(buffer);
                return;
            }
            if let Some(r) =
                clip_rect(pos.x, pos.y, size.width, size.height, self.width, self.height)
            {
                partial_bytes += r.2 as u64 * r.3 as u64 * 4;
                rects[count] = r;
                count += 1;
            }
        }
        if count == 0 {
            // Dirty set but nothing inside the content area: nothing to send.
            return;
        }

        let mut ok = true;
        for &(x, y, w, h) in &rects[..count] {
            let req = sys::KwmRectUpdate {
                win_id: self.kmw_id,
                x,
                y,
                width: w,
                height: h,
                buffer,
            };
            // Safety: `req` is a fully initialised local and `buffer` points at
            // this window's persistent canvas. The kernel independently
            // validates ownership, rectangle bounds and the user-buffer span.
            if unsafe { sys::kwm_update_window_rect(&req) } != 0 {
                ok = false;
                break;
            }
        }
        if !ok {
            // Partial update rejected (e.g. window gone): fall back to full.
            self.upload_full(buffer);
            return;
        }
        damage_debug(count, partial_bytes, self.full_bytes());
    }

    /// Full-canvas fallback: the unchanged Phase 2 / syscall-31 path.
    fn upload_full(&self, buffer: *const u32) {
        // Safety: `buffer` is this window's persistent canvas.
        unsafe { sys::kwm_update_window(self.kmw_id, buffer) };
        damage_debug(1, self.full_bytes(), self.full_bytes());
    }

    fn full_bytes(&self) -> u64 {
        self.width as u64 * self.height as u64 * 4
    }

    /// Translates and dispatches every queued Kyuzen event, then renders if
    /// needed. Returns true when the window should close.
    pub fn pump(&self) -> bool {
        let mut ev = sys::KyuzenEvent::default();
        while unsafe { sys::get_event(&mut ev) } {
            if let Some(we) = self.translate(&ev) {
                self.inner.window().dispatch_event(we);
            }
            if self.close_requested.get() {
                return true;
            }
        }
        self.draw_if_needed();
        self.close_requested.get()
    }

    fn translate(&self, ev: &sys::KyuzenEvent) -> Option<WindowEvent> {
        match ev.type_ {
            sys::EVENT_MOUSE_MOVE => {
                let (x, y) = (ev.param1 as f32, ev.param2 as f32);
                self.last_pos.set((x, y));
                Some(WindowEvent::PointerMoved { position: LogicalPosition::new(x, y) })
            }
            sys::EVENT_MOUSE_CLICK => {
                let (x, y) = self.last_pos.get();
                let button = match ev.param1 {
                    0 => PointerEventButton::Left,
                    1 => PointerEventButton::Right,
                    _ => PointerEventButton::Other,
                };
                let position = LogicalPosition::new(x, y);
                if ev.param2 != 0 {
                    Some(WindowEvent::PointerPressed { position, button })
                } else {
                    Some(WindowEvent::PointerReleased { position, button })
                }
            }
            sys::EVENT_SCROLL => {
                let (x, y) = self.last_pos.get();
                Some(WindowEvent::PointerScrolled {
                    position: LogicalPosition::new(x, y),
                    delta_x: 0.0,
                    // PS/2: +1 = wheel down, -1 = wheel up. Slint scrolls down
                    // on positive delta_y. 40 logical px per notch.
                    delta_y: ev.param1 as f32 * 40.0,
                })
            }
            sys::EVENT_KEY_PRESS => {
                key_text(ev.param3, ev.param1).map(|text| WindowEvent::KeyPressed { text })
            }
            sys::EVENT_KEY_RELEASE => {
                key_text(ev.param3, ev.param1).map(|text| WindowEvent::KeyReleased { text })
            }
            sys::EVENT_WIN_CLOSE => {
                self.close_requested.set(true);
                Some(WindowEvent::CloseRequested)
            }
            _ => None,
        }
    }
}

impl WindowAdapter for KyuzenWindow {
    fn window(&self) -> &slint::Window {
        self.inner.window()
    }

    fn renderer(&self) -> &dyn slint::platform::Renderer {
        self.inner.renderer()
    }

    fn size(&self) -> PhysicalSize {
        self.inner.size()
    }

    fn set_size(&self, size: slint::WindowSize) {
        // KWM window geometry is fixed at creation (no resize in Phase 2);
        // keep Slint's view in sync with the content size we created.
        self.inner.set_size(size);
    }

    fn request_redraw(&self) {
        self.inner.request_redraw();
    }
}

/// Slint `Platform` implementation. Single-threaded by construction, matching
/// the `unsafe-single-threaded` Slint build.
pub struct KyuzenPlatform {
    window: Rc<KyuzenWindow>,
}

impl KyuzenPlatform {
    pub fn new(window: Rc<KyuzenWindow>) -> Self {
        Self { window }
    }
}

impl Platform for KyuzenPlatform {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.window.clone())
    }

    fn duration_since_start(&self) -> Duration {
        // syscall 14 returns milliseconds since boot (monotonic).
        Duration::from_micros(unsafe { sys::uptime() } * 1000)
    }

    fn run_event_loop(&self) -> Result<(), PlatformError> {
        run(&self.window);
        Ok(())
    }
}

/// Registers the platform exactly once. Must be called before creating any
/// Slint component.
pub fn set_platform(window: Rc<KyuzenWindow>) {
    let _ = slint::platform::set_platform(Box::new(KyuzenPlatform::new(window)));
}

/// Drives the Slint event loop around Kyuzen's existing event model:
/// pump events -> dispatch -> update timers/animations -> draw if dirty ->
/// `sys_yield()` when idle. Returns after the KWM close event.
pub fn run(window: &Rc<KyuzenWindow>) {
    // Force the first frame; `draw_if_needed` is otherwise a no-op until Slint
    // marks the scene dirty.
    window.request_redraw();
    loop {
        slint::platform::update_timers_and_animations();
        if window.pump() {
            break;
        }
        // Non-busy idle: syscall 4 halts until the next interrupt (timer tick).
        unsafe { sys::yield_cpu() };
    }
}

/// Clips a physical damage rectangle to the window content, returning
/// `(x, y, width, height)` or `None` when the intersection is empty. Uses i64
/// so a negative origin or an oversized width cannot overflow.
fn clip_rect(
    x: i32,
    y: i32,
    w: u32,
    h: u32,
    win_w: u32,
    win_h: u32,
) -> Option<(i32, i32, u32, u32)> {
    if w == 0 || h == 0 {
        return None;
    }
    let x0 = (x as i64).max(0);
    let y0 = (y as i64).max(0);
    let x1 = (x as i64 + w as i64).min(win_w as i64);
    let y1 = (y as i64 + h as i64).min(win_h as i64);
    if x1 <= x0 || y1 <= y0 {
        return None;
    }
    Some((x0 as i32, y0 as i32, (x1 - x0) as u32, (y1 - y0) as u32))
}

/// Compile-time damage instrumentation. Off by default; enable the
/// `damage-debug` feature to print per-frame rect count and byte counts
/// (console output is mirrored to COM1 by the kernel TTY).
#[cfg(feature = "damage-debug")]
fn damage_debug(rects: usize, partial_bytes: u64, full_bytes: u64) {
    sys::print("[damage] rects=");
    sys::print_u64(rects as u64);
    sys::print(" partial=");
    sys::print_u64(partial_bytes);
    sys::print(" full=");
    sys::print_u64(full_bytes);
    sys::print("\n");
}
#[cfg(not(feature = "damage-debug"))]
fn damage_debug(_rects: usize, _partial_bytes: u64, _full_bytes: u64) {}

/// Maps a Kyuzen set-1 scancode (already carrying the 0x100 extended bit in
/// bit 8) to the Slint key text. Returns the printable ASCII character when
/// available, otherwise the named `Key`. Unsupported keys return `None` and
/// are dropped gracefully.
fn key_text(key_id: i32, ascii: i32) -> Option<SharedString> {
    let named: Option<Key> = match key_id {
        0x01 => Some(Key::Escape),
        0x0E => Some(Key::Backspace),
        0x0F => Some(Key::Tab),
        0x1C => Some(Key::Return),
        0x2A => Some(Key::Shift),
        0x36 => Some(Key::ShiftR),
        0x1D => Some(Key::Control),
        0x11D => Some(Key::ControlR),
        0x38 => Some(Key::Alt),
        0x138 => Some(Key::AltGr),
        0x3A => Some(Key::CapsLock),
        0x148 => Some(Key::UpArrow),
        0x150 => Some(Key::DownArrow),
        0x14B => Some(Key::LeftArrow),
        0x14D => Some(Key::RightArrow),
        0x147 => Some(Key::Home),
        0x14F => Some(Key::End),
        0x149 => Some(Key::PageUp),
        0x151 => Some(Key::PageDown),
        0x152 => Some(Key::Insert),
        0x153 => Some(Key::Delete),
        _ => None,
    };
    if let Some(k) = named {
        return Some(k.into());
    }
    // Printable ASCII (includes Shift/Caps-resolved characters from the kernel).
    // Space is handled here as an ordinary character too.
    if (0x20..0x7F).contains(&ascii) {
        let c = char::from(ascii as u8);
        return Some(c.into());
    }
    None
}
