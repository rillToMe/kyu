# Syscall Reference

Complete reference of KyuzenOS syscall numbers, arguments, and return values.
For the calling convention and boundary-copy rules, see
[Syscalls](../kernel/syscalls.md).

## Calling Convention

`int 0x80`; `RAX` = syscall number; arguments in `RBX`, `RCX`, `RDX`, `RSI`,
`RDI`; return value in `RAX`. All other registers are preserved.

## Argument Limits

Userspace pointers are validated against the caller's address space and bounded
(see [Syscalls](../kernel/syscalls.md)):

| Constant | Value |
| --- | --- |
| `UC_USER_VA_MAX` | `0x0000800000000000` |
| `UC_MAX_RANGE` | 64 MiB |
| `UC_MAX_STR` | 1024 |
| `UC_MAX_FNAME` | 64 |
| `UC_MAX_HOST` | 128 |
| `UC_MAX_KBD` | 512 |
| `UC_MAX_FILE` | 8 MiB |
| `UC_MAX_IO` | 1 MiB |
| `UC_MAX_SOCK` | 64 KiB |
| `UC_MAX_ENTRIES` | 128 |

## Process and Scheduling

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 4 | `yield` | — | — |
| 25 | `load_elf` | `RBX=name` | 0 / -1 |
| 33 | `exec` | `RBX=name` | never returns / -1 |
| 34 | `exit` | `RBX=code` | never returns |
| 43 | `get_task_id` | — | task id |
| 45 | `get_pid` | — | AS cookie (not the PID) |
| 46 | `sleep` | `RBX=ms` | — |
| 57 | `spawn` | `RBX=path` | child pid / -1 |
| 68 | `spawn_argv` | `RBX=path, RCX=argc, RDX=argv` | child pid / -1 |
| 69 | `waitpid` | `RBX=pid, RCX=status*, RDX=options` | child pid / -1 |
| 70 | `getpid` | — | pid |
| 71 | `getppid` | — | ppid / `PROC_NO_PARENT` |
| 72 | `proc_list` | `RBX=buf, RCX=max` | count / -1 |
| 73 | `kill` | `RBX=pid` | 0 / -1 |
| 77 | `spawn_redir` | `RBX=path, RCX=argc, RDX=argv, RSI=spec*` | child pid / -1 |
| 78 | `fork` | — | child pid (parent) / 0 (child) / -1 |
| 79 | `execve` | `RBX=path, RCX=argc, RDX=argv` | never returns / -1 |

## Memory

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 9 | `alloc` | `RBX=size` | pointer / NULL |
| 10 | `free` | `RBX=ptr` | — |
| 19 | `realloc` | `RBX=ptr, RCX=old, RDX=new` | pointer / NULL |
| 42 | `get_cr3` | — | CR3 value |
| 44 | `is_mapped` | `RBX=addr` | 1 / 0 |

## Filesystem (KyuzenFS)

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 5 | `format` | — | 0 / -1 (root only) |
| 6 | `list` | — | — |
| 7 | `fs_read` | `RBX=name, RCX=buf, RDX=cap` | bytes / -1 |
| 8 | `fs_delete` | `RBX=name` | 0 / -1 |
| 11 | `file_exists` | `RBX=name` | 1 / 0 |
| 12 | `file_size` | `RBX=name` | size / -1 |
| 13 | `read_file_to_buffer` | `RBX=name, RCX=buf, RDX=cap` | size / 0 |
| 18 | `create_file` | `RBX=name, RCX=data, RDX=size` | 0 / -1 |
| 24 | `get_file_list` | `RBX=buf, RCX=max` | count / -1 |
| 64 | `mkdir` | `RBX=path` | 0 / -1 |
| 82 | `rename` | `RBX=old, RCX=new` | 0 / -1 |
| 83 | `stat` | `RBX=path, RCX=size*, RDX=flags*` | 0 / -1 |

## VFS / File Descriptors

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 47 | `open` | `RBX=path, RCX=flags` | fd / -1 |
| 48 | `read` | `RBX=fd, RCX=buf, RDX=count` | bytes / -1 |
| 49 | `write` | `RBX=fd, RCX=buf, RDX=count` | bytes / -1 |
| 50 | `lseek` | `RBX=fd, RCX=offset, RDX=origin` | new pos / -1 |
| 51 | `close` | `RBX=fd` | 0 / -1 |
| 74 | `dup` | `RBX=oldfd` | new fd / -1 |
| 75 | `dup2` | `RBX=oldfd, RCX=newfd` | newfd / -1 |
| 76 | `pipe` | `RBX=fds*` | 0 / -1 |
| 81 | `readdir` | `RBX=fd, RCX=index, RDX=name, RSI=cap` | 1 / 0 / -1 |

