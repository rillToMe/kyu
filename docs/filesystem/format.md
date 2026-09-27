# KyuzenFS On-Disk Format

This document specifies the KyuzenFS V4 on-disk layout. The format is defined
in `include/kyuzenfs_v4.h` and implemented in `kernel/fs/`.

## Constants

| Constant | Value | Meaning |
| --- | --- | --- |
| `KZFS_MAGIC` | `0x53465A4B` | "KZFS" (little-endian) |
| `KZFS_VERSION` | `0x00040000` | Version 4.0 |
| `KZFS_BLOCK_SIZE` | 4096 | Filesystem block size (bytes) |
| `KZFS_BLOCK_SECTORS` | 8 | Sectors per block (512-byte sectors) |
| `KZFS_SECTOR_SIZE` | 512 | Disk sector size |
| `KZFS_INODE_SIZE` | 128 | Inode size (bytes) |
| `KZFS_INODES_PER_BLOCK` | 32 | Inodes per 4 KB block |
| `KZFS_NAME_MAX` | 255 | Maximum name length |
| `KZFS_NUM_DIRECT_EXTENTS` | 4 | Extents stored directly in the inode |
| `KZFS_CRASHDUMP_SECTORS` | 8 | Reserved crash-dump sectors (last 4 KB) |

### Address arithmetic

```text
byte address = block number * 4096
LBA          = block number * 8
```

## Layout

```text
┌──────────────────────────────────────────────────────────────┐
│ Block 0          : Superblock (4096 bytes)                    │
├──────────────────────────────────────────────────────────────┤
│ block_bitmap     : one bit per block (free/used)              │
├──────────────────────────────────────────────────────────────┤
│ inode_bitmap     : one bit per inode (free/used)              │
├──────────────────────────────────────────────────────────────┤
│ inode_table      : 32 inodes per block, 128 bytes each        │
├──────────────────────────────────────────────────────────────┤
│ data_blocks      : file and directory data                    │
├──────────────────────────────────────────────────────────────┤
│ ...                                                           │
├──────────────────────────────────────────────────────────────┤
│ last 8 sectors   : crash dump (reserved, never allocated)     │
└──────────────────────────────────────────────────────────────┘
```

## Superblock

`struct kzfs_superblock` — packed, 4096 bytes:

| Field | Type | Description |
| --- | --- | --- |
| `magic` | u32 | `KZFS_MAGIC` |
| `version` | u32 | `KZFS_VERSION` |
| `block_size` | u32 | 4096 |
| `_pad0` | — | alignment |
| `total_blocks` | u64 | Total blocks in the volume |
| `free_blocks` | u64 | Free data blocks |
| `total_inodes` | u32 | Total inodes |
| `free_inodes` | u32 | Free inodes |
| `block_bitmap_start` | u64 | First block of the block bitmap |
| `block_bitmap_blocks` | u32 | Bitmap length in blocks |
| `_pad1` | — | alignment |
| `inode_bitmap_start` | u64 | First block of the inode bitmap |
| `inode_bitmap_blocks` | u32 | Bitmap length in blocks |
| `_pad2` | — | alignment |
| `inode_table_start` | u64 | First block of the inode table |
| `inode_table_blocks` | u32 | Inode table length in blocks |
| `_pad3` | — | alignment |
| `data_blocks_start` | u64 | First data block |
| `root_inode` | u32 | Root directory inode number (always 1) |
| `_pad4` | — | alignment |
| `reserved[3992]` | u8 | Padding to 4096 bytes |

## Inode

`struct kzfs_inode` — 128 bytes:

