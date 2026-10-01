# ============================================================================
# KyuzenSources.cmake — explicit kernel source list.
#
# WHY EXPLICIT (and not file(GLOB_RECURSE ...)):
#   The old Makefile used `wildcard` over an explicit SRC_DIRS list, then
#   filtered 29 files back out — because `libs/core/`, `system/`, and
#   `tests/host/unit/` are *directories* that hold non-kernel code, and a
#   recursive glob from the root would also sweep in third_party/, legacy/,
#   rust/target, and build output. The 29-entry filter-out list is the
#   accumulated evidence that implicit discovery misfires here.
#
#   An explicit list means: adding a kernel source is a one-line edit in one
#   file, and "what is in the kernel?" is answerable by reading this file.
#
# This list is the EXACT resolved output of the old Makefile's
#   $(filter-out ...,$(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c)))
# verified by dumping the variable from Make itself:
#   111 C sources + 12 ASM sources.
# Source order is preserved: the kernel links in this order, and the old
# Makefile's object order must be reproduced for binary parity
# (BUILD_SYSTEM_AUDIT.md §16.2, risk R1).
# ============================================================================

include_guard(GLOBAL)

set(KYUZEN_KERNEL_C_SOURCES
    # --- arch/x86 -----------------------------------------------------------
    arch/x86/gdt.c
    arch/x86/idt.c
    arch/x86/lapic.c

    # --- drivers ------------------------------------------------------------
    drivers/acpi.c
    drivers/ata.c
    drivers/keyboard.c
    drivers/mouse.c
    drivers/pci.c
    drivers/rtc.c
    drivers/serial.c
    drivers/timer.c
    drivers/tty.c

    # --- kernel core --------------------------------------------------------
    kernel/boot_console.c
    kernel/cpu.c
    kernel/display.c
    kernel/entropy.c
    kernel/kernel.c
    kernel/kprint.c
    kernel/string.c
    kernel/timer_callbacks.c

    # --- syscall layer ------------------------------------------------------
    kernel/syscall/sys_fs.c
    kernel/syscall/sys_gpu.c
    kernel/syscall/sys_kwm.c
    kernel/syscall/sys_mem.c
    kernel/syscall/sys_misc.c
    kernel/syscall/sys_net.c
    kernel/syscall/sys_proc.c
    kernel/syscall/sys_system.c
    kernel/syscall/sys_vfs.c
    kernel/syscall/syscall.c

    # --- smp ----------------------------------------------------------------
    kernel/smp/smp.c

    # --- compositor / KWM ---------------------------------------------------
    kernel/gfx/compositor.c
    kernel/gfx/fb.c
    kernel/gfx/kwm.c

    # --- scheduler ----------------------------------------------------------
    kernel/sched/block.c
    kernel/sched/core.c
    kernel/sched/debug.c
    kernel/sched/lifecycle.c
    kernel/sched/runqueue.c

    # --- filesystem (KyuzenFS V4) ------------------------------------------
    kernel/fs/bcache.c
    kernel/fs/kfs_balloc.c
    kernel/fs/kfs_dir.c
    kernel/fs/kfs_extent.c
    kernel/fs/kfs_inode.c
    kernel/fs/kfs_shim.c
    kernel/fs/kfs_super.c
    kernel/fs/kfs_vnode.c
    kernel/fs/vfs.c
    kernel/fs/vfs_fd.c

    # --- network (kernel side) ---------------------------------------------
    # These four include lwIP headers and therefore need kyuzen-flags-lwip.
    kernel/net/net_dns.c
    kernel/net/net_init.c
    kernel/net/net_ping.c
    kernel/net/net_socket.c

    # --- memory management --------------------------------------------------
    kernel/mm/heap.c
    kernel/mm/heap_watch.c
    kernel/mm/paging.c
    kernel/mm/pmm.c
    kernel/mm/uheap.c

    # --- crash / panic logging ---------------------------------------------
    kernel/debug/crash_archive.c
    kernel/debug/crashdump.c
    kernel/debug/panic_log.c

    # --- synchronisation ----------------------------------------------------
    kernel/sync/event.c
    kernel/sync/spinlock.c
    kernel/sync/sync.c
    kernel/sync/wait.c

    # --- process model ------------------------------------------------------
    kernel/proc/elf.c
    kernel/proc/proc.c
    kernel/proc/usercopy.c

    # --- panic (BSOD) -------------------------------------------------------
    kernel/panic/panic.c
    kernel/panic/panic_draw.c
    kernel/panic/panic_explain.c
    kernel/panic/panic_hw.c

    # --- graphics abstraction + backends ------------------------------------
    graphics/ghal.c
    graphics/backend/software.c
    graphics/backend/virtio_gpu.c

    # --- Intel Gen12 backend ------------------------------------------------
    graphics/backend/intel/intel_bcs.c
    graphics/backend/intel/intel_bench.c
    graphics/backend/intel/intel_cmd.c
    graphics/backend/intel/intel_fence.c
    graphics/backend/intel/intel_gen12_batch.c
    graphics/backend/intel/intel_gen12_bench.c
    graphics/backend/intel/intel_gen12_ctx.c
    graphics/backend/intel/intel_gen12_engine.c
    graphics/backend/intel/intel_gen12_fault.c
    graphics/backend/intel/intel_gen12_fence.c
    graphics/backend/intel/intel_gen12_ghal.c
    graphics/backend/intel/intel_gen12_ppgtt.c
    graphics/backend/intel/intel_gen12_robust.c
    graphics/backend/intel/intel_gen12_submit.c
    graphics/backend/intel/intel_gen12_test_blit.c
    graphics/backend/intel/intel_gen12_test_copy.c
    graphics/backend/intel/intel_gen12_test_fill.c
    graphics/backend/intel/intel_gen12_vm.c
    graphics/backend/intel/intel_gpu_alloc.c
    graphics/backend/intel/intel_gtt.c
    graphics/backend/intel/intel_init.c
    graphics/backend/intel/intel_irq.c
    graphics/backend/intel/intel_mmio.c
    graphics/backend/intel/intel_rect.c
    graphics/backend/intel/intel_robust.c
    graphics/backend/intel/intel_surface.c
    graphics/backend/intel/intel_test_blit.c
    graphics/backend/intel/intel_test_copy.c
    graphics/backend/intel/intel_test_fill.c

    # --- GPU memory ---------------------------------------------------------
    graphics/memory/gpu_alloc.c

    # --- virtio-gpu driver + virtqueue --------------------------------------
    drivers/graphics/hw/virtio_gpu_cmd.c
    drivers/graphics/hw/virtio_gpu_dev.c
    drivers/graphics/hw/virtqueue.c

    # --- colour math (kernel copy: kernel flags, not user-space flags) ------
    # libs/gui/color is the ONLY source compiled into BOTH the kernel and
    # user-space. The two copies use different flags (-mcmodel=kernel, -O) and
    # therefore cannot share object files.
    libs/gui/color/src/color_blend.c
    libs/gui/color/src/color_space.c
    libs/gui/color/src/color_utils.c
)

set(KYUZEN_KERNEL_ASM_SOURCES
    arch/x86/exceptions.asm
    arch/x86/gdt_flush.asm
    arch/x86/idle_switch.asm
    arch/x86/idt_flush.asm
    arch/x86/isr128.asm
    arch/x86/isr14.asm
    arch/x86/keyboard_isr.asm
    arch/x86/lapic_timer_isr.asm
    arch/x86/mouse_isr.asm
    arch/x86/reschedule_isr.asm
    arch/x86/smp_ap_entry.asm
    arch/x86/timer_isr.asm
)

# The four kernel sources that include lwIP headers. Kept as a named set so
# kernel/CMakeLists.txt can apply kyuzen-flags-lwip to exactly these files.
set(KYUZEN_KERNEL_LWIP_CONSUMERS
    kernel/net/net_dns.c
    kernel/net/net_init.c
    kernel/net/net_ping.c
    kernel/net/net_socket.c
)
