# Syscalls

Userspace applications request kernel services through the `int 0x80` software
interrupt. This document describes the calling convention, the dispatch layer,
and the boundary-copy mechanism that validates every userspace pointer.

For the complete list of syscall numbers, see the
[Syscall Table](../reference/syscalls.md).

## Calling Convention

KyuzenOS does **not** use the Linux syscall ABI. Registers are assigned as
follows:

| Register | Role |
| --- | --- |
| `RAX` | Syscall number (input); return value (output) |
| `RBX` | Argument 1 |
| `RCX` | Argument 2 |
| `RDX` | Argument 3 |
| `RSI` | Argument 4 |
| `RDI` | Argument 5 |

All other general-purpose registers are preserved across the call (the entry
stub saves and restores the full register set).

The IDT gate for vector 128 is an **interrupt gate** with DPL 3 (`0xEE`), so
userspace may invoke it directly. Because it is an interrupt gate, interrupts
are disabled on entry; the `yield` syscall performs `sti; hlt` in kernel mode
because `hlt` is privileged.

## Trap Frame

The syscall stub saves a full `registers_t` frame whose layout matches
`PUSHA64` in `arch/x86/isr_macro.inc`:

```text
r15, r14, r13, r12, r11, r10, r9, r8,
rdi, rsi, rbp, rdx, rcx, rbx, rax,
int_num, error_code,
rip, cs, rflags, rsp, ss
```

`RAX` sits at `[RSP + 112]` in this layout. The dispatcher reads the syscall
number from `r->rax` and writes the return value back to `r->rax`.

> **Correctness note:** an earlier stub overwrote the frame's `RAX` slot with a
> leftover register value. That was removed; `r->rax` is now the single source
> of the return value.

## Dispatch

The dispatcher (`kernel/syscall/syscall.c`) is intentionally thin:

1. Record diagnostic context (`g_last_syscall_*`) for the panic handler.
2. Build a boundary-copy context from the trap frame.
3. **Kill pre-check** — if the caller has `kill_pending`, exit immediately.
4. Route to the subsystem handler by syscall number. Each handler returns `1`
   if it has already set `r->rax` and wants an early return.
5. **Kill post-check** — a task flagged while blocked inside the syscall must
   not return to user code.
6. Store the return value in `r->rax`.

Routing is grouped by subsystem:

| Handler | Syscalls | File |
| --- | --- | --- |
| `sys_proc_handle` | 25, 33, 34, 57, 68–73, 77, 78, 79 | `sys_proc.c` |
| `sys_mem_handle` | 9, 10, 19 | `sys_mem.c` |
| `sys_fs_handle` | 5–8, 11–13, 18, 24, 64, 82, 83 | `sys_fs.c` |
| `sys_vfs_handle` | 47–51, 74–76, 81 | `sys_vfs.c` |
| `sys_kwm_handle` | 22, 23, 26, 29–32, 58–63, 66, 67, 84, 85 | `sys_kwm.c` |
| `sys_net_handle` | 41, 52–56, 86 | `sys_net.c` |
| `sys_gpu_handle` | 65 | `sys_gpu.c` |
| `sys_system_handle` | 4, 14–17, 20, 35–39, 46, 87 | `sys_system.c` |
| `sys_misc_handle` | 1–3, 27, 28, 42–45, 80 | `sys_misc.c` |

Syscall 21 is reserved and unused.

## Boundary Copy

Every pointer passed from Ring 3 is validated before use. The boundary-copy
layer lives in `kernel/proc/usercopy.c` with constants in
`include/usercopy.h`.

### Context

The context is created per syscall invocation and lives on the syscall stack —
never in a global or per-CPU variable, because a syscall may block
(`sti; hlt`) and a nested syscall can run on the same CPU.

```c
typedef struct {
    int          from_user;   // (r->cs & 3) == 3
    phys_addr_t  pml4;        // caller's address space
} ucopy_ctx_t;
```

### Validation

`user_range_ok(ctx, uaddr, len)` rejects:

- `uaddr == 0`
- `len > UC_MAX_RANGE`
- wraparound (`uaddr + len < uaddr`)
- for Ring 3: `uaddr >= UC_USER_VA_MAX`, or `len > UC_USER_VA_MAX - uaddr`