| Field | Type | Offset | Description |
| --- | --- | --- | --- |
| `mode` | u16 | 0 | File type flags |
| `uid` | u16 | 2 | Owner uid |
| `gid` | u16 | 4 | Owner gid |
| `links_count` | u16 | 6 | Link count |
| `size_bytes` | u64 | 8 | File size |
| `blocks_used` | u64 | 16 | Blocks allocated |
| `atime` | u64 | 24 | Access time |
| `mtime` | u64 | 32 | Modification time |
| `ctime` | u64 | 40 | Change time |
| `direct[4]` | extent[4] | 48 | Four direct extents (48 bytes) |
| `indirect_extent_block` | u64 | 96 | Block holding overflow extents |
| `reserved[24]` | u8 | 104 | Reserved |

Inode flags:

| Flag | Value | Meaning |
| --- | --- | --- |
| `KZFS_INODE_FLAG_FILE` | `0x01` | Regular file |
| `KZFS_INODE_FLAG_DIR` | `0x02` | Directory |

### Runtime inode

The in-memory representation (`kzfs_v4_inode_mem_t`) adds `ino`, `dirty`, and
`refcount` to the on-disk fields. It lives in the inode cache and is pointed to
by a vnode's `fs_data`.

## Extents

`struct kzfs_extent` — 12 bytes:

| Field | Type | Description |
| --- | --- | --- |
| `start_block` | u64 | First physical block |
| `block_count` | u32 | Number of blocks (0 = empty slot) |

A file's logical blocks are mapped by walking its extents in order. Four
extents fit directly in the inode; if more are needed, an **indirect extent
block** holds additional extents:

```text
KZFS_EXTS_PER_IND_BLOCK = 4096 / 12 = 341
KZFS_MAX_EXTENTS        = 341 + 4  = 345
```

Allocation grows in runs of up to 64 blocks at a time. Contiguous extents are
merged on append.

## Directory Entries

A directory is a file containing variable-length entries:

`struct kzfs_dir_entry`:

| Field | Type | Description |
| --- | --- | --- |
| `inode_num` | u32 | Target inode (0 = free slot) |
| `rec_len` | u16 | Total record length (padded to 4) |
| `name_len` | u8 | Name length |
| `file_type` | u8 | Entry type |
| `name` | char[255] | Name |

| Constant | Value |
| --- | --- |
| `KZFS_DIRENT_MIN_REC` | 12 |
| `KZFS_DIRENT_MAX_REC` | 263 |

`.` and `..` entries are maintained on disk (ext2-style), but path resolution
uses a stack ancestor for deterministic `..` behavior. Free slots are reused by
splitting a larger record.

## Layout Computation

The layout is derived from the total block count
(`kfs_super.c::layout_compute`):

```text
total_inodes       = total_blocks / 16              # ~1 inode per 64 KB
                     clamped to [512, 262144]
                     capped at total_blocks * 16
block_bitmap_start = 1
block_bitmap_blocks = ceil((total_blocks + 7) / 8 / 4096)
inode_bitmap_start  = block_bitmap_start + block_bitmap_blocks
inode_bitmap_blocks = ceil((total_inodes + 7) / 8 / 4096)
inode_table_start   = inode_bitmap_start + inode_bitmap_blocks
inode_table_blocks  = ceil(total_inodes / 32)
data_blocks_start   = inode_table_start + inode_table_blocks
free_blocks         = total_blocks - data_blocks_start
free_inodes         = total_inodes - 1              # inode 1 = root
root_inode          = 1
```

`layout_globals_load()` must run before any disk operation; it populates the
globals used by the rest of the filesystem.

## Formatting

`kfs_format_locked`:

1. Read total sectors from the ATA device (fallback 204800).
2. Subtract the crash-dump sectors.
3. Build the bitmaps in RAM; mark all blocks before `data_blocks_start` used.
4. Initialize inode 1 as the root directory (`mode=DIR`, `links_count=3`).
5. Create the `/apps` directory.
6. Write the root directory block with `.`, `..`, and `apps`.
7. Flush bitmaps, superblock, and the block cache.

## Related Documentation

- [Filesystem Overview](README.md)
- [VFS & File Descriptors](vfs.md)
- [Memory Map](../reference/memory-map.md) — crash-dump region
