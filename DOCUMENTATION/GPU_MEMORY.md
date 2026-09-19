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
- Reservation map (AL-4/5/7/8): `0x00000` legacy BCS ring (32KB),
  `0x10000` legacy status, `0x20000` Gen12 context (16KB), `0x24000`
  Gen12 status, `0x25000` Gen12 ring (16KB), buffers from 1MB.
  Framebuffer is never GTT-mapped (scanout is a CPU zero-copy wrap).
- `intel_gtt_map_pages()` is frozen (AL-4 reverted an overlap check to
  keep it byte-identical); Gen12 adds only a read-only PTE accessor.

## Gen12 PPGTT alias (AL-9, `intel_gen12_ppgtt.c`)

XY commands resolve through the context VM, so a minimal 3-level VM
mirrors the GGTT window: `CTX_PDP0 → PD[0] → PT[0..511]`, PDP1-3 = 0
(never walked below 1GB). Encodings from i915 (`gen8_ppgtt.c`,
`intel_gtt.h`): leaf = `phys|PRESENT|RW` (PAT0), PD = `phys|PRESENT|RW`
(`PPAT_CACHED_PDE` = 0), PDP slots take physical addresses. Re-synced
from live GGTT PTEs before every submit carrying XY (inside the single
owner lock); tables CPU-written + `clflush`ed.

## CPU/GPU coherency discipline

- Buffers with CPU-written patterns are `clflush`ed before submit;
  GPU-written buffers are `clflush`ed after the fence before CPU reads.
  x86 `CLFLUSH`, no HW assumptions. The iGPU snoops LLC, but validation
  never trusts the cache across a GPU write.

## Surfaces (`intel_gsurf_t`)

GPU buffer + geometry (w/h/stride), XRGB8888 only. OOB writes ignored,
OOB reads return 0. GHAL surfaces (`intel_init.c`) prefer GPU backing
with kmalloc fallback; scanout wraps the fb (no GTT mapping, CPU path).

## Lifetime

Create -> map -> use -> unmap -> destroy. Destroy is idempotent-safe
(NULL/-1 tolerated). Validators (`intel_rect.h`) reject unmapped/NULL
buffers before any GPU use. PMM owns the pages; GTT owns nothing (PTEs
are mappings, cleared on unmap/destroy).
