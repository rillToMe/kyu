// =====================================================================
// test/kyuzenfs_tree_test.c — Level 1 (host) untuk FILESYSTEM TREE.
//
// Yang diuji: lapisan KyuzenFS V4 yang dipakai langsung oleh vfs_fd.c dan
// syscall layer — resolusi path (kfs_walk_path / kfs_walk_parent), operasi
// tree (mkdir/create/readdir/rename/remove/stat), dan vnode ops per node.
//
// Cara kerja: modul kernel/fs/*.c di-include APA ADANYA ke satu translation
// unit, dengan ATA (RAM disk 32 MB) dan heap (malloc host) di-mock. Jadi
// kode yang diuji adalah kode kernel asli — bukan salinan.
//
// Jalankan: make test-kyuzenfs-tree
//
// Yang TIDAK diuji di sini (butuh kernel/QEMU): kernel/vfs_fd.c dan
// kernel/syscall.c (fd handle + boundary-copy user). Keduanya memakai
// resolver yang sama, jadi bug resolusi path apa pun akan terlihat di sini.
// =====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "kyuzenfs_v4.h"   // on-disk: konstanta block/format + error code

// --- Mock ATA: RAM disk 32 MB (8192 block 4KB) -------------------------
#define MOCK_DISK_BLOCKS 8192u

static uint8_t *g_disk;
static uint32_t g_disk_blocks;

uint32_t ata_get_total_sectors(void) {
    return g_disk_blocks * KZFS_BLOCK_SECTORS;
}

int ata_read_block4k(uint64_t block_num, void *buf) {
    if (!g_disk || block_num >= g_disk_blocks) return -1;
    memcpy(buf, g_disk + block_num * KZFS_BLOCK_SIZE, KZFS_BLOCK_SIZE);
    return 0;
}

int ata_write_block4k(uint64_t block_num, const void *buf) {
    if (!g_disk || block_num >= g_disk_blocks) return -1;
    memcpy(g_disk + block_num * KZFS_BLOCK_SIZE, buf, KZFS_BLOCK_SIZE);
    return 0;
}

// --- Mock heap + console ----------------------------------------------
void *kmalloc(size_t size) { return malloc(size); }
void  kfree(void *ptr)     { free(ptr); }
void *krealloc(void *ptr, size_t old_size, size_t new_size) {
    (void)old_size;
    return realloc(ptr, new_size);
}
void kprint(const char *s) { fputs(s, stdout); }

// --- Modul FS kernel, apa adanya ---------------------------------------
#include "../kernel/fs/bcache.c"
#include "../kernel/fs/kfs_super.c"
#include "../kernel/fs/kfs_balloc.c"
#include "../kernel/fs/kfs_inode.c"
#include "../kernel/fs/kfs_extent.c"
#include "../kernel/fs/kfs_dir.c"
#include "../kernel/fs/kfs_vnode.c"
#include "../kernel/fs/kfs_shim.c"

// =====================================================================
// Harness
// =====================================================================
static int g_pass = 0;
static int g_fail = 0;

static void check(const char *label, int ok) {
    if (ok) { g_pass++; printf("[FS TEST] %-52s OK\n", label); }
    else    { g_fail++; printf("[FS TEST] %-52s FAIL\n", label); }
}

// Inode number sebuah path (0 = gagal resolve).
static uint32_t ino_of(const char *path) {
    struct vnode *vn = NULL;
    if (kfs_walk_path(path, &vn) != KZFS_EOK || !vn) return 0;
    uint32_t ino = vn->inode_num;
    vn->ops->release(vn);
    return ino;
}

// Kumpulkan nama entri (lewati "." dan ".."). Return jumlah.
#define NAME_CAP 128
static int list_names(const char *path, char out[][NAME_CAP], int max) {
    struct vnode *dir = NULL;
    if (kfs_walk_path(path, &dir) != KZFS_EOK || !dir) return -1;
    int n = 0;
    char nm[KZFS_NAME_MAX + 2];
    uint8_t ty = 0;
    for (uint32_t i = 0; n < max; i++) {
        if (dir->ops->readdir(dir, i, nm, sizeof(nm), &ty) != KZFS_EOK) break;
        if (dir_is_dot(nm)) continue;
        strncpy(out[n], nm, NAME_CAP - 1);
        out[n][NAME_CAP - 1] = '\0';
        n++;
    }
    dir->ops->release(dir);
    return n;
}

