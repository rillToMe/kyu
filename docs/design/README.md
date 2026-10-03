# Design Notes

This directory holds design records: rationale, decisions, audits, and
proposals that explain **why** parts of KyuzenOS are built the way they are.
They complement the subsystem documentation, which describes **what exists**
and **how it works**.

For the current architecture, always prefer the subsystem documents linked
from the [documentation index](../README.md). Design notes are supporting
material; where a design note and the implementation disagree, the
implementation wins.

## Kernel & Runtime

| Document | Description |
| --- | --- |
| [Scheduler Module Split](sched-module-split.md) | Why `kernel/sched/` is split into core/runqueue/lifecycle/block/debug |
| [ATA Driver Redesign Proposal](ata-driver-redesign-proposal.md) | Proposed redesign of the ATA PIO driver |
| [Hot Reload API](hot-reload-api.md) | The generic hot-reload syscall design (84/85) |
| [Ring-3 Init Migration](ring3-init-migration.md) | Moving login/shell/zen out of the kernel image into ring-3 ELFs, with `init.elf` as supervisor (FIX.md K-1) |

## Libraries & SDKs

| Document | Description |
| --- | --- |
| [C SDK Design](kyuzen-c-sdk.md) | The C SDK boundary, libc integration, and port layer |
| [LLVM libc 22 Freestanding Audit](audit-llvm-libc-22-freestanding.md) | Current audit of the LLVM libc 22 freestanding port |
| [LLVM libc Freestanding Audit (earlier)](audit-llvm-libc-freestanding.md) | Earlier audit; superseded by the 22.1.8 audit above |
| [Color Library](color-library.md) | `color_t`, blending, HSL/HSV, and the `COLOR_HEX` constant |
| [Font Rendering](font-rendering.md) | FreeType-backed text rendering and the bitmap/FreeType split |
| [PNG Decode Profile](png-decode-profile.md) | The trimmed stb_image profile (PNG/BMP only) |

## GUI

| Document | Description |
| --- | --- |
| [UI Rules](../../.rules/UI.md) | **Binding.** Design-system rules: tokens, states, XML, rendering correctness, UI performance |
| [Design System](gui/libui-design-system.md) | How to build a page with libui: tokens, states, icons, XML, recipes |
| [libui Redesign Report](gui/libui-redesign-report.md) | Audit of the previous UI architecture and what replaced it |
| [Widget Split](widget-split.md) | The layered structure of the widget toolkit |
| [UI Theme System](gui/ui-theme-system.md) | Theme representation and application |
| [Color API Audit](gui/color-api-audit.md) | Audit of the color API surface |
| [Color Hex Syntax Feasibility](gui/color-hex-syntax-feasibility.md) | Why `COLOR_HEX` uses the adopted syntax |
| [Color Widget Audit](gui/ui-color-widget-audit.md) | Widget color usage audit |
| [Task Manager (C++)](taskmgr-cpp.md) | The Task Manager application design |

## Graphics & GPU

| Document | Description |
| --- | --- |
| [GPU Architecture](gpu/intel-integrated/GPU_ARCHITECTURE.md) | Intel integrated GPU driver architecture |
| [GPU Driver](gpu/intel-integrated/GPU_DRIVER.md) | Intel driver internals |
| [GPU Commands](gpu/intel-integrated/GPU_COMMANDS.md) | Batch-buffer command encoding |
| [GPU Memory](gpu/intel-integrated/GPU_MEMORY.md) | GTT and buffer management |
| [GPU Acceleration Benchmark](gpu/intel-integrated/GPU_ACCELERATION_BENCHMARK.md) | Performance measurements |

> The `docs/history/graphics/` directory contains superseded graphics design
> documents (the pre-GHAL `gpu.h` API) and the per-phase performance reports.

## Networking

| Document | Description |
| --- | --- |
| [Network ABI](network/NETWORK_ABI.md) | The socket syscall ABI |
| [Network Audit](network/NETWORK_AUDIT.md) | lwIP integration and locking audit |
| [Socket Lifecycle](network/NETWORK_SOCKET_LIFECYCLE.md) | Socket ownership, handles, and teardown |

## Browser

| Document | Description |
| --- | --- |
| [Browser Design](browser/browser.md) | Engine architecture |
| [Browser Roadmap](browser/browser-roadmap.md) | Planned capabilities |
| [Lexbor Stage A](browser/lexbor-stage-a.md) | Vendored Lexbor 3.0.0 HTML/DOM: freestanding archive, port layer, host + QEMU verification |
| [Lexbor Stage B](browser/lexbor-stage-b.md) | Lexbor 3.0.0 as the production HTML parser behind `html::parse()`, via a dedicated DOM adapter |

## Related Documentation

- [Documentation Index](../README.md)
- [Development History](../history/README.md)
