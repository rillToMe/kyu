# Security Policy

KyuzenOS is an operating system kernel with a Ring-3 userspace boundary, a
network stack, and a web browser. A bug in the areas below is a security bug,
not just a crash.

## Reporting a Vulnerability

**Do not report security vulnerabilities in public GitHub issues, pull
requests, or discussions.** A public report tells everyone about the flaw before
there is a fix, including anyone who would exploit it.

Report privately using GitHub's private vulnerability reporting:

**<https://github.com/rillToMe/kyu/security/advisories/new>**

This opens a private advisory that only the maintainer can see, and gives us a
place to discuss the problem, develop a fix, and publish an advisory once a fix
is available.

> **If that link reports that private reporting is unavailable**, the repository
> owner has not yet enabled the feature. In that case, open a GitHub issue that
> contains **no technical detail** — just ask for a private channel and wait for
> a reply before describing anything. Do not include reproduction steps,
> addresses, logs, or the affected component.

Please include, as far as you can:

- What the issue is, and which component is affected.
- The commit you tested against.
- Steps to reproduce, or a proof of concept.
- The impact — what an attacker gains, and from what starting point.
- Any suggested fix, if you have one.

You do not need a complete exploit. A clear description of a reachable flaw is
useful on its own.

## What Counts as a Security Issue

This project's trust boundaries are the kernel/userspace split and the parsing
of untrusted input. Examples:

- **Privilege escalation from Ring 3** — reaching Ring 0, or escaping a
  process's address space.
- **Syscall boundary validation bypass** — defeating the `int 0x80` boundary
  copy layer, so a kernel pointer or out-of-range buffer is accepted.
- **Protection-mechanism bypass** — SMAP/SMEP, write-protect, or NX being
  circumventable, or a per-process address space that is not actually isolated.
- **Kernel memory corruption reachable from userspace** — heap or stack
  corruption, use-after-free, or an out-of-bounds access triggered by a
  userspace request.
- **Authentication bypass** — defeating the login path or reading `users.sys`
  credentials without authorization.
- **Parser vulnerabilities in untrusted input** — the KyuzenFS on-disk parser,
  the ELF loader, the HTML/CSS engine, the PNG/BMP decoders, the TLS stack, or
  the network stack processing attacker-controlled packets.
- **Denial of service that the kernel cannot recover from** — a panic or
  deadlock reachable from unprivileged userspace or from the network.

Bugs that merely crash the system with no attacker-controlled path are ordinary
bugs. Report those with the
[bug report template](.github/ISSUE_TEMPLATE/bug_report.md).

## Scope

This policy covers code in this repository.

**Vendored dependencies** in `third_party/` — lwIP, LLVM libc/libc++, FreeType,
BearSSL, and Lexbor — are third-party projects. If a flaw exists in the upstream
code and is not specific to how KyuzenOS integrates it, report it to the
upstream project as well. We would still like to know, because we ship it and
the integration may change the impact. Where the flaw is in the KyuzenOS
integration or port layer, it is ours.

**Supported versions.** KyuzenOS is under active development and has no
maintained release branches. Only the current default branch,
`feature/64bit-migration`, is supported. Fixes land there. There is no security
backporting to tags.

**No bug bounty.** This is a community project and there is no payment for
reports. Credit in the published advisory is offered unless you prefer
otherwise.

## What to Expect

This project is maintained by one person, so responses are best-effort rather
than on a contractual timeline. We will:

- Acknowledge your report as soon as we can.
- Confirm whether we consider it a vulnerability, and tell you if we disagree
  and why.
- Keep you informed as we work on a fix.
- Credit you in the advisory when it is published, unless you ask us not to.

Please give us a reasonable opportunity to fix the issue before publishing it
yourself.

## Safe Harbor

We will not pursue or support legal action against anyone who reports a
vulnerability in good faith under this policy, who avoids privacy violations,
data destruction, and service disruption while testing, and who gives us
reasonable time to respond before public disclosure.

Testing must stay within your own environment — a QEMU instance or hardware you
control. Do not test against systems you do not own or have permission to test.
