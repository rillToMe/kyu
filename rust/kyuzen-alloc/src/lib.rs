//! Rust global allocator backed by Kyuzen's userspace uheap syscalls.
//!
//! Pipeline: Rust alloc::alloc -> kyuzen-alloc (GlobalAlloc) -> kyuzen-sys
//! -> kernel uheap syscalls 9/10/19.
//!
//! Phase 1 mapped every Rust allocation to one page-granular uheap region.
//! That is correct but pathological for Slint: the kernel uheap never reuses
//! freed address space (`uheap_brk` only grows), so a long-running GUI that
//! allocates/frees per frame would exhaust the user range. Phase 2 adds a
//! small size-class cache in front of the syscalls: the kernel is asked for
//! 64 KiB chunks, and blocks are handed out of per-class free lists and
//! reused on free. Only large or highly-aligned requests go straight to the
//! kernel. The GlobalAlloc contract is unchanged; `hello-rs` still works.

#![no_std]

use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicBool, Ordering};

/// Largest alignment served by the cached path. uheap regions are page
/// aligned, so larger alignments go straight to `sys_alloc` (which is also
/// 4096-aligned) — anything above that is unsupported.
const MAX_SUPPORTED_ALIGN: usize = 4096;

/// Largest request served from the size-class cache. Bigger requests (e.g. a
/// window canvas) go directly to the kernel one region per allocation.
const MAX_CACHED: usize = 4096;

/// One kernel uheap region is requested per cache chunk. Carved blocks are
/// reused via the free lists, so region churn is bounded.
const CHUNK_SIZE: usize = 64 * 1024;

/// Size classes: 16-byte steps up to 128, then coarser powers of two. Every
/// class is a multiple of 16, so carving from a 4096-aligned chunk keeps each
/// block aligned to at least 16.
const CLASS_SIZES: [usize; 20] = [
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 256, 320, 384, 512, 768, 1024, 1536, 2048, 3072, 4096,
];

struct Heap {
    free_lists: [*mut u8; CLASS_SIZES.len()],
    bump: *mut u8,
    bump_end: *mut u8,
}

impl Heap {
    const fn new() -> Self {
        Self {
            free_lists: [ptr::null_mut(); CLASS_SIZES.len()],
            bump: ptr::null_mut(),
            bump_end: ptr::null_mut(),
        }
    }
}

struct HeapCell(UnsafeCell<Heap>);
// Safety: Rust userspace on Kyuzen is single-threaded (Slint is built with
// `unsafe-single-threaded`). The spin lock below additionally serialises the
// allocator if it is ever called from two contexts.
unsafe impl Sync for HeapCell {}

static HEAP: HeapCell = HeapCell(UnsafeCell::new(Heap::new()));
static LOCK: AtomicBool = AtomicBool::new(false);

struct LockGuard;

impl LockGuard {
    #[inline]
    fn acquire() -> Self {
        while LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        LockGuard
    }
}

impl Drop for LockGuard {
    #[inline]
    fn drop(&mut self) {
        LOCK.store(false, Ordering::Release);
    }
}

#[inline]
fn class_index_for(size: usize) -> Option<usize> {
    let mut i = 0;
    while i < CLASS_SIZES.len() {
        if CLASS_SIZES[i] >= size {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// True when this layout is served by the cached size-class path.
#[inline]
fn is_cached(layout: &Layout) -> bool {
    layout.align() <= 16 && layout.size() <= MAX_CACHED
}

/// Global allocator instance, installed in apps with #[global_allocator].
pub struct KyuzenAllocator;

unsafe impl GlobalAlloc for KyuzenAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = layout.size();
        if size == 0 {
            return NonNull::dangling().as_ptr();
        }
        if layout.align() > MAX_SUPPORTED_ALIGN {
            return ptr::null_mut();
        }

        if !is_cached(&layout) {
            return kyuzen_sys::alloc(size);
        }

        let class = match class_index_for(size) {
            Some(c) => c,
            None => return kyuzen_sys::alloc(size),
        };

        let _guard = LockGuard::acquire();
        let heap = &mut *HEAP.0.get();

        // Pop from the class free list first.
        let head = heap.free_lists[class];
        if !head.is_null() {
            heap.free_lists[class] = *(head as *mut *mut u8);
            return head;
        }

        // Otherwise carve a fresh block from the current chunk.
        let block = CLASS_SIZES[class];
        if heap.bump.is_null() || heap.bump.add(block) > heap.bump_end {
            let chunk = kyuzen_sys::alloc(CHUNK_SIZE);
            if chunk.is_null() {
                return ptr::null_mut();
            }
            heap.bump = chunk;
            heap.bump_end = chunk.add(CHUNK_SIZE);
        }
        let p = heap.bump;
        heap.bump = heap.bump.add(block);
        p
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.size() == 0 || ptr.is_null() {
            return;
        }
        if !is_cached(&layout) {
            kyuzen_sys::free(ptr);
            return;
        }
        let class = match class_index_for(layout.size()) {
            Some(c) => c,
            None => {
                kyuzen_sys::free(ptr);
                return;
            }
        };

        let _guard = LockGuard::acquire();
        let heap = &mut *HEAP.0.get();
        *(ptr as *mut *mut u8) = heap.free_lists[class];
        heap.free_lists[class] = ptr;
    }

    // The default realloc (alloc + copy + dealloc) from the trait is used.
    // It is correct for both the cached and direct paths; Slint's hot path is
    // not realloc-heavy enough to justify a bespoke implementation here.
}

// Note: no #[alloc_error_handler] here. It is still unstable on stable
// toolchains; since Rust 1.68 the compiler provides a default OOM handler
// that panics, which routes through the app's #[panic_handler] -> exit(34).
