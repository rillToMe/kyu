#include "kyuzenfs.h"
#include "ata.h"
#include "heap.h"
#include "string.h"
#include "fs.h"
#include "spinlock.h"

// Konstanta Internal
kfs_header_t current_fs; 
uint32_t* fat_table = 0; // Sekarang Dinamis di RAM!
uint32_t total_disk_sectors = 0;
uint32_t fat_sectors_needed = 0;
static spinlock_t fs_lock = SPINLOCK_INIT;

extern fs_node_t tty_node;
extern void print_hex(uint32_t num); 
extern uint32_t ata_get_total_sectors(void);
extern void serial_print(const char* s);
extern int g_serial_ready;

// Boot-console separation: saat 1, kprint menahan output dari TTY/framebuffer
// dan mengalirkannya ke COM1 (serial) saja. Dipakai kernel_main untuk meredam
// log verbose subsistem selama boot. 0 = perilaku normal (console).
int kprint_quiet = 0;

static uint32_t slen(const char* str) {
    uint32_t l = 0; while(str[l]) l++; return l;
}
void kprint(const char* str) {
    if (!str) return;

    // Mode verbose: diagnostik penuh → serial, console tetap bersih.
    if (kprint_quiet) {
        if (g_serial_ready) serial_print(str);
        return;
    }

    if (!tty_node.write) return;

#ifdef HEAP_WATCH_DEBUG
    // Mirror kprint to COM1 so boot log survives a watchpoint freeze / BSOD,
    // where the framebuffer TTY is no longer readable on the host side.
    serial_print(str);
#endif

    // NOTE: No lock here — kprint is called from within fs_lock-protected
    // functions. TTY output may interleave across CPUs but won't deadlock.
    write_fs(&tty_node, 0, slen(str), (uint8_t*)str);
    extern void compositor_flush(void);
    if (tty_node.write) compositor_flush();
}

// Fungsi sangat penting agar OS tidak lag saat copy file besar
static void kfs_sync_fat(uint32_t fat_index) {
    if (fat_index >= total_disk_sectors) return;
    uint32_t sector_offset = fat_index / 128; // 128 elemen 32-bit pas untuk 1 Sektor (512 Byte)
    uint8_t* fat_bytes = (uint8_t*)fat_table;
    ata_write_sector(1 + sector_offset, fat_bytes + (sector_offset * 512));
}

static uint32_t find_free_sector() {
    for (uint32_t i = current_fs.root_dir_sector + 1; i < total_disk_sectors; i++) {
        if (fat_table[i] == FAT_FREE) return i;
    }
    return 0;
}

// Cari entry bernama `name` di direktori `dir_sector` (chain sector penuh).
// Internal: caller MUST hold fs_lock.
static int find_entry_in(uint32_t dir_sector, const char* name,
                         kfs_file_entry_t* out_entry, uint32_t* out_sec, int* out_idx) {
    if (dir_sector < 2) return 0;
    uint8_t* buffer = (uint8_t*)kmalloc(512);
    uint32_t curr_sec = dir_sector;

    while (curr_sec != FAT_EOF && curr_sec != FAT_FREE) {
        ata_read_sector(curr_sec, buffer);
        kfs_file_entry_t* entries = (kfs_file_entry_t*)buffer;
        for (int i = 0; i < 16; i++) {
            if (entries[i].flags != FLAG_EMPTY && strcmp(entries[i].filename, name) == 0) {
                if (out_entry) *out_entry = entries[i];
                if (out_sec) *out_sec = curr_sec;
                if (out_idx) *out_idx = i;
                kfree(buffer);
                return 1;
            }
        }
        curr_sec = fat_table[curr_sec];
    }
    kfree(buffer);
    return 0;
}

