# Shell & CLI

KyuzenOS has one shell engine (`system/shell_core.c`) shared by the console
shell and the GUI terminal. Both are **ring-3 ELFs**: `shell.elf` (console,
spawned by `login.elf`) and `apps/terminal.c` (GUI, spawned by the user).
Each is a front end that delegates every command to the same engine — commands
are registered once and appear in every terminal.

The console shell exits on `logout`; `login.elf` waits for it (`waitpid`)
and then shows the login screen again.

## Builtin Commands

| Command | Purpose |
| --- | --- |
| `help` | List available commands |
| `clear` | Clear the screen |
| `echo` | Print text |
| `ls` | List files |
| `tree` | Recursive directory tree |
| `baca` / `cat` | Read a file |
| `hapus` / `rm` | Delete a file |
| `mkdir` | Create a folder |
| `fetch` | System information |
| `neofetch` | System information plus art (`neofect.json`) |
| `sched` | CPU / scheduler status |
| `time` / `date` | Current time |
| `start` | Run an application concurrently |
| `ping` | Ping a host |
| `install_app` | Install an application |
| `nettest` | TCP socket test |
| `whoami` | Current user |
| `id` | UID and username |
| `users` | List accounts |
| `df` | Disk usage |
| `uname` | System information |
| `wait` | Delay N milliseconds |
| `jobs` | List child processes |
| `kill` | Stop a process (kernel-authorized) |
| `reap` | Wait for and reap a child (blocking) |
| `logout` | End the session |
| `sudo` | Run a command as root |
| `adduser` | Add a user (root) |
| `format` | Format the disk (root) |
| `shutdown` | Power off (root) |
| `restart` / `reboot` | Restart (root) |

Commands marked "(root)" require root privileges; the kernel enforces the
root-only operations (`fs_format`, `shutdown`, `reboot`), and `sudo` is a UX
gate on top.

## Running Applications

- Typing an application name execs it **in place** (replacing the shell
  process's image).
- `start <app>` spawns the application **concurrently** as a new Ring-3 task;
  the shell keeps running.
- `start <app> arg1 arg2` passes arguments (`argc = 3`).

## Redirection and Pipelines

The shell supports redirection and pipelines for **external** applications
only. Operators must be separate tokens (`a | b`, `cmd > f`); `a>b` is a single
word.

| Syntax | Meaning |
| --- | --- |
| `cmd > f` | Redirect stdout to `f` (`O_WRONLY\|O_CREAT\|O_TRUNC`) |
| `cmd >> f` | Append stdout to `f` (`O_APPEND`) |
| `cmd < f` | Redirect stdin from `f` (`O_RDONLY`) |
| `a \| b \| c` | Pipeline (up to 4 stages) |

Rules:

- `<` is only valid in the first stage; `>`/`>>` only in the last stage.
- stderr always stays on the console.
- External application names resolve to `/apps/*.elf`; an external application
  takes precedence over a same-named builtin (for example, `echo` → `echo.elf`).
- Builtin-only names and `start` inside a stage are rejected with an error.

The GUI terminal implements pipelines with `fork` → `dup2` → `execve`. The
console shell (`shell.elf`, also ring-3 since the init migration) uses the
`spawn_redir` path with identical wiring.

Examples:

```text
echo hello | cat
echo hello > f1.txt
cat < f1.txt
cat f1.txt | cat
```

## Job Control

- `start` is asynchronous.
- `jobs` lists own children.
- `reap <pid>` blocks on one child and prints its status (`(killed)` for the
  kill exit code).
- `kill <pid>` sends a kill (kernel-authorized).

There is no background `&` operator, no job numbering, and no `SIGINT`/signal
model. Ctrl-C is handled as a foreground interrupt: it maps onto the existing
kill chain and is documented in [Process Model](../kernel/processes.md).

## GUI Terminal

`apps/terminal.c` is a front end only: it owns a KWM window and a `TextEdit`
transcript and delegates commands to the shared engine. Keyboard input reaches
it through the KWM focus path; because the focused window owns the keyboard,
typed input goes to the terminal, not the console shell.

## Related Documentation

- [Userspace Model](overview.md)
- [Applications](applications.md)
- [Process Model](../kernel/processes.md) — spawn, exec, fork, kill
- [VFS & File Descriptors](../filesystem/vfs.md) — redirection uses this layer
