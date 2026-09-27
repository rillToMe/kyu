# Graphics Backends

KyuzenOS ships three graphics backends behind the Graphics HAL (GHAL). This
document describes each and how to add a new one.

## Backend Summary

| Backend | File | Capabilities | Acceleration |
| --- | --- | --- | --- |
| Software | `graphics/backend/software.c` | `PARTIAL_FLUSH` | None (CPU) |
| VirtIO-GPU | `graphics/backend/virtio_gpu.c` | `PARTIAL_FLUSH`, `ASYNC_PRESENT` | 2D blit/fill via host |
| Intel iGPU | `graphics/backend/intel/intel_init.c` | `PARTIAL_FLUSH` | 2D blit/fill via BCS engine |

Selection order in `ghal_init()`: **VirtIO-GPU → Intel → Software**. The
software backend never fails, so a backend is always active.

## Software Backend

The software backend wraps the Limine framebuffer. It provides the reference
implementation and the guaranteed fallback.

- `surface_create_scanout` wraps the hardware framebuffer zero-copy
  (`owns_pixels = 0`); uploads write straight to the screen, and present is a
  no-op.
- It advertises only `GHAL_CAP_PARTIAL_FLUSH`.
- All 2D operations (fill, blit, upload) are performed on the CPU.

## VirtIO-GPU Backend

`graphics/backend/virtio_gpu.c` drives the VirtIO-GPU device through the
generic virtqueue implementation (`drivers/graphics/hw/virtqueue.c`).

### Device and queues

| Property | Value |
| --- | --- |
| PCI vendor | `VIRTIO_PCI_VENDOR_ID` = `0x1AF4` |
| PCI device | `VIRTIO_PCI_DEVICE_GPU` = `0x1050` |
| Control queue | index 0, requested size 32 |
| Cursor queue | index 1, requested size 16 |
| Max queue size | `VG_VQ_MAX_SIZE` = 256 |

### Commands

Opcodes (`virtio_gpu_regs.h`):

| Command | Value |
| --- | --- |
| `GET_DISPLAY_INFO` | `0x0100` |
| `RESOURCE_CREATE_2D` | `0x0101` |
| `RESOURCE_UNREF` | `0x0102` |
| `SET_SCANOUT` | `0x0103` |
| `RESOURCE_FLUSH` | `0x0104` |
| `TRANSFER_TO_HOST_2D` | `0x0105` |
| `RESOURCE_ATTACH_BACKING` | `0x0106` |
| `UPDATE_CURSOR` | `0x0300` |
| `MOVE_CURSOR` | `0x0301` |

Pixel format: XRGB8888 maps to `B8G8R8X8_UNORM` (virtio names are byte order;
using `X8R8G8B8_UNORM` loses the blue channel).

### Scanout negotiation

At init, the backend queries display info, enumerates up to
`VGPU_MAX_PMODES` (16) modes, and selects `pmodes[0]` as the active scanout. If
the boot framebuffer is larger than the host mode and within `GHAL_MAX_DIM`, it
probes that resolution (create resource → `SET_SCANOUT` → destroy) and accepts
it only on device proof.

VirtIO-GPU v1 has no guest mode-set, so `mode_set` returns `-1` and
`GHAL_CAP_MODE_SET` is not advertised.

### Async present and fences

`virtio_gpu_dev_submit2` submits two separate descriptor chains with a single
notification and no wait (the spec allows one command per chain). Both chains
carry `VIRTIO_GPU_FLAG_FENCE` with the same `fence_id`.

| Constant | Value |
| --- | --- |
| `VGPU_FENCE_MAX_HEADS` | 64 |
| `VGPU_ASYNC_PAIRS` | 8 |
| `VGPU_RESP_SYNC_MAX` | 512 |
| `VGPU_RESP_SLOT` | 64 |
| `VGPU_RESP_N_ASYNC` | 56 |

The compositor polls the previous frame's fence before uploading the next
(backpressure); it never waits inside the timer IRQ.

### Cursor

The VirtIO cursor is 64×64 ARGB8888. `virtio_cursor_update` transfers the image
and issues `UPDATE_CURSOR`; `virtio_cursor_move` clamps to `>= 0` and issues
`MOVE_CURSOR`. After `VGPU_CURSOR_MAX_TIMEOUTS` (3) consecutive timeouts the
cursor is marked dead and the compositor falls back to software.

VirtIO-GPU deliberately does **not** advertise `GHAL_CAP_HW_CURSOR`: the host
composited cursor plane is not visible on non-X11 frontends.

## Intel Integrated GPU Backend

