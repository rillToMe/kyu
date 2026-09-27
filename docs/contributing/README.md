# Contributing

Thank you for your interest in KyuzenOS. This guide explains how to contribute
safely. The repository also contains a `.rules/` directory with the detailed,
mandatory development rules; this document is the human-readable entry point.

## Before You Start

1. Read [Architecture Overview](../architecture/overview.md) to understand the
   system.
2. Read [Building](../development/building.md) and get a clean `make` and
   `make run` working.
3. Read the `.rules/` files — they are binding:
   - `RULES.md` — general principles
   - `STYLE_GUIDE.md` — code style, comments, naming
   - `ARCHITECTURE.md` — architecture, kernel APIs, drivers, error handling,
     memory safety, synchronization
   - `BUILD.md` — build verification and testing
   - `.rules/DOCUMENTATION.md` — documentation requirements
   - `REVIEW_CHECKLIST.md` — the pre-completion checklist

## Core Principles

- **Never prioritize speed over quality.**
- **Prefer readability over clever code.**
- **Reuse existing utilities**; do not duplicate systems.
- **Make minimal, surgical changes**; preserve backward compatibility.
- **Do not introduce technical debt** intentionally.
- **Never fabricate** APIs, hardware behavior, or results.

## Repository Conventions

- **Language**: project code is C, C++, and Rust only. Build orchestration
  tooling (for example, `compiledb`) is the exception.
- **Kernel layout**: kernel code lives under `kernel/` in subsystem
  subdirectories (`mm/`, `sched/`, `proc/`, `sync/`, `fs/`, `net/`, `gfx/`,
  `syscall/`, `panic/`, `debug/`, `smp/`). Architecture code is in `arch/x86/`;
  drivers in `drivers/`.
- **Userspace**: libraries in `libs/`, applications in `apps/`, kernel-context
  programs in `system/`.
- **One authoritative path per subsystem.** Legacy entry points are retained
  deliberately and documented as such; do not add a second implementation.

## Making a Change

1. **Understand before editing.** Read the surrounding code and any related
   documentation.
2. **Match the existing style.** Follow the naming, comment, and structure
   conventions of the file you are editing.
3. **Keep it minimal.** Do not rewrite working systems.
4. **Respect the contracts.** Lock ordering, blocking rules, ownership, and
   buffer limits are documented; violating them is a bug.
5. **Handle errors.** Never ignore a return value.
6. **Test.** Build the whole system (`make`), build apps (`make apps`), and run
   the relevant tests ([Testing](../development/testing.md)).
7. **Document.** Update the relevant documentation under `docs/`.

### Documentation Requirements

Per `.rules/DOCUMENTATION.md`, documentation is mandatory. A change that
affects behavior, an interface, or a subsystem must update the matching
document in `docs/`. If you add a new subsystem, add a document for it and link
it from the [documentation index](../README.md).

## Review Checklist

Before declaring a task complete (from `.rules/REVIEW_CHECKLIST.md`):

- [ ] The project compiles cleanly (`make`, and `make apps` if relevant).
- [ ] No new warnings.
- [ ] Documentation updated where behavior or interfaces changed.
- [ ] No duplicated logic and no dead code introduced.
- [ ] The architecture is preserved (no ad-hoc systems).
- [ ] Logging is appropriate.
- [ ] Memory safety and thread safety are maintained.
- [ ] Formatting matches the surrounding code.

## Reporting Bugs

When reporting a bug, include:

- What you did, what you expected, and what happened.
- The relevant `serial.log` excerpt.
- If it is a crash: the BSOD details (vector, registers, `CR2`) or the crash
  report.
- Reproduction steps.

See [Debugging](../development/debugging.md) for how to capture this
information.

## Current Status

KyuzenOS is under active development. Some subsystems are complete, some are
partial, and some are planned. Documentation labels each accordingly
(`Currently supported`, `Not currently implemented`, `Experimental`,
`Planned`). When in doubt, the code is the source of truth.

## Related Documentation

- [Building](../development/building.md)
- [Testing](../development/testing.md)
- [Debugging](../development/debugging.md)
- [Architecture Overview](../architecture/overview.md)
