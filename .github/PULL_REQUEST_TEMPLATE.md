<!--
Thanks for contributing to KyuzenOS. Keep this short — the checklist is a
prompt, not paperwork. See CONTRIBUTING.md for the full workflow.
-->

## Summary

<!-- What does this change do, in one or two sentences? -->

## Motivation

<!-- Why is this needed? What problem does it solve? Link the issue if there is one. -->

## Changes

<!--
The notable changes, not a file listing. If this touches a public interface
(syscall numbers, the KyuzenFS on-disk format, an application manifest, a
library API), call that out explicitly.
-->

## Testing

<!--
What did you actually run, and what was the result? "Builds and passes tests"
is not enough — name the test and what you observed.

Include any of the following that apply:
  - ./build.sh              (kernel + libs + apps)
  - ./build.sh test         (host suite; 17 of 20 pass, 3 are disabled)
  - ./build.sh run          (booted in QEMU)
  - in-OS tests, e.g. `start fork_test`
-->

## Related issue

<!-- Closes #... / Relates to #... — or "None". -->

## Checklist

- [ ] `./build.sh` succeeds with no new warnings
- [ ] `./build.sh test` passes
- [ ] Booted in QEMU if kernel or userspace behavior changed (`./build.sh run`)
- [ ] Documentation updated for any behavior or interface change
- [ ] No new SSE/x87 instructions introduced, and no new dependency without prior discussion
- [ ] Commits follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) per [RULES.md](RULES.md)
- [ ] No unrelated reformatting or drive-by changes
