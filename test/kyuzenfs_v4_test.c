// Host test KyuzenFS V4: bcache + extent engine + direktori + vnode ops.
// Pola test/kyuzenfs_dir_test.c: ATA di-mock ke RAM (disk 32MB = 8192 block),
// heap di-mock ke malloc, spinlock di-mock. kernel/fs/kyuzenfs_v4.c dan
// kernel/fs/bcache.c di-include langsung.
//
// Build & run dari root repo:
//   clang -iquote test -iquote include -g -fsanitize=address,undefined \
//       test/kyuzenfs_v4_test.c -o /tmp/kfs4 && /tmp/kfs4
//
// PENTING: make conc / heap-stress mengglob test/*.c ke build kernel.
// Test ini WAJIB dikecualikan di Makefile seperti kyuzenfs_dir_test.c.
#include <stdint.h>

#if !defined(CONC_TEST) && !defined(HEAP_STRESS_TEST)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- heap mock ---
void* kmalloc(size_t s) { return malloc(s); }
void  kfree(void* p)    { free(p); }

// --- ATA mock: disk 32MB di RAM (65536 sektor = 8192 block 4KB) ---
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

// Wrapper 4KB (Modul 1) — versi mock: 8 sektor di RAM.
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

// --- kprint mock (diam: log KFS tidak perlu tampil di output test) ---
void kprint(const char* s) { (void)s; }
void serial_print(const char* s) { (void)s; }
int  g_serial_ready = 0;

// --- include kernel sources under test ---
#include "bcache.h"
#include "../kernel/fs/bcache.c"
#include "kyuzenfs_v4.h"
#include "vnode.h"
// KyuzenFS V4 dipecah jadi modul kecil (kontrak bersama di kfs_internal.h):
#include "../kernel/fs/kfs_super.c"
#include "../kernel/fs/kfs_balloc.c"
#include "../kernel/fs/kfs_inode.c"
#include "../kernel/fs/kfs_extent.c"
#include "../kernel/fs/kfs_dir.c"
#include "../kernel/fs/kfs_vnode.c"
#include "../kernel/fs/kfs_shim.c"

// Total block yang dikelola FS = seluruh disk DIKURANGI ekor crashdump yang
// disisihkan panic handler (KZFS_CRASHDUMP_SECTORS, lihat include/kyuzenfs_v4.h).
// Diperiksa di test_format_mount: area panic tidak boleh menggeser/menabrak
// area data file user.
static uint32_t total_blocks_expected =
    (uint32_t)((DISK_SEC - KZFS_CRASHDUMP_SECTORS) / 8u);

// ---- helpers ----
static void write_file_via_shim(const char* path, const char* data, uint32_t size) {
    assert(kfs_create_file((char*)path, (char*)data, size) == 1);
}

static void check_file(const char* path, const char* expect, uint32_t size) {
    uint8_t buf[70000];
    memset(buf, 0xAA, sizeof(buf));
    assert(kfs_exists((char*)path) == 1);
    assert(kfs_get_file_size((char*)path) == size);
    assert(kfs_read_to_buffer((char*)path, (char*)buf, sizeof(buf)) == 1);
    assert(memcmp(buf, expect, size) == 0);
}

static void test_format_mount(void) {
    memset(DISK, 0, sizeof(DISK));
    kfs_init();                                   // magic invalid → format
    assert(kfs_v4_is_mounted());

    struct vnode *root = kfs_v4_root_vnode();
    assert(root && root->type == V_DIR && root->inode_num == 1);
    root->ops->release(root);

    // "." ".." "apps" di root
    assert(kfs_exists((char*)"/apps") == 1);
    assert(kfs_exists((char*)"/nope") == 0);
    assert(kfs_resolve_dir((char*)"/", NULL) == 1);
    assert(kfs_resolve_dir((char*)"/apps", NULL) == 1);
    assert(kfs_resolve_dir((char*)"/nope", NULL) == 0);

    // Layout: FS berhenti sebelum area crashdump di ekor disk.
    uint64_t tb = 0, fb = 0; uint32_t ti = 0, fi = 0;
    kfs_v4_get_stats(&tb, &fb, &ti, &fi);
    assert(tb == total_blocks_expected);
    assert(ti > 0 && fi < ti);
    printf("PASS format/mount (+ area crashdump disisihkan)\n");
}

static void test_file_crud(void) {
    // Create + read kecil
    write_file_via_shim("/hello.txt", "Hello, KyuzenFS V4!", 19);
    check_file("/hello.txt", "Hello, KyuzenFS V4!", 19);

    // Overwrite via create sama → ditolak (EEXIST path)
    assert(kfs_create_file((char*)"/hello.txt", (char*)"x", 1) == 0);

    // Folder bertingkat + file di dalamnya
    assert(kfs_create_folder((char*)"/docs") == 1);
    write_file_via_shim("/docs/a.txt", "AAA", 3);
    write_file_via_shim("/docs/deep", NULL, 0);   // file kosong
    check_file("/docs/a.txt", "AAA", 3);
    assert(kfs_get_file_size((char*)"/docs/deep") == 0);

    // get_file_list: root punya apps, docs, hello.txt (tanpa ./..)
    typedef struct { char filename[24]; uint32_t size; uint8_t is_folder; } info_t;
    info_t list[16];
    int n = kfs_get_file_list((char*)"/", list, 16);
    assert(n == 3);
    int found_apps = 0, found_docs = 0, found_hello = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(list[i].filename, "apps") && list[i].is_folder) found_apps = 1;
        if (!strcmp(list[i].filename, "docs") && list[i].is_folder) found_docs = 1;
        if (!strcmp(list[i].filename, "hello.txt") && !list[i].is_folder) found_hello = 1;
    }
    assert(found_apps && found_docs && found_hello);

    int m = kfs_get_file_list((char*)"/docs", list, 16);
    assert(m == 2);

    // Delete
    kfs_delete_file((char*)"/docs/a.txt");
    assert(kfs_exists((char*)"/docs/a.txt") == 0);
    assert(kfs_get_file_list((char*)"/docs", list, 16) == 1);
    printf("PASS file CRUD + direktori\n");
}