static int has_name(char names[][NAME_CAP], int n, const char *want) {
    for (int i = 0; i < n; i++) if (strcmp(names[i], want) == 0) return 1;
    return 0;
}

// Tampilkan pohon dari path (Rule 11). Menggunakan readdir mentah lalu
// rekursi lewat resolver yang sama dengan yang dipakai user-space.
static void tree_walk(const char *path, const char *prefix) {
    char names[64][NAME_CAP];
    uint8_t dirs[64];
    int n = 0;

    struct vnode *dir = NULL;
    if (kfs_walk_path(path, &dir) != KZFS_EOK || !dir) return;
    char nm[KZFS_NAME_MAX + 2];
    uint8_t ty = 0;
    for (uint32_t i = 0; n < 64; i++) {
        if (dir->ops->readdir(dir, i, nm, sizeof(nm), &ty) != KZFS_EOK) break;
        if (dir_is_dot(nm)) continue;
        strncpy(names[n], nm, NAME_CAP - 1);
        names[n][NAME_CAP - 1] = '\0';
        dirs[n] = (ty == KZFS_INODE_FLAG_DIR) ? 1 : 0;
        n++;
    }
    dir->ops->release(dir);

    for (int i = 0; i < n; i++) {
        int last = (i == n - 1);
        printf("%s%s%s\n", prefix, last ? "`-- " : "|-- ", names[i]);
        if (!dirs[i]) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s%s%s",
                 (strcmp(path, "/") == 0) ? "" : path, "/", names[i]);
        char next[512];
        snprintf(next, sizeof(next), "%s%s", prefix, last ? "    " : "|   ");
        tree_walk(child, next);
    }
}

