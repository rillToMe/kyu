# Filesystem

KyuzenOS uses **KyuzenFS V4**, an extent-based filesystem with a block cache, a
vnode abstraction, and a POSIX-style file-descriptor API.

## Overview

| Layer | Location | Responsibility |
| --- | --- | --- |
| Block cache | `kernel/fs/bcache.c` | Cache 4 KB blocks in memory (LRU) |
| Superblock / layout | `kernel/fs/kfs_super.c` | Mount, format, on-disk layout |
| Inode cache | `kernel/fs/kfs_inode.c` | Load, cache, and flush inodes |
| Extent engine | `kernel/fs/kfs_extent.c` | Map logical blocks to physical blocks |
| Directory operations | `kernel/fs/kfs_dir.c` | Lookup, insert, remove, path resolution |
| Vnode interface | `kernel/fs/kfs_vnode.c`, `include/vnode.h` | File/directory operations |
| VFS fd layer | `kernel/fs/vfs_fd.c`, `include/vfs.h` | File descriptors, pipes, dup/dup2 |
| Host format tool | `tools/mkfs.kyuzenfs.c` | Create a KyuzenFS image on the host |

- [KyuzenFS Format](format.md) — on-disk structures and layout
- [VFS & File Descriptors](vfs.md) — the runtime interface and fd model

## Design Summary

- **Extent-based**: file data is described by extents (start block + count),
  not a per-block pointer table. Four extents are stored directly in the inode;
  overflow goes to a single indirect extent block.
- **4 KB blocks**: the filesystem block size is 4096 bytes, matching the memory
  page size. The ATA driver reads/writes in 8-sector (4 KB) blocks.
- **Path-aware**: real directories with nesting (`mkdir`, `readdir`, `rename`).
  A directory is an inode flagged as a directory containing variable-length
  directory entries.
- **Cached**: a 1 MB block cache (256 blocks) with LRU eviction, plus an inode
  cache.
- **Crash-aware**: the last 8 sectors are reserved for a crash dump and are
  never allocated by the filesystem.

## Layout at Boot

The kernel creates this tree on first boot:

```text
/                 root directory (inode 1)
/apps             application ELF files and manifests
/system           system configuration
/system/config
/system/fonts
/home
/home/user
/home/user/{Documents,Downloads,Pictures,Projects}
```

Application binaries and manifests are installed under `/apps/`; user data
stays at the root.

## Mount and Format

- `kfs_init` initializes the block cache, loads the superblock, and mounts. If
  the superblock is invalid (`EINVAL`), the filesystem is formatted
  automatically.
- `kfs_format` (syscall 5) is root-only.
- `make mkfs` builds the host-side formatter; `./mkfs.kyuzenfs disk.img`
  creates a filesystem image.

## Path Resolution

- Paths are **absolute only**; there is no current working directory.
- Maximum path depth is `KZFS_PATH_MAX_DEPTH` = 32 components.
- `.` is a no-op; `..` pops one level (root stays root).
- A component longer than `KZFS_NAME_MAX` (255) returns `-ENAMETOOLONG`.

The ELF loader resolves bare application names to `/apps/<name>`; absolute
paths are used as-is.

## Synchronization

- A single global `fs_lock` serializes filesystem operations.
- Lock order is `fs_lock → bcache_lock` and is never reversed.
- `kfs_sync_all` flushes bitmaps, the superblock, and the cache periodically
  (approximately every 3 seconds) and at shutdown.
- `kfs_sync_all_try` is the best-effort variant used by the panic path; it
  never waits on a held lock.

## Error Codes

KyuzenFS uses POSIX-style negative error codes:

| Code | Value | Meaning |
| --- | --- | --- |
| `KZFS_EOK` | 0 | Success |
| `ENOENT` | -2 | Not found |
| `EIO` | -5 | I/O error |
| `ENOSPC` | -28 | No space left |
| `EINVAL` | -22 | Invalid argument |
| `EEXIST` | -17 | Already exists |
| `ENOTDIR` | -20 | Not a directory |
| `EISDIR` | -21 | Is a directory |
| `ENOMEM` | -12 | Out of memory |
| `ENAMETOOLONG` | -36 | Name too long |
| `EBADF` | -9 | Bad file descriptor |

## Current Limitations

- No relative paths or working directory.
- No recursive directory delete; a directory must be empty to be removed.
- No symbolic links or hard links (the inode has a `links_count` but there is
  no `link()` syscall).
- No file permissions or ownership enforcement at the filesystem level.
- No journaling; recovery relies on the crash dump region, not on-disk
  transactions.

## Development Notes

- The block cache holds `bcache_lock` across ATA I/O (a documented
  simplification for a single polled PIO disk). Do not assume I/O happens
  outside the lock.
- Inode writes are read-modify-write at the block level; writing an inode must
  not clobber the other inodes sharing its block.
- The crash-dump sector count must match between `mkfs`, `kfs_super.c`, and the
  crash-dump writer.

## Related Documentation

- [KyuzenFS Format](format.md)
- [VFS & File Descriptors](vfs.md)
- [Process Model](../kernel/processes.md) — fd inheritance across fork/spawn
- [Drivers: ATA](#ata-storage-driver) — the storage transport

## ATA Storage Driver

KyuzenFS is backed by the ATA PIO driver (`drivers/ata.c`). The driver exposes
both a sector API and a 4 KB block API:

- `ata_read_sector(lba, buffer)` / `ata_write_sector(lba, buffer)` — 512-byte
  sectors, LBA28 addressing.
- `ata_read_block4k(block, buffer)` / `ata_write_block4k(block, buffer)` — one
  block = 8 sectors.
- `ata_read_range(lba, count, buffer)` — multi-sector reads.

### Behavior and limits

- **LBA28** addressing (drive select `0xE0 | ((lba>>24) & 0x0F)`). The
  filesystem must stay within the 28-bit LBA range (~128 GB).
- **Polled PIO** with `ata_wait_bsy` / `ata_wait_drq` (bounded poll counts) and
  a 400 ns delay via four `inb(0x3F6)` reads.
- **Multi-sector batching**: `ata_read_range` can issue a single `READ SECTORS`
  command for up to `ATA_READ_BATCH_MAX_SECTORS` (8) sectors, transferring each
  with `insw_rep`. A legacy per-sector loop is always compiled and is the
  permanent fallback.
- Writes flush the cache per sector (`CACHE FLUSH 0xE7`).
- Read errors are surfaced as negative return codes from the range API; the
  single-sector API is `void` and fills the buffer with zeros on error.

See [Debugging](../development/debugging.md) for storage-related diagnostics.
