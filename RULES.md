# KyuzenOS Rules

Short, binding rules for commits, branches, and pull requests.

These cover *how you submit* a change. The rules for *how you write* it - style,
architecture, error handling, documentation, and the pre-completion checklist -
live in [`.rules/`](.rules/RULES.md). Both apply.

## Commits

[Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/), one
logical change per commit.

```text
<type>(<scope>): <description>
```

### Types

| Type | Use for |
| --- | --- |
| `feat` | A new capability |
| `fix` | A bug fix |
| `docs` | Documentation only |
| `refactor` | Restructuring that preserves behavior |
| `perf` | Performance work |
| `test` | Tests only |
| `build` | Build system, toolchain, dependencies |
| `ci` | CI configuration |
| `style` | Formatting only, no behavior change |
| `chore` | Maintenance that fits nowhere else |
| `revert` | Reverting an earlier commit |

### Scope

Optional. Lowercase, and names the subsystem you touched - usually a directory
or module name. Scopes already in use in this repository: `build`, `fs`, `gfx`,
`gitignore`, `login`, `mkfs`, `mm`, `mouse`, `qemu`, `ring3`, `roadmap`.

```text
feat(build): host tests via CTest
fix(mm): stop the VMM racing on the user PML4
docs(sched): correct the work-stealing description
```

### Quality

- Imperative mood, lowercase, no trailing period: `fix(mm): free the page`.
- Subject line at most 72 characters.
- Say what changed and why. Do not narrate the process.
- One logical change per commit. Do not bundle unrelated work.
- Every commit must build. Never commit a broken intermediate state.

## Branches

```text
<type>/<short-description>
```

Use `feature/`, `fix/`, `docs/`, or `refactor/`, followed by a lowercase
hyphenated description - for example `feature/64bit-migration`.

`feature/64bit-migration` is this repository's default branch and the active
line of development. Open pull requests against it. `main` is an older line and
is not where new work lands.

## Pull Requests

- One logical change per pull request. Keep it reviewable.
- Target the default branch, `feature/64bit-migration`.
- The build must pass: `./build.sh`.
- The test suite must pass: `./build.sh test`.
- Update the documentation your change affects - see
  [`.rules/DOCUMENTATION.md`](.rules/DOCUMENTATION.md).
- Fill in the pull request template and state what you verified.
- Do not include unrelated reformatting or drive-by cleanup.
