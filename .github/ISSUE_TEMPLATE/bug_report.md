---
name: Bug report
about: Something in KyuzenOS does not work as documented
title: 'bug: '
labels: bug
---

<!--
Before filing: check that the bug still reproduces on the current default
branch (feature/64bit-migration) with a clean build.

Security vulnerabilities do NOT belong here — see SECURITY.md.
-->

## What happened

<!-- The observed behavior. Paste the actual output or error, do not summarize it. -->

## What was expected

<!-- What should have happened instead, and where that is documented. -->

## Steps to reproduce

1.
2.
3.

## Environment

| | |
| --- | --- |
| Commit | <!-- git rev-parse --short HEAD --> |
| Host OS | <!-- e.g. Windows 11 + MSYS2, Ubuntu 24.04 --> |
| Clang version | <!-- clang --version, first line --> |
| Where it reproduces | <!-- QEMU (./build.sh run), bare metal, or host tests only --> |

## Evidence

<!--
For a crash, include the BSOD details: exception vector, registers, CR2.
The relevant serial.log excerpt is usually the single most useful thing you
can attach. See docs/development/debugging.md for how to capture it.
-->

```text

```

## Additional context

<!--
Anything that narrows it down: does it reproduce every boot or intermittently?
Does it depend on SMP (./build.sh run uses 8 CPUs)? Did it work at an earlier
commit — if so, which?
-->
