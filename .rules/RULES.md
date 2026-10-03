# KyuzenOS Development Rules

Version: 1.0

This document defines the mandatory development rules for every contributor and AI coding agent working on KyuzenOS.

Failure to follow these rules is considered a bug.

This is the entry-point file. Detailed rules live in the other files under `.rules/`:

- `STYLE_GUIDE.md` — clean code, comments, naming
- `ARCHITECTURE.md` — architecture, kernel APIs, drivers, error handling, memory safety, synchronization, logging, performance
- `UI.md` — design system, theme tokens, widget states, XML, rendering correctness, UI performance
- `BUILD.md` — build verification & testing
- `DOCUMENTATION.md` — documentation requirements
- `../RULES.md` — commit, branch, and pull request rules (repository root)
- `REVIEW_CHECKLIST.md` — checklists to run before declaring a task done

---

# 1. General Principles

- Never prioritize speed over code quality.
- Always prefer readability over clever code.
- Every change must improve or maintain the architecture.
- Never introduce technical debt intentionally.
- Every implementation must be production quality.

---

# 2. AI Agent Rules

AI agents MUST:

- Read existing code before writing new code.
- Match the existing coding style.
- Reuse existing utilities.
- Avoid rewriting working systems.
- Preserve backward compatibility.
- Keep changes minimal.
- Avoid unnecessary dependencies.
- Avoid introducing new frameworks.
- Never fabricate APIs.
- Never invent hardware behavior.

If uncertain, inspect the implementation before making changes.

---

# 3. Golden Rule

When making changes:

Leave the codebase better than you found it.

Every commit should improve KyuzenOS.