// Tulis satu entry ke direktori `dir_sec` (slot kosong pertama, extend chain
// dengan sektor baru kalau penuh). Generic untuk file maupun folder.
// Internal: caller MUST hold fs_lock.
static int insert_dir_entry(uint32_t dir_sec, kfs_file_entry_t* new_entry) {
    uint8_t* buffer = (uint8_t*)kmalloc(512);
    uint32_t last_sec = dir_sec;
    int found_idx = -1;

    while (dir_sec != FAT_EOF && dir_sec != FAT_FREE) {
        ata_read_sector(dir_sec, buffer);
        kfs_file_entry_t* entries = (kfs_file_entry_t*)buffer;
        for (int i = 0; i < 16; i++) {
            if (entries[i].flags == FLAG_EMPTY) { found_idx = i; break; }
        }
        if (found_idx != -1) break;
        last_sec = dir_sec;
        dir_sec = fat_table[dir_sec];
    }

    if (found_idx == -1) {                 // chain penuh → sambung sektor baru
        uint32_t new_sec = find_free_sector();
        if (new_sec == 0) { kfree(buffer); return 0; }
        fat_table[last_sec] = new_sec;
        fat_table[new_sec] = FAT_EOF;
        kfs_sync_fat(last_sec); kfs_sync_fat(new_sec);
        dir_sec = new_sec;
        memset(buffer, FLAG_EMPTY, 512);
        found_idx = 0;
    }

    kfs_file_entry_t* entries = (kfs_file_entry_t*)buffer;
    entries[found_idx] = *new_entry;
    entries[found_idx].filename[22] = '\0';
    ata_write_sector(dir_sec, buffer);
    kfree(buffer);
    return 1;
}

// Ikuti path dari root, komponen per '/', lewati komponen kosong (`//`,
// leading/trailing slash). Tiap komponen harus folder. "/" / "" → root.
// Internal: caller MUST hold fs_lock.
static int resolve_dir_nolock(const char* path, uint32_t* out_dir_sector) {
    uint32_t dir_sec = current_fs.root_dir_sector;
    int i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;               // trailing '/' → selesai
        int start = i;
        while (path[i] && path[i] != '/') i++;
        int len = i - start;
        if (len > 22) return 0;            // komponen >22 char tak mungkin ada
        char comp[24];
        for (int j = 0; j < len; j++) comp[j] = path[start + j];
        comp[len] = '\0';
        kfs_file_entry_t e;
        if (!find_entry_in(dir_sec, comp, &e, NULL, NULL)) return 0;
        if (e.flags != FLAG_FOLDER) return 0;
        dir_sec = e.start_sector;
    }
    *out_dir_sector = dir_sec;
    return 1;
}

// Pecah `path` jadi (parent dir sector, komponen terakhir). Path tanpa '/'
// atau berakhiran '/' → parent root. Nama kosong (trailing '/') → 0.
// Internal: caller MUST hold fs_lock.
static int split_last_nolock(const char* path, uint32_t* out_parent, const char** out_name) {
    const char* name = path;
    for (const char* p = path; *p; p++) if (*p == '/') name = p + 1;
    if (*name == '\0') return 0;           // path berakhir '/' → tak ada nama
    char parent[128];
    int plen = (int)(name - path);         // posisi awal nama
    if (plen > 0) plen--;                  // buang '/' pemisah terakhir
    if (plen > 127) plen = 127;
    for (int i = 0; i < plen; i++) parent[i] = path[i];
    parent[plen] = '\0';
    uint32_t sec;
    if (!resolve_dir_nolock(parent, &sec)) return 0;
    *out_parent = sec;
    *out_name = name;
    return 1;
}

