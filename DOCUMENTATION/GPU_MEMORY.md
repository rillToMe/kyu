# GPU Memory — KyuzenOS Intel 2D Backend

The iGPU has no VRAM. Everything is system RAM with two views:
CPU via HHDM, GPU via GTT. `graphics/backend/intel_gpu_alloc.*`,
`intel_gtt.*`, `intel_surface.*`.

## Layers

```
PMM pages (phys)
  |-- CPU view: phys + hhdm_offset (per-page; may be non-contiguous)
  `-- GPU view: GTT PTE -> gpu_vaddr (entry N covers page N, 4KB)
```

## GPU buffers (`intel_gpu_buffer_t`)

- Create: `intel_gpu_buffer_create(w, h)` — PMM pages, zeroed, overflow-checked.
  Max 64 pages (256KB), 32 concurrent buffers.
- `cpu_ptr` valid only when pages are contiguous; all internal users do
  per-page access (`byte -> page = byte>>12`), so non-contiguity is normal.
- Map: `intel_gpu_buffer_map_gpu()` assigns a bump vaddr from 1MB and writes
  PTEs at `vaddr>>12` (fixed Phase-8 bug: index used to be an independent
  counter, so the GPU read the wrong entries). Destroy unmaps + frees.

## GTT (`intel_gtt.c`, 512 entries = 2MB)

- Table: 512 x 8B in contiguous RAM (Gen12-compatible; Gen8-11 ignore HI).
  Programmed via `GTT_BASE_LOW/HIGH`, `GTT_SIZE`, `GTT_MEM_CTRL`; TLB flush
  via `GTT_INV` + spin.
- Reservation: `0x00000` BCS ring (32KB), `0x10000` fence status page,
  buffers from 1MB. Framebuffer is never GTT-mapped (scanout is a CPU
  zero-copy wrap; GPU commands target private buffers only).

## Surfaces (`intel_gsurf_t`)

GPU buffer + geometry (w/h/stride), XRGB8888 only. OOB writes ignored,
OOB reads return 0. GHAL surfaces (`intel_init.c`) prefer GPU backing
with kmalloc fallback; scanout wraps the fb (no GTT mapping, CPU path).

## Lifetime

Create -> map -> use -> unmap -> destroy. Destroy is idempotent-safe
(NULL/-1 tolerated). Validators (`intel_rect.h`) reject unmapped/NULL
buffers before any GPU use. PMM owns the pages; GTT owns nothing (PTEs
are mappings, cleared on unmap/destroy).
