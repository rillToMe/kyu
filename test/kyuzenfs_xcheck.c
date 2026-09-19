// test/kyuzenfs_xcheck.c — Cross-check tools/mkfs.kyuzenfs.c <-> kernel V4.
// Image yang diformat host formatter HARUS bisa di-mount parser kernel:
// superblock, bitmap, inode table, dan dirent root harus byte-kompatibel.
// Hanya pakai API publik shim (pola test_persistence_across_remount).
//
// Build & run (lihat Makefile target test-kyuzenfs-xcheck):
//   clang -iquote test -iquote include -fsanitize=address,undefined //       test/kyuzenfs_xcheck.c -o kyuzenfs_xcheck
//   ./mkfs.kyuzenfs testimg.img && ./kyuzenfs_xcheck testimg.img
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

void* kmalloc(size_t s) { return malloc(s); }
void  kfree(void* p)    { free(p); }

#define DISK_SEC 65536
static uint8_t DISK[512 * DISK_SEC];
void ata_read_sector(uint32_t sec, uint8_t* buf) {
    if (sec < DISK_SEC) memcpy(buf, DISK + (size_t)sec * 512, 512);
    else memset(buf, 0, 512);
}
void ata_write_sector(uint32_t sec, uint8_t* buf) {
    if (sec < DISK_SEC) memcpy(DISK + (size_t)sec * 512, buf, 512);
}
uint32_t ata_get_total_sectors(void) { return DISK_SEC; }

// Wrapper 4KB mock (Modul 1) — 8 sektor di RAM.
int ata_read_block4k(uint64_t block_num, void *buf) {
    if (!buf) return -1;
    uint32_t lba0 = (uint32_t)(block_num * 8);
    for (int s = 0; s < 8; s++)
        ata_read_sector(lba0 + (uint32_t)s, (uint8_t*)buf + s * 512);
    return 0;
}
int ata_write_block4k(uint64_t block_num, const void *buf) {
    if (!buf) return -1;
    uint32_t lba0 = (uint32_t)(block_num * 8);
    for (int s = 0; s < 8; s++)
        ata_write_sector(lba0 + (uint32_t)s, (uint8_t*)buf + s * 512);
    return 0;
}
static int lock_state;
void spin_lock_irqsave(int* l) { (void)l; lock_state++; }
void spin_unlock_irqrestore(int* l) { (void)l; lock_state--; }
int  spinlock_held_by_me(int* l) { (void)l; return lock_state > 0; }
void kprintf(const char* f, ...) { va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap); }
void kprint(const char* s) { (void)s; }
void serial_print(const char* s) { (void)s; }
int  g_serial_ready = 0;

#include "kyuzenfs_v4.h"
#include "bcache.h"
#include "kyuzenfs.h"
#define KZFS_UNIT_TEST 1
#include "../kernel/fs/bcache.c"
// KyuzenFS V4 dipecah jadi modul kecil (dipakai bersama kernel/fs/kfs_internal.h):
#include "../kernel/fs/kfs_super.c"
#include "../kernel/fs/kfs_balloc.c"
#include "../kernel/fs/kfs_inode.c"
#include "../kernel/fs/kfs_extent.c"
#include "../kernel/fs/kfs_dir.c"
#include "../kernel/fs/kfs_vnode.c"
#include "../kernel/fs/kfs_shim.c"

int main(int argc, char **argv) {
    const char *img = (argc > 1) ? argv[1] : "testimg.img";
    FILE* f = fopen(img, "rb");
    assert(f);
    assert(fread(DISK, 1, sizeof(DISK), f) == sizeof(DISK));
    fclose(f);

    // 1. Mount image mkfs (superblock valid â†’ mount, bukan format).
    kfs_init();
    assert(kfs_v4_is_mounted());
    assert(kfs_exists((char*)"/apps") == 1);       // folder bawaan mkfs
    assert(kfs_exists((char*)"/x.txt") == 0);

    // 2. Tulis file via shim.
    static char data[64];
    memset(data, 'A', 40);
    assert(kfs_create_file((char*)"/x.txt", data, 40) == 1);
    assert(kfs_get_file_size((char*)"/x.txt") == 40);
    static uint8_t rb[64];
    assert(kfs_read_to_buffer((char*)"/x.txt", (char*)rb, 64) == 1);
    assert(memcmp(rb, data, 40) == 0);

    // 3. Folder bertingkat + file nested.
    assert(kfs_create_folder((char*)"/docs") == 1);
    assert(kfs_create_file((char*)"/docs/nested.txt", data, 40) == 1);
    assert(kfs_exists((char*)"/docs/nested.txt") == 1);

    // 4. Sync lalu remount (simulasi boot ulang) â€” pola test suite.
    kfs_sync_all();
    kfs_init();
    assert(kfs_v4_is_mounted());
    assert(kfs_exists((char*)"/x.txt") == 1);
    assert(kfs_get_file_size((char*)"/x.txt") == 40);
    memset(rb, 0, sizeof(rb));
    assert(kfs_read_to_buffer((char*)"/x.txt", (char*)rb, 64) == 1);
    assert(memcmp(rb, data, 40) == 0);
    assert(kfs_exists((char*)"/docs") == 1);
    assert(kfs_exists((char*)"/docs/nested.txt") == 1);
    memset(rb, 0, sizeof(rb));
    assert(kfs_read_to_buffer((char*)"/docs/nested.txt", (char*)rb, 64) == 1);
    assert(memcmp(rb, data, 40) == 0);

    printf("XCHECK PASS: image mkfs termount, tulis+baca+persistensi remount OK\n");
    return 0;
}