void kfs_format(void) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);

    total_disk_sectors = ata_get_total_sectors();
    if (total_disk_sectors == 0) total_disk_sectors = 204800; // Jika QEMU gagal, paksa 100MB

    uint32_t fat_size_bytes = total_disk_sectors * 4;
    fat_sectors_needed = (fat_size_bytes + 511) / 512;
    // FAT is flushed/reloaded a whole sector (512B) at a time, so the buffer
    // must be allocated to the sector-rounded size — not fat_size_bytes — or
    // the last ata_*_sector spills past the heap block and corrupts the
    // adjacent split-remainder header.
    uint32_t fat_alloc_bytes = fat_sectors_needed * 512;

    current_fs.magic[0] = 'K'; current_fs.magic[1] = 'Z';
    current_fs.magic[2] = 'F'; current_fs.magic[3] = 'S';
    current_fs.root_dir_sector = 1 + fat_sectors_needed;
    current_fs.total_files = 0;

    if (fat_table == 0) fat_table = (uint32_t*)kmalloc(fat_alloc_bytes);
    for (uint32_t i = 0; i < total_disk_sectors; i++) fat_table[i] = FAT_FREE;

    fat_table[0] = FAT_EOF;
    for (uint32_t i = 1; i <= fat_sectors_needed; i++) fat_table[i] = FAT_EOF;
    fat_table[current_fs.root_dir_sector] = FAT_EOF;

    uint8_t* buffer = (uint8_t*)kmalloc(512);
    memset(buffer, 0, 512); memcpy(buffer, &current_fs, 512); ata_write_sector(0, buffer);

    uint8_t* fat_bytes = (uint8_t*)fat_table;
    for (uint32_t i = 0; i < fat_sectors_needed; i++) {
        ata_write_sector(1 + i, fat_bytes + (i * 512));
    }

    memset(buffer, FLAG_EMPTY, 512);
    ata_write_sector(current_fs.root_dir_sector, buffer);
    kfree(buffer);

    kprint("[OK] Disk diformat ke KyuzenFS V3 (32-Bit Dinamis)!\n");
    spinlock_unlock_irqrestore(&fs_lock, flags);
}

// Internal: caller MUST hold fs_lock
static void kfs_format_nolock(void) {
    total_disk_sectors = ata_get_total_sectors();
    if (total_disk_sectors == 0) total_disk_sectors = 204800;

    uint32_t fat_size_bytes = total_disk_sectors * 4;
    fat_sectors_needed = (fat_size_bytes + 511) / 512;
    // Sector-rounded alloc — see kfs_format() note. Prevents FAT flush overrun.
    uint32_t fat_alloc_bytes = fat_sectors_needed * 512;

    current_fs.magic[0] = 'K'; current_fs.magic[1] = 'Z';
    current_fs.magic[2] = 'F'; current_fs.magic[3] = 'S';
    current_fs.root_dir_sector = 1 + fat_sectors_needed;
    current_fs.total_files = 0;

    if (fat_table == 0) fat_table = (uint32_t*)kmalloc(fat_alloc_bytes);
    for (uint32_t i = 0; i < total_disk_sectors; i++) fat_table[i] = FAT_FREE;

    fat_table[0] = FAT_EOF;
    for (uint32_t i = 1; i <= fat_sectors_needed; i++) fat_table[i] = FAT_EOF;
    fat_table[current_fs.root_dir_sector] = FAT_EOF;

    uint8_t* buffer = (uint8_t*)kmalloc(512);
    memset(buffer, 0, 512); memcpy(buffer, &current_fs, 512); ata_write_sector(0, buffer);

    uint8_t* fat_bytes = (uint8_t*)fat_table;
    for (uint32_t i = 0; i < fat_sectors_needed; i++) {
        ata_write_sector(1 + i, fat_bytes + (i * 512));
    }

    memset(buffer, FLAG_EMPTY, 512);
    ata_write_sector(current_fs.root_dir_sector, buffer);
    kfree(buffer);

    kprint("[OK] Disk diformat ke KyuzenFS V3 (32-Bit Dinamis)!\n");
}

void kfs_init(void) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);

    uint8_t* buffer = (uint8_t*)kmalloc(512);
    ata_read_sector(0, buffer);
    memcpy(&current_fs, buffer, sizeof(kfs_header_t));
    kfree(buffer);

    if (current_fs.magic[0] != 'K' || current_fs.magic[1] != 'Z') {
        kfs_format_nolock();
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return;
    }

    total_disk_sectors = ata_get_total_sectors();
    if (total_disk_sectors == 0) total_disk_sectors = 204800;

    uint32_t fat_size_bytes = total_disk_sectors * 4;
    fat_sectors_needed = (fat_size_bytes + 511) / 512;
    // Sector-rounded alloc — see kfs_format() note. Prevents FAT reload overrun.
    uint32_t fat_alloc_bytes = fat_sectors_needed * 512;
    if (fat_table == 0) fat_table = (uint32_t*)kmalloc(fat_alloc_bytes);

    uint8_t* fat_bytes = (uint8_t*)fat_table;
    for (uint32_t i = 0; i < fat_sectors_needed; i++) {
        ata_read_sector(1 + i, fat_bytes + (i * 512));
    }

    spinlock_unlock_irqrestore(&fs_lock, flags);
}

