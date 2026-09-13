//! hello-slint — Phase 2 end-to-end proof: Slint (no_std) -> kyuzen-gui
//! WindowAdapter -> SoftwareRenderer -> XRGB8888 canvas -> syscall 31 -> KWM.
//!
//! The window is a plain KWM window (titlebar/shadows owned by the kernel).
//! Slint owns only the content area; clicking the button updates `counter`.

#![no_std]
#![no_main]

extern crate alloc;

use kyuzen_alloc::KyuzenAllocator as Allocator;
use kyuzen_sys as sys;
use slint::ComponentHandle;

#[global_allocator]
static ALLOCATOR: Allocator = Allocator;

slint::include_modules!();

const WINDOW_W: u32 = 360;
const WINDOW_H: u32 = 300;

#[no_mangle]
pub extern "C" fn main() -> ! {
    sys::print("[hello-slint] starting\n");

    let window = match kyuzen_gui::KyuzenWindow::new(80, 60, WINDOW_W, WINDOW_H) {
        Ok(w) => w,
        Err(_) => {
            sys::print("[hello-slint] KWM window creation failed\n");
            unsafe { sys::exit() }
        }
    };
    window.set_title(b"Hello Slint");
    kyuzen_gui::set_platform(window.clone());

    let ui = match App::new() {
        Ok(ui) => ui,
        Err(e) => {
            sys::print("[hello-slint] Slint init failed: ");
            sys::print_fmt(format_args!("{e}"));
            sys::print("\n");
            unsafe { sys::exit() }
        }
    };

    let weak = ui.as_weak();
    ui.on_increment(move || {
        if let Some(ui) = weak.upgrade() {
            ui.set_counter(ui.get_counter() + 1);
        }
    });

    sys::print("[hello-slint] running\n");
    kyuzen_gui::run(&window);

    sys::print("[hello-slint] window closed, exiting\n");
    unsafe {
        sys::kwm_destroy_window(window.kmw_id());
        sys::exit()
    }
}

#[panic_handler]
fn panic_handler(info: &core::panic::PanicInfo) -> ! {
    sys::print("\n[hello-slint] RUST PANIC: ");
    sys::print_fmt(format_args!("{info}"));
    sys::print("\n");
    unsafe { sys::exit() }
}