## Window Manager / Graphics

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 22 | `draw_pixel` | `RBX=x, RCX=y, RDX=color` | — |
| 23 | `draw_image` | `RBX=x, RCX=y, RDX=w, RSI=h, RDI=buf` | — |
| 26 | `draw_string` | `RBX=str, RCX=x, RDX=y, RSI=color` | — |
| 29 | `get_event` | `RBX=event*` | 1 / 0 |
| 30 | `create_window` | `RBX=x, RCX=y, RDX=w, RSI=h, RDI=title` | window id / -1 |
| 31 | `update_window` | `RBX=id, RCX=canvas*` | 0 / -1 |
| 32 | `destroy_window` | `RBX=id` | 0 / -1 |
| 40 | `get_window_pos` | `RBX=x*, RCX=y*` | — |
| 58 | `set_cursor` | `RBX=kind` | — |
| 59 | `create_desktop` | `RBX=w, RCX=h` | window id / -1 |
| 60 | `set_title` | `RBX=id, RCX=title` | 0 / -1 |
| 61 | `get_windows` | `RBX=buf, RCX=max` | count / -1 |
| 62 | `activate_window` | `RBX=id` | 0 / -1 |
| 63 | `get_screen_size` | `RBX=w*, RCX=h*` | — |
| 65 | `gpu_stats` | `RBX=stats*` | 0 / -1 |
| 66 | `update_window_rect` | `RBX=id, RCX=x, RDX=y, RSI=w, RDI=h, +canvas` | 0 / -1 |
| 67 | `set_window_opaque` | `RBX=id` | 0 / -1 |
| 84 | `wallpaper_reload` | — | legacy alias of 85 |
| 85 | `hot_reload` | `RBX=target, RCX=flags` | 0 / -1 |

## Networking

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 41 | `ping` | `RBX=host` | RTT ms / error |
| 52 | `socket` | `RBX=type` | handle / error |
| 53 | `connect` | `RBX=handle, RCX=addr, RDX=port` | 0 / error |
| 54 | `sock_send` | `RBX=handle, RCX=buf, RDX=len` | bytes / error |
| 55 | `sock_recv` | `RBX=handle, RCX=buf, RDX=len` | bytes / error |
| 56 | `sock_close` | `RBX=handle` | 0 / error |
| 86 | `resolve` | `RBX=host, RCX=out` | 0 / error |

## System Information

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 14 | `uptime` | — | milliseconds |
| 15 | `total_ram` | — | bytes |
| 16 | `used_ram` | — | bytes |
| 17 | `get_cpu_string` | `RBX=buf` | — |
| 20 | `get_time` | `RBX=buf` (6×u32) | — |
| 35 | `total_disk` | — | bytes |
| 36 | `used_disk` | — | bytes |
| 37 | `cpu_usage` | — | percent |
| 38 | `shutdown` | — | root only |
| 39 | `reboot` | — | root only |
| 80 | `crash_notice` | `RBX=crash_notice_t*` | 1 / 0 |
| 87 | `entropy` | `RBX=buf, RCX=len` | 0 / error |

## Miscellaneous

| # | Name | Args | Returns |
| --- | --- | --- | --- |
| 1 | `print` | `RBX=str` | — |
| 2 | `clear_screen` | — | — |
| 3 | `read_keyboard` | `RBX=buf, RCX=cap` | count / -1 |
| 27 | `set_uid` | `RBX=uid` | 0 / -1 (root only) |
| 28 | `get_uid` | — | uid |
| 21 | *(reserved)* | — | — |

## Return Conventions

- `-1` generally signals a generic failure or denial.
- `0` generally signals success where no value is returned.
- Filesystem operations return POSIX-style negative codes (see
  [Filesystem](../filesystem/README.md)).
- Socket operations return `KSOCK_*` codes (see
  [Networking](../networking/README.md)).
- `sys_kill` returns `0` on success or `-1` if denied.

## Related Documentation

- [Syscalls](../kernel/syscalls.md) — ABI, dispatch, boundary copy
- [Process Model](../kernel/processes.md)
- [VFS & File Descriptors](../filesystem/vfs.md)
- [Socket Model](../networking/sockets.md)