int kfs_create_file(char* path, char* data, uint32_t size) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);

    if (current_fs.root_dir_sector < 2) kfs_format_nolock();
    uint32_t parent_sec; const char* name;
    if (!split_last_nolock(path, &parent_sec, &name)) { spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }
    if (find_entry_in(parent_sec, name, NULL, NULL, NULL)) { kprint("Error: File sudah ada!\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }

    uint32_t total_size = size;
    uint32_t sectors_needed = (total_size == 0) ? 1 : ((total_size + 511) / 512);
    uint32_t start_sector = 0, prev_sector = 0;
    uint32_t data_offset = 0, remaining = total_size;
    uint8_t* buffer = (uint8_t*)kmalloc(512);

    for (uint32_t i = 0; i < sectors_needed; i++) {
        uint32_t free_sec = find_free_sector();
        if (free_sec == 0) { kprint("Error: Disk Penuh!\n"); kfree(buffer); spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }

        fat_table[free_sec] = FAT_EOF;
        if (start_sector == 0) start_sector = free_sec;
        else {
            fat_table[prev_sector] = free_sec;
            kfs_sync_fat(prev_sector);
        }
        kfs_sync_fat(free_sec);
        prev_sector = free_sec;

        uint32_t chunk_size = (remaining > 512) ? 512 : remaining;
        memset(buffer, 0, 512); memcpy(buffer, data + data_offset, chunk_size);
        ata_write_sector(free_sec, buffer);

        data_offset += chunk_size; remaining -= chunk_size;
    }

    kfs_file_entry_t entry;
    for (int i = 0; i < 22; i++) { entry.filename[i] = name[i]; if (!name[i]) break; }
    entry.filename[22] = '\0';
    entry.flags = FLAG_FILE;
    entry.start_sector = start_sector;
    entry.size_bytes = total_size;
    if (!insert_dir_entry(parent_sec, &entry)) { kfree(buffer); spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }

    current_fs.total_files++;
    memset(buffer, 0, 512); memcpy(buffer, &current_fs, 512); ata_write_sector(0, buffer);
    kfree(buffer); spinlock_unlock_irqrestore(&fs_lock, flags); return 1;
}

void kfs_delete_file(char* path) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);

    uint32_t parent_sec; const char* name;
    if (!split_last_nolock(path, &parent_sec, &name)) { spinlock_unlock_irqrestore(&fs_lock, flags); return; }
    kfs_file_entry_t entry; uint32_t dir_sec; int dir_idx;
    if (!find_entry_in(parent_sec, name, &entry, &dir_sec, &dir_idx)) { kprint("Error: File tidak ada!\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return; }
    // ponytail: folder tak bisa dihapus (delete rekursif = fase terpisah).
    if (entry.flags == FLAG_FOLDER) { kprint("Error: Folder tidak bisa dihapus!\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return; }

    uint32_t curr = entry.start_sector;
    while(curr != FAT_EOF && curr != FAT_FREE) {
        uint32_t next = fat_table[curr];
        fat_table[curr] = FAT_FREE;
        kfs_sync_fat(curr);
        curr = next;
    }

    uint8_t* buffer = (uint8_t*)kmalloc(512);
    ata_read_sector(dir_sec, buffer);
    kfs_file_entry_t* entries = (kfs_file_entry_t*)buffer;
    entries[dir_idx].flags = FLAG_EMPTY;
    ata_write_sector(dir_sec, buffer);

    current_fs.total_files--;
    memset(buffer, 0, 512); memcpy(buffer, &current_fs, 512); ata_write_sector(0, buffer);
    kfree(buffer); kprint("[OK] File dihapus!\n");
    spinlock_unlock_irqrestore(&fs_lock, flags);
}

int kfs_exists(char* path) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    uint32_t parent_sec; const char* name;
    int result = 0;
    if (split_last_nolock(path, &parent_sec, &name))
        result = find_entry_in(parent_sec, name, NULL, NULL, NULL);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return result;
}

uint32_t kfs_get_file_size(char* path) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    uint32_t parent_sec; const char* name;
    kfs_file_entry_t entry;
    if (split_last_nolock(path, &parent_sec, &name) &&
        find_entry_in(parent_sec, name, &entry, NULL, NULL)) {
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return entry.size_bytes;
    }
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return 0;
}

// Internal: caller MUST hold fs_lock
static uint32_t kfs_get_file_size_nolock(char* path) {
    uint32_t parent_sec; const char* name;
    kfs_file_entry_t entry;
    if (split_last_nolock(path, &parent_sec, &name) &&
        find_entry_in(parent_sec, name, &entry, NULL, NULL))
        return entry.size_bytes;
    return 0;
}

int kfs_read_to_buffer(char* path, char* out_buffer, uint32_t buffer_capacity) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    uint32_t parent_sec; const char* name;
    kfs_file_entry_t entry;
    if (!split_last_nolock(path, &parent_sec, &name) ||
        !find_entry_in(parent_sec, name, &entry, NULL, NULL)) {
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return 0;
    }
    if (entry.size_bytes > buffer_capacity) {
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return 0;
    }

    uint32_t curr = entry.start_sector;
    uint32_t offset = 0; uint32_t remaining = entry.size_bytes;
    uint8_t* temp = (uint8_t*)kmalloc(512);

    while(curr != FAT_EOF && curr != FAT_FREE && remaining > 0) {
        ata_read_sector(curr, temp);
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        memcpy(out_buffer + offset, (char*)temp, chunk);
        offset += chunk; remaining -= chunk;
        curr = fat_table[curr];
    }
    kfree(temp);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return 1;
}