For Ring-3 callers it then walks the range **page by page** (4 KB), confirming
each page is mapped into the caller's address space. For Ring-0 callers
(kernel-context shells calling via `int 0x80`) it returns immediately — kernel
pointers are legal.

`copy_from_user` / `copy_to_user` validate, then `memcpy` inside a
`user_access_begin()`/`user_access_end()` window (STAC/CLAC) for Ring-3
callers. `strncpy_from_user` validates only the pages actually touched, so a
short string ending in a mapped page succeeds.

### Why dereference-after-validation is safe

During a syscall, CR3 is the caller's PML4 (`int 0x80` does not change CR3),
and only the calling task can unmap its own address space — and it is inside
the syscall. So a validated range cannot be unmapped out from under the copy.

### Contract per syscall class

| Contract | Meaning | Examples |
| --- | --- | --- |
| **copy-in** | Buffer/string copied to kernel before use | `print` (1), `create_file` (18), `ping` (41) |
| **copy-out** | Result written to a kernel buffer, then copied out after the lock is released | `read` (48), `get_file_list` (24), `get_event` (29) |
| **shared** | Range validated, then used directly (documented exception) | `draw_image` (23), `kwm_update_window` (31) |
| **bypass** | No pointer, or kernel pointer (Ring 0) | `alloc` (9), `yield` (4) |

The two **shared** cases are deliberate: `draw_image` reads the caller's pixels
in place, and `kwm_update_window` copies up to 16 MB per frame directly into
the window canvas. Both validate the full range first.

## Limits (`include/usercopy.h`)

| Constant | Value | Applies to |
| --- | --- | --- |
| `UC_USER_VA_MAX` | `0x0000800000000000` | User VA ceiling (PML4 index < 256) |
| `UC_MAX_RANGE` | 64 MiB | Absolute range ceiling |
| `UC_MAX_STR` | 1024 | `print` (1), `draw_string` (26) |
| `UC_MAX_FNAME` | 64 | All filesystem/vfs names |
| `UC_MAX_HOST` | 128 | `ping` (41), `resolve` (86) |
| `UC_MAX_KBD` | 512 | `read_keyboard` (3) |
| `UC_MAX_FILE` | 8 MiB | File body (13, 18) |
| `UC_MAX_IO` | 1 MiB | `read`/`write` (48, 49) per call |
| `UC_MAX_SOCK` | 64 KiB | Socket send/recv (54, 55) per call |
| `UC_MAX_ENTRIES` | 128 | `get_file_list` (24) |

## SMAP / SMEP

When the CPU supports them, SMAP and SMEP are enabled (`cpu_enable_smap_smep`),
and write-protect is enforced (`CR0.WP`). User memory is then only accessible
inside `stac`/`clac` windows:

- `usercopy.c` — `copy_from_user`/`copy_to_user`, `strncpy_from_user`
- `uheap.c` — realloc copy
- `elf.c` — segment copy and BSS zeroing
- `gfx/kwm.c` — window canvas blit (`rep movsl`)
- `syscall.c` — `draw_image` call site

The `stac`/`clac` instructions are gated on a runtime flag, so CPUs without
SMAP never execute them (avoiding `#UD`).

> **Known limitation:** ISR entry does not execute `clac`, so an interrupt
> arriving inside a user-access window inherits `AC=1` until `iretq`. This is a
> temporary reduction of protection, not a correctness bug (RFLAGS is
> per-task). Closing it would require a `clac` in every entry stub.

## Userspace Wrappers

Applications do not issue `int 0x80` directly. They call wrapper functions in
`libs/core/userlib.c` (and the kernel-context shim
`libs/core/kernel_userlib.c`). The C SDK's CRT and the C++ SDK's runtime build
on these wrappers.

## Related Documentation

- [Syscall Table](../reference/syscalls.md) — full number/argument reference
- [Userspace Model](../userspace/overview.md) — Ring-3 isolation
- [Memory Management](memory.md) — user heap and address spaces
- [Processes](processes.md) — process-related syscalls
