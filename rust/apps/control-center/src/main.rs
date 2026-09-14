//! control-center — a multi-view dashboard on the KyuzenOS Slint platform.
//! Proves the no_std Slint -> kyuzen-gui pipeline on a complex UI: sidebar
//! navigation, switchable views, live widgets, a process list, settings form
//! and a small terminal log. Rust owns the mutable state (process table, log
//! buffer, settings), Slint renders it.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::rc::Rc;
use alloc::string::String;
use alloc::vec;

use kyuzen_alloc::KyuzenAllocator as Allocator;
use kyuzen_sys as sys;
use slint::{SharedString, VecModel};

#[global_allocator]
static ALLOCATOR: Allocator = Allocator;

slint::include_modules!();

const WINDOW_W: u32 = 640;
const WINDOW_H: u32 = 460;

fn proc(name: &str, pid: i32, cpu: f32, mem: f32) -> ProcessInfo {
    ProcessInfo {
        name: name.into(),
        pid,
        cpu,
        mem,
    }
}

fn status_for(idx: i32) -> &'static str {
    match idx {
        0 => "overview",
        1 => "processes",
        2 => "settings",
        _ => "logs",
    }
}

#[no_mangle]
pub extern "C" fn main() -> ! {
    sys::print("[control-center] starting\n");

    let window = match kyuzen_gui::KyuzenWindow::new(120, 80, WINDOW_W, WINDOW_H) {
        Ok(w) => w,
        Err(_) => {
            sys::print("[control-center] KWM window creation failed\n");
            unsafe { sys::exit() }
        }
    };
    window.set_title(b"Control Center");
    kyuzen_gui::set_platform(window.clone());

    let ui = match ControlCenter::new() {
        Ok(ui) => ui,
        Err(e) => {
            sys::print("[control-center] Slint init failed: ");
            sys::print_fmt(format_args!("{e}"));
            sys::print("\n");
            unsafe { sys::exit() }
        }
    };

    ui.set_processes(Rc::new(VecModel::from(vec![
        proc("kernel", 0, 8.0, 21.0),
        proc("kwm", 1, 2.5, 12.0),
        proc("shell", 2, 1.2, 6.0),
        proc("hello-slint", 3, 0.4, 3.5),
        proc("control-center", 4, 0.6, 4.0),
        proc("e1000", 5, 0.1, 2.0),
        proc("kyuzenfs", 6, 0.2, 2.5),
    ]))
    .into());
    ui.set_statusMsg(SharedString::from("overview"));
    ui.set_log(SharedString::from("Welcome to KyuzenOS shell\n> "));

    let weak = ui.as_weak();
    ui.on_navigate(move |idx| {
        if let Some(ui) = weak.upgrade() {
            ui.set_activeView(idx);
            ui.set_statusMsg(SharedString::from(status_for(idx)));
        }
    });

    let weak = ui.as_weak();
    ui.on_saveSettings(move || {
        if let Some(ui) = weak.upgrade() {
            let who = ui.get_username();
            let who = if who.is_empty() { "guest" } else { who.as_str() };
            let b = (ui.get_brightness() * 100.0) as i32;
            let v = (ui.get_volume() * 100.0) as i32;
            ui.set_statusMsg(SharedString::from(format!("saved {who} b{b}% v{v}%")));
        }
    });

    let mut log = String::from("Welcome to KyuzenOS shell\n> ");
    let mut command_id: u64 = 0;
    let weak = ui.as_weak();
    ui.on_runCommand(move |cmd| {
        if let Some(ui) = weak.upgrade() {
            let trimmed: String = cmd.trim().into();
            if trimmed == "clear" {
                log.clear();
            } else {
                log.push_str(&trimmed);
                log.push('\n');
                command_id += 1;
                log.push_str(&dispatch(&trimmed, command_id));
                log.push('\n');
            }
            log.push_str("> ");
            ui.set_log(SharedString::from(log.as_str()));
        }
    });

    sys::print("[control-center] running\n");
    kyuzen_gui::run(&window);

    sys::print("[control-center] window closed, exiting\n");
    unsafe {
        sys::kwm_destroy_window(window.kmw_id());
        sys::exit()
    }
}

/// Tiny command dispatcher for the built-in log pane. Returns the echoed line.
fn dispatch(cmd: &str, id: u64) -> String {
    match cmd {
        "" => String::new(),
        "help" => "commands: help, clear, uptime, hello, theme".into(),
        "uptime" => format!("[{}] uptime {} ms", id, unsafe { sys::uptime() }),
        "hello" => "hello from control-center".into(),
        "theme" => "theme is dark".into(),
        c => format!("unknown command: {c}"),
    }
}

#[panic_handler]
fn panic_handler(info: &core::panic::PanicInfo) -> ! {
    sys::print("\n[control-center] RUST PANIC: ");
    sys::print_fmt(format_args!("{info}"));
    sys::print("\n");
    unsafe { sys::exit() }
}