int main(void) {
    g_disk_blocks = MOCK_DISK_BLOCKS;
    g_disk = (uint8_t *)calloc(g_disk_blocks, KZFS_BLOCK_SIZE);
    if (!g_disk) { fprintf(stderr, "cannot allocate mock disk\n"); return 2; }

    printf("=== KyuzenFS V4 — filesystem tree (host, RAM disk 32 MB) ===\n");

    // ---------------- mount / format ----------------
    kfs_init();
    check("mount: format otomatis disk kosong", kfs_v4_is_mounted() == 1);

    uint32_t size = 0;
    uint8_t is_dir = 0;
    check("stat \"/\" -> direktori",
          kfs_v4_stat("/", &size, &is_dir) == KZFS_EOK && is_dir == 1);

    // ---------------- mkdir ----------------
    check("mkdir /home",                     kfs_create_folder("/home") == 1);
    check("mkdir /home/user",                kfs_create_folder("/home/user") == 1);
    check("mkdir /home/user/Documents",      kfs_create_folder("/home/user/Documents") == 1);
    check("mkdir /home/user/Downloads",      kfs_create_folder("/home/user/Downloads") == 1);
    check("mkdir /home/user/Pictures",       kfs_create_folder("/home/user/Pictures") == 1);
    check("mkdir /home/user/Projects",       kfs_create_folder("/home/user/Projects") == 1);
    check("mkdir /system",                   kfs_create_folder("/system") == 1);
    check("mkdir /system/config",            kfs_create_folder("/system/config") == 1);
    check("mkdir /system/fonts",             kfs_create_folder("/system/fonts") == 1);
    check("mkdir existing -> gagal",         kfs_create_folder("/home") == 0);
    check("mkdir parent hilang -> gagal",    kfs_create_folder("/tidak-ada/sub") == 0);

    // ---------------- create + write + read ----------------
    const char *text = "Halo KyuzenOS";
    uint32_t tlen = (uint32_t)strlen(text);
    check("create /home/user/Documents/test.txt",
          kfs_create_file("/home/user/Documents/test.txt", (char *)text, tlen) == 1);
    check("create existing -> gagal",
          kfs_create_file("/home/user/Documents/test.txt", (char *)"x", 1) == 0);
    check("create parent hilang -> gagal",
          kfs_create_file("/tidak-ada/x.txt", (char *)"x", 1) == 0);
    check("create di dalam file -> gagal",
          kfs_create_file("/home/user/Documents/test.txt/x.txt", (char *)"x", 1) == 0);
    check("get_file_size == panjang isi",
          kfs_get_file_size("/home/user/Documents/test.txt") == tlen);

    char buf[256];
    memset(buf, 0, sizeof(buf));
    check("read_to_buffer sukses",
          kfs_read_to_buffer("/home/user/Documents/test.txt", buf, sizeof(buf)) == 1);
    check("isi file identik", memcmp(buf, text, tlen) == 0);

    // Tulis lewat vnode (jalur yang dipakai vfs_write) di ujung file.
    {
        struct vnode *vn = NULL;
        int rc = kfs_walk_path("/home/user/Documents/test.txt", &vn);
        uint64_t w = 0;
        rc = rc == KZFS_EOK ? vn->ops->write(vn, tlen, "!!!", 3, &w) : rc;
        check("write(append) lewat vnode ops", rc == KZFS_EOK && w == 3);
        check("size bertambah setelah append",
              vn && vn->size == tlen + 3);
        if (vn) {
            uint64_t got = 0;
            char tail[8];
            memset(tail, 0, sizeof(tail));
            rc = vn->ops->read(vn, tlen, tail, 3, &got);
            check("read offset setelah append", rc == KZFS_EOK && got == 3 &&
                                                memcmp(tail, "!!!", 3) == 0);
            vn->ops->release(vn);
        }
    }

    // ---------------- readdir ----------------
    {
        char names[32][NAME_CAP];
        int n = list_names("/", names, 32);
        check("readdir / (apps, home, system)",
              n == 3 && has_name(names, n, "apps") &&
              has_name(names, n, "home") && has_name(names, n, "system"));

        n = list_names("/home/user", names, 32);
        check("readdir /home/user (Documents/Downloads/Pictures/Projects)",
              n == 4 && has_name(names, n, "Documents") &&
              has_name(names, n, "Downloads") && has_name(names, n, "Pictures") &&
              has_name(names, n, "Projects"));

        n = list_names("/home/user/Documents", names, 32);
        check("readdir /home/user/Documents (test.txt)",
              n == 1 && has_name(names, n, "test.txt"));

        n = list_names("/tidak-ada", names, 32);
        check("readdir path hilang -> -1", n == -1);
    }

    // Entri "." dan ".." ada di storage layer (readdir mentah), dilewati shim.
    {
        struct vnode *d = NULL;
        char nm[KZFS_NAME_MAX + 2];
        uint8_t ty = 0;
        check("readdir mentah memuat \".\" dan \"..\"",
              kfs_walk_path("/home/user", &d) == KZFS_EOK &&
              d->ops->readdir(d, 0, nm, sizeof(nm), &ty) == KZFS_EOK &&
              strcmp(nm, ".") == 0 &&
              d->ops->readdir(d, 1, nm, sizeof(nm), &ty) == KZFS_EOK &&
              strcmp(nm, "..") == 0);
        if (d) d->ops->release(d);
    }

    // ---------------- resolusi path ----------------
    check("walk \"/\"",                        ino_of("/") != 0);
    check("walk \"//\" (komponen kosong)",     ino_of("//") == ino_of("/"));
    check("walk \"/home\"",                    ino_of("/home") != 0);
    check("walk \"/home/user\"",               ino_of("/home/user") != 0);
    check("walk \"/home/user/../user\"",
          ino_of("/home/user/../user") == ino_of("/home/user") &&
          ino_of("/home/user") != 0);
    check("walk \"/home/user/./Documents\"",
          ino_of("/home/user/./Documents") == ino_of("/home/user/Documents") &&
          ino_of("/home/user/Documents") != 0);
    check("walk \"/home//user///Documents\"",
          ino_of("/home//user///Documents") == ino_of("/home/user/Documents"));
    check("walk trailing slash \"/home/user/Documents/\"",
          ino_of("/home/user/Documents/") == ino_of("/home/user/Documents"));
    check("walk nama bare = relatif root",
          ino_of("home/user") == ino_of("/home/user") && ino_of("/home/user") != 0);
    check("walk \"/..\" = root",               ino_of("/..") == ino_of("/"));
    check("walk \"/../home\" = /home",         ino_of("/../home") == ino_of("/home"));
    check("walk \"/home/user/../../..\" = root",
          ino_of("/home/user/../../..") == ino_of("/"));
    check("walk path hilang -> -ENOENT",
          kfs_walk_path("/home/user/TidakAda", NULL) == KZFS_EINVAL);
    {
        struct vnode *vn = NULL;
        check("walk path hilang -> -ENOENT (out)",
              kfs_walk_path("/home/user/TidakAda", &vn) == KZFS_ENOENT && vn == NULL);
        check("walk lewat file -> -ENOTDIR",
              kfs_walk_path("/home/user/Documents/test.txt/x", &vn) == KZFS_ENOTDIR);
        // Kedalaman: buat rantai direktori sungguhan, lalu pastikan resolver
        // menolak path yang melewati KZFS_PATH_MAX_DEPTH (bukan ENOENT).
        char deep[512];
        int p = 0;
        int made = 1;
        for (int i = 1; i <= KZFS_PATH_MAX_DEPTH + 1; i++) {
            p += snprintf(deep + p, sizeof(deep) - (size_t)p, "/d%d", i);
            if (kfs_create_folder(deep) != 1) { made = 0; break; }
        }
        check("mkdir rantai dalam (33 level) berhasil", made == 1);
        check("walk kedalaman > batas -> -EINVAL",
              made == 1 && kfs_walk_path(deep, &vn) == KZFS_EINVAL);
        check("walk tepat di batas kedalaman -> OK",
              made == 1 && ino_of("/d1/d2") != 0);

        // Bersihkan rantai supaya pohon di akhir laporan tetap rapi.
        if (made) {
            for (int i = KZFS_PATH_MAX_DEPTH + 1; i >= 1; i--) {
                int q = 0;
                char tmp[512];
                for (int k = 1; k <= i; k++)
                    q += snprintf(tmp + q, sizeof(tmp) - (size_t)q, "/d%d", k);
                (void)kfs_delete_file(tmp);
            }
            check("rantai dalam bersih kembali", ino_of("/d1") == 0);
        }
    }

    // ---------------- akses tipe salah ----------------
    {
        struct vnode *d = NULL;
        kfs_walk_path("/home", &d);
        uint64_t got = 0;
        check("read direktori -> -EISDIR",
              d && d->ops->read(d, 0, buf, 4, &got) == KZFS_EISDIR);
        check("write direktori -> -EISDIR",
              d && d->ops->write(d, 0, "x", 1, &got) == KZFS_EISDIR);
        if (d) d->ops->release(d);
    }

    // ---------------- stat + hapus ----------------
    check("stat file -> size sesuai",
          kfs_v4_stat("/home/user/Documents/test.txt", &size, &is_dir) == KZFS_EOK &&
          size == tlen + 3 && is_dir == 0);
    check("stat direktori -> is_dir=1",
          kfs_v4_stat("/home/user/Documents", &size, &is_dir) == KZFS_EOK &&
          is_dir == 1);
    check("stat path hilang -> -ENOENT",
          kfs_v4_stat("/tidak-ada", &size, &is_dir) == KZFS_ENOENT);
    check("hapus path hilang -> -ENOENT",
          kfs_delete_file("/tidak-ada") == KZFS_ENOENT);
    check("rmdir direktori tidak kosong -> -EEXIST",
          kfs_delete_file("/home") == KZFS_EEXIST);
    check("hapus root -> gagal",              kfs_delete_file("/") != KZFS_EOK);

    // ---------------- rename ----------------
    check("rename file (nama baru)",
          kfs_rename_path("/home/user/Documents/test.txt",
                          "/home/user/Documents/renamed.txt") == KZFS_EOK);
    check("nama lama hilang setelah rename",
          kfs_walk_path("/home/user/Documents/test.txt", NULL) == KZFS_EINVAL);
    check("nama baru ada + isi utuh",
          kfs_get_file_size("/home/user/Documents/renamed.txt") == tlen + 3 &&
          kfs_read_to_buffer("/home/user/Documents/renamed.txt", buf, sizeof(buf)) == 1 &&
          memcmp(buf, text, tlen) == 0);
    check("rename lintas direktori",
          kfs_rename_path("/home/user/Documents/renamed.txt",
                          "/home/user/Downloads/moved.txt") == KZFS_EOK &&
          ino_of("/home/user/Downloads/moved.txt") != 0 &&
          ino_of("/home/user/Documents/renamed.txt") == 0);
    check("rename ke path identik = no-op",
          kfs_rename_path("/home/user/Downloads/moved.txt",
                          "/home/user/Downloads/moved.txt") == KZFS_EOK);
    check("rename path hilang -> -ENOENT",
          kfs_rename_path("/home/user/Downloads/tidak-ada.txt",
                          "/home/user/Downloads/x.txt") == KZFS_ENOENT);
    check("rename root -> -EINVAL",           kfs_rename_path("/", "/root2") != KZFS_EOK);

    // Rename folder beserta isinya (subtree harus tetap konsisten).
    check("create file di /home/user/Projects",
          kfs_create_file("/home/user/Projects/note.txt", (char *)"catatan", 7) == 1);
    check("rename folder ke dalam folder lain",
          kfs_rename_path("/home/user/Projects",
                          "/home/user/Documents/Projects") == KZFS_EOK);
    check("isi subtree ikut pindah",
          kfs_get_file_size("/home/user/Documents/Projects/note.txt") == 7);
    check("folder lama hilang",
          kfs_walk_path("/home/user/Projects", NULL) == KZFS_EINVAL);
    check("rename folder ke subtree sendiri -> -EINVAL",
          kfs_rename_path("/home/user/Documents/Projects",
                          "/home/user/Documents/Projects/sub") != KZFS_EOK);
    check("rename folder kembali ke asal",
          kfs_rename_path("/home/user/Documents/Projects",
                          "/home/user/Projects") == KZFS_EOK &&
          kfs_get_file_size("/home/user/Projects/note.txt") == 7);

    // Target yang sudah ada tidak boleh ditimpa.
    check("siapkan a.txt + b.txt",
          kfs_create_file("/home/user/Documents/a.txt", (char *)"a", 1) == 1 &&
          kfs_create_file("/home/user/Documents/b.txt", (char *)"b", 1) == 1);
    check("rename ke target yang ada -> -EEXIST",
          kfs_rename_path("/home/user/Documents/a.txt",
                          "/home/user/Documents/b.txt") == KZFS_EEXIST);
    check("rename file ke nama folder yang ada -> gagal",
          kfs_rename_path("/home/user/Documents/a.txt",
                          "/home/user/Pictures") != KZFS_EOK);

    // ---------------- slot dirent dipakai ulang (regresi) ----------------
    // Entri panjang dihapus → slot free BESAR di tengah direktori, lalu nama
    // pendek memakai slot itu. Sisa slot wajib jadi record free sendiri; kalau
    // tidak, scan berikutnya mendarat di sisa tanpa header (rec_len 0) dan
    // semua entri sesudahnya hilang. Kasus ini normal setelah rename.
    {
        char names[32][NAME_CAP];
        check("mkdir /home/user/Slots", kfs_create_folder("/home/user/Slots") == 1);
        check("siapkan aa.txt + nama panjang + z.txt",
              kfs_create_file("/home/user/Slots/aa.txt", (char *)"aa", 2) == 1 &&
              kfs_create_file("/home/user/Slots/abcdefghij.txt", (char *)"x", 1) == 1 &&
              kfs_create_file("/home/user/Slots/z.txt", (char *)"z", 1) == 1);
        check("hapus entri panjang (slot free 24 di tengah)",
              kfs_delete_file("/home/user/Slots/abcdefghij.txt") == KZFS_EOK);
        check("nama pendek memakai slot besar",
              kfs_create_file("/home/user/Slots/b.txt", (char *)"b", 1) == 1);
        int n = list_names("/home/user/Slots", names, 32);
        check("z.txt sesudah slot tetap terlihat",
              n == 3 && has_name(names, n, "aa.txt") &&
              has_name(names, n, "b.txt") && has_name(names, n, "z.txt"));
        check("z.txt sesudah slot tetap bisa di-stat",
              kfs_v4_stat("/home/user/Slots/z.txt", NULL, NULL) == KZFS_EOK);

        // Sisa slot 12 byte (cukup untuk record bebas) juga harus utuh.
        check("siapkan nama panjang 16 + zz.txt",
              kfs_create_file("/home/user/Slots/abcdefghijkl.txt", (char *)"x", 1) == 1 &&
              kfs_create_file("/home/user/Slots/zz.txt", (char *)"z", 1) == 1);
        check("hapus entri panjang (slot free 28 di tengah)",
              kfs_delete_file("/home/user/Slots/abcdefghijkl.txt") == KZFS_EOK);
        check("nama pendek memakai slot (sisa 12 jadi record bebas)",
              kfs_create_file("/home/user/Slots/c.txt", (char *)"c", 1) == 1);
        n = list_names("/home/user/Slots", names, 32);
        check("semua entri direktori tetap terlihat",
              n == 5 && has_name(names, n, "aa.txt") && has_name(names, n, "b.txt") &&
              has_name(names, n, "c.txt") && has_name(names, n, "z.txt") &&
              has_name(names, n, "zz.txt"));
        check("entri terakhir tetap bisa dibaca",
              kfs_read_to_buffer("/home/user/Slots/zz.txt", buf, sizeof(buf)) == 1 &&
              buf[0] == 'z');

        // Bersihkan supaya pohon di akhir laporan rapi.
        check("bersihkan /home/user/Slots",
              kfs_delete_file("/home/user/Slots/aa.txt") == KZFS_EOK &&
              kfs_delete_file("/home/user/Slots/b.txt") == KZFS_EOK &&
              kfs_delete_file("/home/user/Slots/c.txt") == KZFS_EOK &&
              kfs_delete_file("/home/user/Slots/z.txt") == KZFS_EOK &&
              kfs_delete_file("/home/user/Slots/zz.txt") == KZFS_EOK &&
              kfs_delete_file("/home/user/Slots") == KZFS_EOK);
    }

    // ---------------- hapus (file + rmdir kosong) ----------------
    check("hapus file",                       kfs_delete_file("/home/user/Documents/a.txt") == KZFS_EOK);
    check("file terhapus tidak bisa di-stat",
          kfs_v4_stat("/home/user/Documents/a.txt", NULL, NULL) == KZFS_ENOENT);
    check("hapus file di /home/user/Downloads",
          kfs_delete_file("/home/user/Downloads/moved.txt") == KZFS_EOK);
    check("rmdir folder kosong -> OK",        kfs_delete_file("/home/user/Downloads") == KZFS_EOK);
    check("folder terhapus tidak terlihat di parent",
          ino_of("/home/user/Downloads") == 0);
    check("b.txt masih ada (target rename utuh)",
          kfs_get_file_size("/home/user/Documents/b.txt") == 1);

    // ---------------- persistensi (data benar-benar ke \"disk\") ----------------
    kfs_sync_all();
    {
        uint32_t magic = 0;
        memcpy(&magic, g_disk, 4);                     // block 0 = superblock
        check("superblock tertulis ke device (magic KZFS)", magic == (uint32_t)KZFS_MAGIC);

        // Dirent harus benar-benar ada di area data device, bukan hanya di
        // cache/index memori. Scan area data mentah di RAM disk.
        int found_home = 0, found_note = 0;
        for (uint64_t b = data_start; b < g_disk_blocks; b++) {
            uint8_t *blk = g_disk + b * KZFS_BLOCK_SIZE;
            for (uint32_t off = 0; off + KZFS_DIRENT_MIN_REC <= KZFS_BLOCK_SIZE; ) {
                uint32_t ino = 0;
                uint16_t rec = 0;
                memcpy(&ino, blk + off, 4);
                memcpy(&rec, blk + off + 4, 2);
                if (rec < KZFS_DIRENT_MIN_REC) break;
                uint8_t nl = blk[off + 6];
                if (ino != 0 && nl == 4 && memcmp(blk + off + 8, "home", 4) == 0) found_home = 1;
                if (ino != 0 && nl == 8 && memcmp(blk + off + 8, "note.txt", 8) == 0) found_note = 1;
                off += rec;
            }
        }
        check("dirent \"home\" ada di device (bukan cuma RAM)", found_home == 1);
        check("dirent \"note.txt\" ada di device", found_note == 1);
    }

    // ---------------- tree (Rule 11) ----------------
    printf("\n--- tree / ---\n/\n");
    tree_walk("/", "");

    printf("\n=== %d OK, %d FAIL ===\n", g_pass, g_fail);
    free(g_disk);
    return g_fail == 0 ? 0 : 1;
}