static void test_large_file_extents(void) {
    // 300KB → 75 block: direct (4) habis, indirect extent + multi-run alloc.
    static uint8_t big[300 * 1024];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7 + 3);
    write_file_via_shim("/big.bin", (char*)big, sizeof(big));

    uint8_t *rb = (uint8_t*)kmalloc(sizeof(big));
    assert(rb);
    assert(kfs_read_to_buffer((char*)"/big.bin", (char*)rb, sizeof(big)) == 1);
    assert(memcmp(rb, big, sizeof(big)) == 0);
    kfree(rb);
    printf("PASS file besar (extent indirect, 300KB)\n");
}

static void test_bcache_lru_evict(void) {
    // Cache 256 entry. Baca 400 block data (via file besar) → evict bekerja,
    // data tetap benar (dirty write-back saat evict).
    struct block_buffer *b;
    for (uint64_t blk = 6000; blk < 6400; blk++) {
        b = bcache_read(blk);
        assert(b);
        memset(b->data, (int)(blk & 0xFF), 4096);
        bcache_mark_dirty(b);
        bcache_release(b);
    }
    // Pastikan dirty ter-flush: flush semua lalu baca ulang.
    bcache_flush_all();
    for (uint64_t blk = 6000; blk < 6400; blk++) {
        b = bcache_read(blk);
        assert(b);
        for (int i = 0; i < 4096; i++) assert(b->data[i] == (uint8_t)(blk & 0xFF));
        bcache_release(b);
    }
    assert(bcache_hit_count() > 0 && bcache_miss_count() > 0);
    printf("PASS bcache LRU + write-back (hit=%llu miss=%llu)\n",
           (unsigned long long)bcache_hit_count(),
           (unsigned long long)bcache_miss_count());
}

static void test_vnode_inplace_write(void) {
    // Buka file via lookup, tulis di offset tengah (in-place, tanpa append)
    struct vnode *root = kfs_v4_root_vnode();
    assert(root);
    struct vnode *vn = NULL;
    assert(root->ops->lookup(root, "hello.txt", &vn) == KZFS_EOK && vn);
    assert(vn->type == V_REG);

    uint64_t w = 0;
    const char *patch = "KYUZEN-4";
    assert(vn->ops->write(vn, 7, patch, 8, &w) == KZFS_EOK && w == 8);
    assert(vn->size == 19);                       // size tidak berubah

    uint8_t buf[32];
    uint64_t got = 0;
    assert(vn->ops->read(vn, 0, buf, 19, &got) == KZFS_EOK && got == 19);
    assert(memcmp(buf, "Hello, KYUZEN-4 V4!", 19) == 0);

    // Extend file lewat write di luar EOF (offset 30 > size 19)
    assert(vn->ops->write(vn, 30, "TAIL", 4, &w) == KZFS_EOK && w == 4);
    assert(vn->size == 34);
    memset(buf, 0, sizeof(buf));
    assert(vn->ops->read(vn, 19, buf, 15, &got) == KZFS_EOK && got == 15);
    assert(buf[0] == 0 && buf[10] == 0);          // lubang = nol
    assert(memcmp(buf + 11, "TAIL", 4) == 0);     // offset 30 - 19 = 11

    // Truncate ke 8
    assert(vn->ops->truncate(vn, 8) == KZFS_EOK);
    assert(vn->size == 8);
    memset(buf, 0, sizeof(buf));
    assert(vn->ops->read(vn, 0, buf, 32, &got) == KZFS_EOK && got == 8);
    assert(memcmp(buf, "Hello, K", 8) == 0);

    vn->ops->release(vn);
    root->ops->release(root);
    printf("PASS vnode in-place write + extend + truncate\n");
}

static void test_persistence_across_remount(void) {
    // Flush semua, lalu remount dari disk yang sama (simulasi boot ulang):
    // kfs_init dengan superblock valid → mount_load_bitmaps.
    kfs_sync_all();
    assert(kfs_exists((char*)"/big.bin") == 1);
    assert(kfs_get_file_size((char*)"/big.bin") == 300 * 1024);

    kfs_init();
    assert(kfs_v4_is_mounted());

    static uint8_t big[300 * 1024];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7 + 3);
    uint8_t *rb = (uint8_t*)kmalloc(sizeof(big));
    assert(rb);
    assert(kfs_read_to_buffer((char*)"/big.bin", (char*)rb, sizeof(big)) == 1);
    assert(memcmp(rb, big, sizeof(big)) == 0);
    kfree(rb);
    printf("PASS persistensi lintas remount\n");
}

int main(void) {
    test_format_mount();
    test_file_crud();
    test_large_file_extents();
    test_bcache_lru_evict();
    test_vnode_inplace_write();
    test_persistence_across_remount();
    printf("SEMUA TEST KyuzenFS V4 LULUS\n");
    return 0;
}

#endif // !CONC_TEST && !HEAP_STRESS_TEST