`graphics/backend/intel/` drives Intel integrated GPUs via the BCS (blitter)
engine. It targets Gen12 (Alder Lake) in particular.

### Generations

| Generation | Device ID ranges |
| --- | --- |
| Gen12 | `0x4680–0x469F`, `0x9A40–0x9AFF`, `0x4C80–0x4C9F` |
| Gen11 | `0x8A50–0x8A5F` |
| Gen9 | `0x1900–0x19FF`, `0x3E90–0x3EFF` |
| Gen8 | `0x1600–0x16FF` |
| Gen7 | `0x0400–0x04FF` |

Reference target: `VEN_8086 DEV_468B` (Alder Lake-S UHD GT1, Gen12), single
copy engine BCS0.

### Key registers

| Register | Offset |
| --- | --- |
| `INTEL_MMIO_DEVID` | `0x00013818` |
| `INTEL_BCS_OFFSET` | `0x12000` |
| `RING_BASE` | `0x12040` |
| `RING_TAIL` | `0x12060` |
| `RING_HEAD` | `0x12064` |
| `RING_CTL` | `0x12068` |
| `GTT_MEM_CTRL` | `0x13800C` |
| `GTT_BASE_LOW` | `0x138010` |
| `GTT_INV` | `0x13801C` |

### GTT

| Constant | Value |
| --- | --- |
| `INTEL_GTT_MAX_ENTRIES` | 512 (2 MB) |
| `INTEL_GPU_PAGE_SIZE` | 4096 |
| `INTEL_BUF_MAX_PAGES` | 64 (256 KB max buffer) |
| `INTEL_MAX_BUFFERS` | 32 |

The GTT reservation map:

| Offset | Use |
| --- | --- |
| `0x00000` | Legacy BCS ring (32 KB) |
| `0x10000` | Legacy status |
| `0x20000` | Gen12 context (16 KB) |
| `0x24000` | Gen12 status |
| `0x25000` | Gen12 ring (16 KB) |
| `0x100000+` | Buffer bump area |

The framebuffer is never GTT-mapped; scanout is a CPU zero-copy wrap.

### Gen12 path

- **VM**: GGTT-only minimum, with a PPGTT alias (3-level `CTX_PDP0 → PD[0] →
  PT[0..511]`) re-synced from live GGTT PTEs before every submit.
- **Context**: a 4-page logical ring context at `0x20000`.
- **Batch**: `GEN12_BATCH_DWORDS` (1024) in one GGTT-mapped page; copy is 10
  DW, fill is 7 DW.
- **Fence**: a sequence number written by a trailing `MI_STORE_DWORD_IMM` to
  the status page; monotonic from 1; bounded poll.
- **Submit**: a single owner path (`gen12_submit_commit`) under one IRQ-save
  lock; forcewake GT → `mfence` → ELSP.

`gen12_is_live()` gates HAL dispatch: it is set only after a boot self-test
proves ELSP + context + fence, and cleared on the first hardware failure
(fail-fast).

### What the Intel backend does not do

- No 3D/GL/Vulkan/shaders.
- No mode-setting (`mode_set = NULL`; boot-fixed).
- No hardware cursor.
- No framebuffer GTT mapping (scanout is a CPU zero-copy wrap).
- No scaling (1:1 only).
- No GPU reset (the legacy ring exposes none).

### Hardware verification status

The Gen12 path targets physical Alder Lake hardware that is not present in
QEMU. All hardware PASS verdicts are pending a physical boot. The legacy
Gen8–11 path is frozen: its register offsets and batch builders diverge from
`i915` and were likely never driven on real hardware; it fails closed via a
CTL-reject.

## Adding a New Backend

1. Define a `ghal_surface_t`-derived concrete struct and the backend's
   `ghal_backend_ops_t` vtable.
2. Implement at minimum `init`, `surface_create`, `surface_destroy`,
   `surface_upload`, `fill_rect`, `blit`, and `present` (the registration
   validates these).
3. Register with `ghal_register_backend` (up to `GHAL_MAX_BACKENDS` = 8
   backends).
4. If the backend needs framebuffer geometry, handle `ghal_set_framebuffer`.
5. Advertise capabilities honestly; only set `GHAL_CAP_HW_CURSOR` if the
   cursor is actually visible, and `GHAL_CAP_ASYNC_PRESENT` only if
   `present_fence` is implemented.

The vtable is append-only: add new operations at the end and NULL-check them in
dispatch. Never reorder fields or leak backend-specific types into `ghal.h`.

## Related Documentation

- [Graphics Overview](README.md)
- [Compositor](compositor.md)
- [Display & Primitives](display.md)
- [Reference: Constants](../reference/constants.md)