// Internal: caller MUST hold fs_lock
static int kfs_read_to_buffer_nolock(char* path, char* out_buffer) {
    uint32_t parent_sec; const char* name;
    kfs_file_entry_t entry;
    if (!split_last_nolock(path, &parent_sec, &name) ||
        !find_entry_in(parent_sec, name, &entry, NULL, NULL)) return 0;

    uint32_t curr = entry.start_sector;
    uint32_t offset = 0; uint32_t remaining = entry.size_bytes;
    uint8_t* temp = (uint8_t*)kmalloc(512);

    while(curr != FAT_EOF && curr != FAT_FREE && remaining > 0) {
        ata_read_sector(curr, temp);
        uint32_t chunk = (remaining > 512) ? 512 : remaining;
        memcpy(out_buffer + offset, (char*)temp, chunk);
        offset += chunk; remaining -= chunk;
        curr = fat_table[curr];
    }
    kfree(temp);
    return 1;
}

// === FUNGSI API UNTUK TASK MANAGER (RATUSAN MB) ===
uint32_t kfs_get_total_space(void) {
    return total_disk_sectors * 512;
}

uint32_t kfs_get_used_space(void) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    uint32_t free_sec = 0;
    for (uint32_t i = current_fs.root_dir_sector + 1; i < total_disk_sectors; i++) {
        if (fat_table[i] == FAT_FREE) free_sec++;
    }
    uint32_t used_sec = total_disk_sectors - free_sec;
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return used_sec * 512;
}

