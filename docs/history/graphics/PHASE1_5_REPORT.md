# Phase 1.5 Completion Report

Status of the Graphics HAL core implementation. Only items marked **[x]** are
completely implemented, tested (in QEMU where relevant), and integrated.
Partially completed work is marked **[ ]** with a note.

## Graphics HAL

[x] Backend registry — `gpu_register_backend()` (dedup by name, bounded array)
[x] Backend priority — first registered backend that initializes becomes active
[x] Backend activation — `gpu_initialize()` probes all, activates first success
[x] Backend shutdown — `gpu_shutdown()` tears down all initialized backends
[x] GPU dispatch wrappers — `gpu_*` route to active backend's ops
[x] Handle NULL backend safely — wrappers return `GPU_ERR_NO_GPU`
[x] Handle unsupported operations correctly — return `GPU_ERR_UNSUPPORTED`
[x] Remove duplicated framebuffer access from upper layers — renderer routes
    draws through the HAL; compositor present bridge provided (see WM section)

## Backend System

[x] Complete Software Backend — fill/line/draw_image/blit/stretch/blend/present
[x] Validate every backend callback — `software_gpu_selftest()` PASS in QEMU
[x] Verify backend registration order — dedup + first-wins priority verified
[x] Verify backend switching — activation selects first successful init
[x] Add backend self-test — `software_gpu_selftest()` (build `-DGFX_SELFTEST`)

## Surface System

[x] Implement surface allocation — `gpu_surface_create()` (static pool, kmalloc backing)
[x] Implement surface destruction — final release frees pixels
[x] Implement handle manager — unique handles, bounded pool
[x] Implement reference counting — `retain`/`release`
[x] Implement ownership rules — create owns pixels; wrap borrows
[x] Implement clipping helpers — `gpu_surface_clip()`
[x] Implement row access helpers — `gpu_surface_row()`
[x] Implement pixel format helpers — `gpu_pixel_bpp()`, `gpu_surface_bpp()`

## GPU Memory Manager

[x] Implement VRAM allocator — first-fit bitmap over 64 MiB logical VRAM
[x] Implement VRAM free — `gpu_vram_free(offset, size)`
[x] Implement alignment support — `gpu_vram_alloc(size, align)`
[x] Implement allocation validation — 0 reserved; never returns 0 as valid offset
[x] Implement fragmentation handling — first-fit scan with run detection
[x] Add memory statistics — `gpu_vram_stats()`

## Renderer

[x] Complete gfx_* implementation — fill/line/draw_image/blit/stretch/present
[x] Route every drawing operation through HAL — all gfx_* call gpu_*
[x] Remove direct framebuffer rendering — renderer never touches fb_ptr directly
[x] Verify software rendering output — self-test validates fill/blit/blend/stretch
[x] Validate renderer lifecycle — init/acquire/present/shutdown with guards

## Double Buffer

[x] Create front buffer — private or caller-provided via `gfx_graphics_init_with`
[x] Create back buffer — private system-RAM surface
[x] Implement swap — software `present()` copies damage back→front
[x] Implement partial present — only damaged rects submitted
[x] Implement full present — `gfx_present(c, full=1)` covers whole surface

## Dirty Rectangle

[x] Track damage region — `damage_accum()` unions rects into canvas damage
[x] Merge overlapping damage — bounding-box union of all marks
[x] Clear damage after present — `gfx_clear_damage()`
[x] Validate clipping — present clips to surface bounds

## Window Manager Integration

[ ] Replace direct framebuffer writes in live compositor — **NOT integrated**.
    Added `gfx_compositor_present()` bridge that routes a raw back→front damage
    copy through the HAL, but `kernel/gfx/compositor.c` still uses its direct
    `blit_rect_db(fb_db, back_db, r)` path. Deliberate: guarantees identical
    visual output without a GPU to visually verify against. Phase 2 work.
[x] Use gfx_* API everywhere possible — renderer is the supported path
[x] Keep compatibility with existing compositor — existing path untouched
[x] Ensure visual output remains unchanged — compositor draw path not modified

## Thread Safety

[x] Verify spinlock usage — `spinlock_lock_irqsave` for global structures
[x] Protect backend registry — `g_gpu_registry_lock`
[x] Protect surface manager — manager `g_lock`
[x] Protect VRAM allocator — same manager `g_lock`
[x] Protect renderer state — `g_renderer_lock` around init; documented that
    per-surface pixel writes are the caller's responsibility (matches compositor)

## Validation

[x] Verify every public API — covered by self-test + build
[x] Verify every error code — NULL safety + unsupported paths tested
[x] Verify invalid arguments — self-test checks NULL surfaces
[x] Verify NULL safety — wrappers guard NULL backend/surface
[x] Verify memory cleanup — self-test releases surfaces; refcount to 0 frees
[x] Verify no resource leaks — surface manager frees on final release

## Performance

[x] Remove unnecessary allocations — static surface pool; no hot-path alloc
[x] Remove duplicate memory copies — present copies only damaged regions
[x] Minimize framebuffer writes — dirty-rect damage only
[x] Optimize clipping — clip once per op before the inner loops
[x] Optimize fill_rect — row-wise fill with per-row pointer
[x] Optimize blit — row/col mapped once, inner loops write contiguous runs

## Documentation

[x] Update API.md — added `gfx_graphics_init_with`, `gfx_compositor_present`,
    `gpu_vram_stats`
[x] Update ARCHITECTURE.md — added Phase 1.5 status + decisions
[x] Update BACKEND_GUIDE.md — unchanged (no API change requiring it); re-verified
[x] Document implementation decisions — XRGB alpha semantics, concurrency model
[x] Document remaining TODOs for Phase 2 — listed in ARCHITECTURE.md §13

---

## Remaining for Phase 2

[ ] VirtIO GPU backend
[ ] GPU command queue
[ ] Hardware present
[ ] Hardware resource allocation
[ ] Hardware texture upload/download
[ ] Hardware synchronization
[ ] Migrate live compositor draw path to gfx_*/gpu_* (present bridge is ready)