void kfs_list_files(void) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    if (current_fs.magic[0] != 'K') { kprint("Disk belum diformat!\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return; }
    kprint("--- Isi Hard Disk ---\n");
    if (current_fs.total_files == 0) { kprint("(Kosong)\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return; }
    uint32_t curr_sec = current_fs.root_dir_sector;
    uint8_t* buffer = (uint8_t*)kmalloc(512);
    while (curr_sec != FAT_EOF && curr_sec != FAT_FREE) {
        ata_read_sector(curr_sec, buffer);
        kfs_file_entry_t* entries = (kfs_file_entry_t*)buffer;
        for (int i = 0; i < 16; i++) {
            if (entries[i].flags != FLAG_EMPTY) {
                kprint("- "); kprint(entries[i].filename); kprint("\n");
            }
        }
        curr_sec = fat_table[curr_sec];
    }
    kfree(buffer);
    spinlock_unlock_irqrestore(&fs_lock, flags);
}

void kfs_read_file(char* filename) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    uint32_t size = kfs_get_file_size_nolock(filename);
    if(size == 0) { kprint("File tidak ada / kosong.\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return; }
    char* buf = (char*)kmalloc(size + 1);
    if(kfs_read_to_buffer_nolock(filename, buf)) {
        buf[size] = '\0';
        kprint(buf); kprint("\n");
    } else {
        kprint("Gagal membaca file.\n");
    }
    kfree(buf);
    spinlock_unlock_irqrestore(&fs_lock, flags);
}

// Resolve path direktori → sector-nya. "/" / "" → root. Komponen bukan
// folder / tak ada → 0. (Path-aware public API, Fase 1 direktori.)
int kfs_resolve_dir(char* path, uint32_t* out_dir_sector) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    int r = resolve_dir_nolock(path, out_dir_sector);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return r;
}

// Buat folder: alokasi 1 sektor kosong sebagai chain direktori baru, lalu
// entry {FLAG_FOLDER} di parent. Parent harus sudah ada.
int kfs_create_folder(char* path) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);

    uint32_t parent_sec; const char* name;
    if (!split_last_nolock(path, &parent_sec, &name)) { spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }
    if (find_entry_in(parent_sec, name, NULL, NULL, NULL)) { kprint("Error: Folder sudah ada!\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }

    uint32_t new_sec = find_free_sector();
    if (new_sec == 0) { kprint("Error: Disk Penuh!\n"); spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }
    fat_table[new_sec] = FAT_EOF;
    kfs_sync_fat(new_sec);

    uint8_t* buffer = (uint8_t*)kmalloc(512);
    memset(buffer, FLAG_EMPTY, 512);
    ata_write_sector(new_sec, buffer);

    kfs_file_entry_t entry;
    for (int i = 0; i < 22; i++) { entry.filename[i] = name[i]; if (!name[i]) break; }
    entry.filename[22] = '\0';
    entry.flags = FLAG_FOLDER;
    entry.start_sector = new_sec;
    entry.size_bytes = 0;
    if (!insert_dir_entry(parent_sec, &entry)) {
        fat_table[new_sec] = FAT_FREE;     // kembalikan sektor folder
        kfree(buffer);
        spinlock_unlock_irqrestore(&fs_lock, flags);
        return 0;
    }

    current_fs.total_files++;
    memset(buffer, 0, 512); memcpy(buffer, &current_fs, 512); ata_write_sector(0, buffer);
    kfree(buffer); spinlock_unlock_irqrestore(&fs_lock, flags); return 1;
}

// Jembatan untuk File Manager GUI
int kfs_get_file_list(char* path, void* buffer, int max_entries) {
    uint64_t flags = spinlock_lock_irqsave(&fs_lock);
    typedef struct { char filename[24]; uint32_t size; uint8_t is_folder; } file_info_t;
    file_info_t* list = (file_info_t*)buffer;
    uint32_t dir_sec;
    if (!resolve_dir_nolock(path, &dir_sec)) { spinlock_unlock_irqrestore(&fs_lock, flags); return 0; }
    int count = 0;
    uint32_t curr_sec = dir_sec;

    uint8_t* temp = (uint8_t*)kmalloc(512);
    while (curr_sec != FAT_EOF && curr_sec != FAT_FREE && count < max_entries) {
        ata_read_sector(curr_sec, temp);
        kfs_file_entry_t* entries = (kfs_file_entry_t*)temp;
        for (int i = 0; i < 16; i++) {
            if (entries[i].flags != FLAG_EMPTY) {
                int j = 0; while(j < 22 && entries[i].filename[j] != '\0') { list[count].filename[j] = entries[i].filename[j]; j++; }
                list[count].filename[j] = '\0';
                list[count].size = entries[i].size_bytes;
                list[count].is_folder = (entries[i].flags == FLAG_FOLDER);   // FIX: bukan hardcode 0
                count++; if (count >= max_entries) break;
            }
        }
        curr_sec = fat_table[curr_sec];
    }
    kfree(temp);
    spinlock_unlock_irqrestore(&fs_lock, flags);
    return count;
}
