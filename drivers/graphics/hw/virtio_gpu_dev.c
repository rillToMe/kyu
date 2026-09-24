// ============================================================
// VirtIO-GPU Driver Core (drivers/graphics/hw/virtio_gpu_dev.c)
//
// PCI → MMIO → negotiation → virtqueue → command execution.
// Akses MMIO via hhdm_offset (HHDM me-map physical space). Semua
// akses register lewat pointer volatile.
// ============================================================

#include "virtio_gpu_dev.h"
#include "pci.h"          // pci_read_word
#include "io.h"
#include "gpu_alloc.h"
#include "spinlock.h"
#include "timer.h"        // timer_get_ticks — stat wait_ticks §9.6
#include <string.h>

extern uint64_t hhdm_offset;

virtio_gpu_dev_t g_vgpu;

static void serial_log(const char* s) {
    extern void serial_print(const char* s);
    serial_print(s);
}

// --- PCI config space 32-bit read (lewat pci_read_word) ---
static uint32_t pci_cfg_read32(uint16_t bus, uint16_t slot, uint16_t func, uint8_t off) {
    uint32_t lo = (uint32_t)pci_read_word((uint8_t)bus, (uint8_t)slot, (uint8_t)func, off);
    uint32_t hi = (uint32_t)pci_read_word((uint8_t)bus, (uint8_t)slot, (uint8_t)func, (uint8_t)(off + 2));
    return lo | (hi << 16);
}

// --- PCI config space BYTE read (offset apapun, benar untuk offset odd) ---
// pci_read_word hanya bekerja untuk offset even; untuk byte offset odd kita
// ambil word di offset&~1 lalu pilih byte berdasarkan offset&1.
static uint8_t pci_cfg_read8(uint16_t bus, uint16_t slot, uint16_t func, uint8_t off) {
    uint16_t w = pci_read_word((uint8_t)bus, (uint8_t)slot, (uint8_t)func, (uint8_t)(off & (uint8_t)~1));
    return (off & 1) ? (uint8_t)(w >> 8) : (uint8_t)(w & 0xFF);
}

// --- Scan PCI bus untuk device VirtIO-GPU (0x1AF4 / 0x1050) ---
static int pci_find_virtio_gpu(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint16_t slot = 0; slot < 32; slot++) {
            for (uint16_t func = 0; func < 8; func++) {
                uint16_t vid = pci_read_word((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0);
                if (vid == 0xFFFF) { if (func == 0) break; continue; }
                uint16_t did = pci_read_word((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 2);
                if (vid == VIRTIO_PCI_VENDOR_ID && did == VIRTIO_PCI_DEVICE_GPU) {
                    // Validasi class code = 0x03 (Display).
                    uint16_t class_info = pci_read_word((uint8_t)bus, (uint8_t)slot, (uint8_t)func, 0x0A);
                    uint8_t class_code = (class_info >> 8) & 0xFF;
                    if (class_code != 0x03) continue;
                    g_vgpu.bus = bus; g_vgpu.slot = slot; g_vgpu.func = func;
                    return 1;
                }
                if (func == 0) break;   // single-function optimization
            }
        }
    }
    return 0;
}

// --- PCI capability list parsing (cap_vndr == 0x09 = VirtIO) ---
// Layout virtio_pci_cap:
//   +0 cap_vndr, +1 cap_next, +2 cap_len, +3 cfg_type, +4 bar,
//   +5..+7 padding[3], +8 offset(32), +12 length(32)
// virtio_pci_notify_cap menambah notify_off_multiplier(32) di +16.
static int parse_capabilities(void) {
    uint8_t cap_ptr = pci_cfg_read8(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, 0x34);
    int found_common = 0, found_notify = 0, found_isr = 0, found_dev = 0;

    while (cap_ptr != 0) {
        uint8_t cap_vndr = pci_cfg_read8(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, cap_ptr);
        uint8_t cap_next = pci_cfg_read8(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, (uint8_t)(cap_ptr + 1));
        if (cap_vndr != 0x09) { cap_ptr = cap_next; continue; }

        uint8_t cfg_type = pci_cfg_read8(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, (uint8_t)(cap_ptr + 3));
        uint8_t bar = pci_cfg_read8(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, (uint8_t)(cap_ptr + 4));
        uint32_t bar_off = pci_cfg_read32(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, (uint8_t)(cap_ptr + 8));

        // Baca BAR phys (32-bit) dari config space offset 0x10 + bar*4.
        uint32_t bar_phys = pci_cfg_read32(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, (uint8_t)(0x10 + bar * 4));
        bar_phys &= ~0xF;   // clear flags (memory space)

        switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                g_vgpu.common_cfg_phys = bar_phys;
                g_vgpu.common_cfg_off  = bar_off;
                found_common = 1;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                g_vgpu.notify_base_phys = bar_phys;
                g_vgpu.notify_base_off  = bar_off;
                // notify_off_multiplier ada di +16 (virtio_pci_notify_cap).
                g_vgpu.notify_off_multiplier =
                    pci_cfg_read32(g_vgpu.bus, g_vgpu.slot, g_vgpu.func, (uint8_t)(cap_ptr + 16));
                if (g_vgpu.notify_off_multiplier == 0) g_vgpu.notify_off_multiplier = 1;
                found_notify = 1;
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                found_isr = 1;   // tidak dipakai v1 (polling)
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                g_vgpu.device_cfg_phys = bar_phys;
                g_vgpu.device_cfg_off  = bar_off;
                found_dev = 1;
                break;
            default:
                break;
        }
        cap_ptr = cap_next;
    }
    return (found_common && found_notify && found_isr && found_dev) ? 0 : -1;
}

// --- MMIO map via HHDM ---
static void map_mmio(void) {
    if (g_vgpu.common_cfg_phys)
        g_vgpu.common = (volatile virtio_pci_common_cfg_t*)(g_vgpu.common_cfg_phys + hhdm_offset + g_vgpu.common_cfg_off);
    if (g_vgpu.notify_base_phys)
        g_vgpu.notify_base = (volatile uint16_t*)(g_vgpu.notify_base_phys + hhdm_offset + g_vgpu.notify_base_off);
    if (g_vgpu.device_cfg_phys)
        g_vgpu.device_cfg = (volatile virtio_gpu_config_t*)(g_vgpu.device_cfg_phys + hhdm_offset + g_vgpu.device_cfg_off);
}

// --- Feature negotiation ---
static int negotiate_features(void) {
    volatile virtio_pci_common_cfg_t* c = g_vgpu.common;
    if (c == NULL) return -1;

    // Reset.
    c->device_status = 0;
    __asm__ volatile("" ::: "memory");

    // ACKNOWLEDGE
    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE;
    __asm__ volatile("" ::: "memory");
    // DRIVER
    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    __asm__ volatile("" ::: "memory");

    // Baca device features (window 0 dan 1).
    uint64_t dev_features = 0;
    c->device_feature_select = 0;
    __asm__ volatile("" ::: "memory");
    dev_features |= (uint64_t)c->device_feature;
    c->device_feature_select = 1;
    __asm__ volatile("" ::: "memory");
    dev_features |= ((uint64_t)c->device_feature) << 32;

    // Pilih fitur yang kita dukung: 2D only (tidak VIRGL, tidak BLOB).
    // EDID opsional. Guest features low 32-bit.
    uint32_t guest_features = 0;
    // (tidak ada yang wajib untuk 2D dasar)

    c->guest_feature_select = 0;
    __asm__ volatile("" ::: "memory");
    c->guest_feature = guest_features;
    __asm__ volatile("" ::: "memory");
    c->guest_feature_select = 1;
    __asm__ volatile("" ::: "memory");
    c->guest_feature = 0;
    __asm__ volatile("" ::: "memory");

    // FEATURES_OK
    c->device_status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK;
    __asm__ volatile("" ::: "memory");

    // Re-read status — verifikasi FEATURES_OK masih ter-set.
    uint8_t st = c->device_status;
    if (!(st & VIRTIO_STATUS_FEATURES_OK)) {
        c->device_status = 0;   // reset
        return -1;
    }

    (void)dev_features;   // dev_features dipakai untuk log/debug opsional
    return 0;
}

static void serial_log_hex32(uint32_t v);   // fwd (definisi di bawah)

// --- Setup kedua virtqueue ---
// queue_size 32: present async butuh sampai 8 chain (4 rect x transfer+flush)
// = 16 descriptor, plus headroom command sinkron (SET_SCANOUT). QEMU menawarkan
// 64 untuk controlq — 32 memberi margin tanpa memboroskan ring. Angka ini
// hanya PERMINTAAN: virtq_init() menulisnya ke common_cfg lalu memakai hasil
// baca-balik (device boleh mempertahankan ukurannya sendiri), jadi yang
// dipakai driver dan device selalu sama.
static int setup_queues(void) {
    int r = virtq_init(&g_vgpu.controlq, 0, 32, g_vgpu.common, g_vgpu.notify_base,
                       g_vgpu.notify_off_multiplier);
    if (r != 0) return -1;
    r = virtq_init(&g_vgpu.cursorq, 1, 16, g_vgpu.common, g_vgpu.notify_base,
                   g_vgpu.notify_off_multiplier);
    if (r != 0) return -1;
    // DEBUG notify routing (hapus setelah diagnosis)
    serial_log("[vgpu] q0 notify_off=");
    serial_log_hex32((uint32_t)((uint8_t*)g_vgpu.controlq.notify_addr - (uint8_t*)g_vgpu.notify_base));
    serial_log(" q1 notify_off=");
    serial_log_hex32((uint32_t)((uint8_t*)g_vgpu.cursorq.notify_addr - (uint8_t*)g_vgpu.notify_base));
    serial_log(" mult=");
    serial_log_hex32(g_vgpu.notify_off_multiplier);
    serial_log(" qsize=");
    serial_log_hex32((uint32_t)g_vgpu.controlq.queue_size);
    serial_log("\n");
    if (g_vgpu.controlq.queue_size > VGPU_FENCE_MAX_HEADS) {
        serial_log("[vgpu] queue_size melebihi tabel fence\n");
        return -1;
    }
    return 0;
}

// --- DRIVER_OK ---
static int finalize_driver_ok(void) {
    g_vgpu.common->device_status =
        VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
        VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK;
    __asm__ volatile("" ::: "memory");
    // Verifikasi device tidak set NEEDS_RESET/FAILED.
    uint8_t st = g_vgpu.common->device_status;
    if (st & VIRTIO_STATUS_DEVICE_NEEDS_RESET) return -1;
    return 0;
}

static void serial_log_hex32(uint32_t v);

int virtio_gpu_dev_probe(void) {
    if (g_vgpu.initialized) return 0;
    memset(&g_vgpu, 0, sizeof(g_vgpu));

    // 2A.1
    if (!pci_find_virtio_gpu()) {
        serial_log("[vgpu] device not found\n");
        return -1;
    }
    if (parse_capabilities() != 0) {
        serial_log("[vgpu] required capabilities missing\n");
        return -1;
    }

    // 2A.2 MMIO map
    map_mmio();
    if (g_vgpu.common == NULL || g_vgpu.notify_base == NULL || g_vgpu.device_cfg == NULL) {
        serial_log("[vgpu] MMIO map failed\n");
        return -1;
    }

    // 2A.3 negotiation
    if (negotiate_features() != 0) {
        serial_log("[vgpu] feature negotiation failed\n");
        return -1;
    }

    // 2A.4 virtqueue
    if (setup_queues() != 0) {
        serial_log("[vgpu] virtqueue setup failed\n");
        return -1;
    }

    // 2A.3 DRIVER_OK
    if (finalize_driver_ok() != 0) {
        serial_log("[vgpu] DRIVER_OK rejected\n");
        return -1;
    }

    // Pre-alokasi buffer command/response — dipakai ulang tiap command supaya
    // virtio_gpu_dev_command aman dipanggil dari IRQ (tanpa pmm_alloc_page).
    g_vgpu.cmd_lock.locked = 0;
    g_vgpu.cmd_pages_n = 0;
    for (uint32_t i = 0; i < VGPU_CMD_MAX_PAGES; i++) {
        if (gpu_alloc_page(&g_vgpu.cmd_pages[i]) != 0) break;
        g_vgpu.cmd_pages_n++;
    }
    if (g_vgpu.cmd_pages_n == 0 || gpu_alloc_page(&g_vgpu.resp_page) != 0) {
        serial_log("[vgpu] command buffer alloc failed\n");
        return -1;
    }

    // Pool buffer async (A): satu pasang per batch in-flight. Gagal =
    // probe gagal (present async butuh pool) → ghal jatuh ke software.
    g_vgpu.async_ready = 0;
    for (uint32_t i = 0; i < VGPU_ASYNC_PAIRS; i++) {
        if (gpu_alloc_page(&g_vgpu.async_cmd[i][0]) != 0) break;
        if (gpu_alloc_page(&g_vgpu.async_cmd[i][1]) != 0) {
            gpu_free_pages(&g_vgpu.async_cmd[i][0], 1);
            break;
        }
    }
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < VGPU_ASYNC_PAIRS; i++)
            if (g_vgpu.async_cmd[i][1].virt != NULL) n++;
        if (n != VGPU_ASYNC_PAIRS) {
            for (uint32_t i = 0; i < VGPU_ASYNC_PAIRS; i++) {
                if (g_vgpu.async_cmd[i][0].virt != NULL)
                    gpu_free_pages(&g_vgpu.async_cmd[i][0], 1);
                if (g_vgpu.async_cmd[i][1].virt != NULL)
                    gpu_free_pages(&g_vgpu.async_cmd[i][1], 1);
            }
            serial_log("[vgpu] async pool alloc failed\n");
            return -1;
        }
    }
    g_vgpu.async_ready = 1;

    // Phase 2C §9.4 — halaman cursorq. Gagal = non-fatal: cursor_command
    // return -1 dan compositor fallback ke software cursor.
    g_vgpu.cursor_lock.locked = 0;
    if (gpu_alloc_page(&g_vgpu.cursor_page) != 0) {
        serial_log("[vgpu] cursor buffer alloc failed (hw cursor off)\n");
    }

    g_vgpu.num_scanouts = g_vgpu.device_cfg ? g_vgpu.device_cfg->num_scanouts : 1;
    g_vgpu.initialized = 1;
    g_vgpu.negotiation_done = 1;
    serial_log("[vgpu] device ready\n");
    return 0;
}

// --- Phase 2C §9.6 — stat wait (timer ticks, kualitatif) ---
static void stats_wait_begin(uint64_t* t0) { *t0 = timer_get_ticks(); }
static void stats_wait_end(uint64_t t0) {
    g_vgpu.stats.wait_calls++;
    uint64_t now = timer_get_ticks();
    if (now >= t0) g_vgpu.stats.wait_ticks += (now - t0);
}

static uint32_t g_resource_counter = 1;   // 0 reserved

uint32_t virtio_gpu_next_resource_id(void) {
    return g_resource_counter++;
}

// ------------------------------------------------------------
// Phase 2C — bookkeeping reap (dipakai jalur sinkron & jalur fence)
// ------------------------------------------------------------

// Log angka hex 8-digit (post-mortem: response code device di serial).
static void serial_log_hex32(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xF];
    buf[10] = 0;
    serial_log(buf);
}

// Caller holds cmd_lock. A present fence covers BOTH commands and every older
// batch, regardless of used-ring completion order.
static void dev_on_reap(uint32_t head, uint32_t written) {
    if (head >= VGPU_FENCE_MAX_HEADS) return;
    uint64_t f = g_vgpu.fence_of_head[head];
    if (f == 0) return;
    g_vgpu.fence_of_head[head] = 0;

    uint8_t slot = g_vgpu.slot_of_head[head];
    g_vgpu.slot_of_head[head] = 0;
    if (slot != 0 && g_vgpu.resp_page.virt != NULL) {
        virtio_gpu_ctrl_hdr_t response;
        memcpy(&response,
               (uint8_t*)g_vgpu.resp_page.virt + VGPU_RESP_SYNC_MAX + (uint32_t)slot * VGPU_RESP_SLOT,
               sizeof(response));
        if (written < sizeof(response) || response.type != VIRTIO_GPU_RESP_OK_NODATA ||
            !(response.flags & VIRTIO_GPU_FLAG_FENCE) || response.fence_id != f) {
            g_vgpu.stats.err_count++;
            serial_log("[vgpu] async cmd error resp type=");
            serial_log_hex32(response.type);
            serial_log("\n");
        }
    }

    // Kembalikan pasangan buffer async batch ini. Kedua chain batch
    // menghitung mundur; nol = pasangan bebas dipinjam lagi.
    {
        uint8_t p = g_vgpu.pair_of_head[head];
        g_vgpu.pair_of_head[head] = 0;
        if (p != 0) {
            uint8_t pi = (uint8_t)(p - 1);
            if (pi < VGPU_ASYNC_PAIRS && g_vgpu.async_outstanding[pi] > 0)
                g_vgpu.async_outstanding[pi]--;
        }
    }

    uint64_t done = g_vgpu.fence_counter;
    for (uint32_t i = 0; i < VGPU_FENCE_MAX_HEADS; i++) {
        uint64_t pending = g_vgpu.fence_of_head[i];
        if (pending && pending <= done) done = pending - 1;
    }
    g_vgpu.last_fence_done = done;
}

// Blocking sampai chain `head` muncul di used ring (timeout §6.9).
// Chain lain yang selesai di tengah jalan ikut di-reap + tercatat —
// device memproses controlq in-order, jadi fence bisa selesai di tengah
// wait command sinkron dan tabel fence tidak boleh melewatkannya.
static int dev_wait_head(uint32_t head, uint32_t* out_len) {
    uint64_t t0;
    stats_wait_begin(&t0);
    uint32_t iter = 0;
    int r = -1;
    for (;;) {
        uint32_t h = 0, l = 0;
        if (virtq_poll(&g_vgpu.controlq, &h, &l) == 0) {
            dev_on_reap(h, l);
            if (h == head) {
                if (out_len) *out_len = l;
                r = 0;
                break;
            }
            continue;
        }
        if (++iter > VIRTQ_POLL_MAX_ITER) break;
        __asm__ volatile("pause");
    }
    stats_wait_end(t0);
    return r;
}

// --- Kirim command: bangun descriptor chain (cmd buffer, resp buffer),
// submit, notify, wait. Command & response live di halaman physical agar
// device (DMA) bisa akses. ---
//
// Response sinkron menempati offset 0 resp_page (region VGPU_RESP_SYNC_MAX,
// cukup untuk GET_DISPLAY_INFO 408 B) — TIDAK mem-nolkan seluruh halaman,
// karena slot async di atasnya bisa sedang dipakai chain in-flight.
//
// Wait memakai reap-loop virtq_poll (bukan virtq_wait) supaya chain async
// yang selesai lebih dulu ikut tercatat di tabel fence — chain FIFO device
// berarti completion fence bisa terjadi di tengah wait command sinkron.
// try_lock=1: ambil cmd_lock TANPA menunggu (jalur panic, lihat
// virtio_gpu_dev_command_try). Sisanya identik dengan jalur normal.
static int dev_command_impl(const void* cmd, uint32_t cmd_len,
                            void* out, uint32_t out_len, int try_lock) {
    if (!g_vgpu.initialized) return -1;
    if (cmd_len == 0) return -1;

    uint32_t cmd_pages = (cmd_len + 4095) / 4096;
    if (cmd_pages > g_vgpu.cmd_pages_n) return -1;

    // IRQ-safe: buffer pre-alokasi dipakai ulang, dilindungi lock.
    uint64_t lock_flags;
    if (try_lock) {
        if (!spinlock_try_lock_irqsave(&g_vgpu.cmd_lock, &lock_flags)) return -1;
    } else {
        lock_flags = spinlock_lock_irqsave(&g_vgpu.cmd_lock);
    }
    g_vgpu.stats.cmd_count++;
    g_vgpu.stats.cmd_bytes += cmd_len;

    // Halaman command TIDAK contiguous — copy per halaman, jangan sekali memcpy
    // (memcpy cmd_len ke cmd_pages[0] akan meluber ke memori lain).
    {
        const uint8_t* src = (const uint8_t*)cmd;
        uint32_t left = cmd_len;
        for (uint32_t i = 0; i < cmd_pages; i++) {
            uint32_t chunk = left > 4096 ? 4096 : left;
            memcpy(g_vgpu.cmd_pages[i].virt, src, chunk);
            src  += chunk;
            left -= chunk;
        }
    }
    if (out && out_len) {
        uint32_t clear = out_len > VGPU_RESP_SYNC_MAX ? VGPU_RESP_SYNC_MAX : out_len;
        memset(g_vgpu.resp_page.virt, 0, clear);
    }

    uint64_t addrs[VGPU_CMD_MAX_PAGES + 1];
    uint32_t lens[VGPU_CMD_MAX_PAGES + 1];
    uint16_t flags[VGPU_CMD_MAX_PAGES + 1];
    int nb = 0;
    for (uint32_t i = 0; i < cmd_pages; i++) {
        addrs[nb] = g_vgpu.cmd_pages[i].phys;
        lens[nb]  = (i == cmd_pages - 1) ? (cmd_len - i * 4096) : 4096;
        flags[nb] = 0;
        nb++;
    }
    if (out && out_len) {
        addrs[nb] = g_vgpu.resp_page.phys;
        lens[nb]  = out_len > VGPU_RESP_SYNC_MAX ? VGPU_RESP_SYNC_MAX : out_len;
        flags[nb] = VIRTQ_DESC_F_WRITE;
        nb++;
    }

    int head = virtq_submit(&g_vgpu.controlq, addrs, lens, flags, nb);
    if (head < 0) {
        spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, lock_flags);
        return -1;
    }
    if ((uint32_t)head < VGPU_FENCE_MAX_HEADS) {
        g_vgpu.fence_of_head[head] = 0;   // command sinkron — tanpa fence
        g_vgpu.slot_of_head[head] = 0;
        g_vgpu.pair_of_head[head] = 0;    // sinkron tidak pakai pool async
    }
    g_vgpu.stats.notify_count++;
    virtq_notify(&g_vgpu.controlq);

    uint32_t written = 0;
    int done = dev_wait_head((uint32_t)head, &written);

    if (out && out_len && done == 0) {
        uint32_t copy = out_len;
        if (copy > VGPU_RESP_SYNC_MAX) copy = VGPU_RESP_SYNC_MAX;
        if (copy > 4096) copy = 4096;
        memcpy(out, g_vgpu.resp_page.virt, copy);
    }

    spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, lock_flags);
    return done;
}

int virtio_gpu_dev_command(const void* cmd, uint32_t cmd_len,
                           void* out, uint32_t out_len) {
    return dev_command_impl(cmd, cmd_len, out, out_len, 0);
}

// Varian jalur panic: satu command sinkron TANPA response, memakai cmd_lock
// secara try-lock. Kalau CPU yang fault ternyata SEDANG memegang cmd_lock
// (fault di dalam jalur command itu sendiri), menunggu lock di sini berarti
// deadlock permanen — BSOD tidak akan pernah tergambar. Lebih baik command
// dilewati: pemanggil (panic) mencobanya lagi di kesempatan berikutnya.
int virtio_gpu_dev_command_try(const void* cmd, uint32_t cmd_len) {
    return dev_command_impl(cmd, cmd_len, NULL, 0, 1);
}

// ------------------------------------------------------------
// Phase 2C §9.1/§9.2 — batch submit + fence async present
// ------------------------------------------------------------
static int dev_poll_fences_locked(void);

// Kirim DUA command sebagai DUA chain terpisah (spec virtio-gpu: SATU
// command per chain — menggabungkan dua ctrl_hdr dalam satu buffer membuat
// QEMU memproses command pertama dan MEMBUANG sisanya), SATU notify, TANPA
// menunggu completion. Completion diverifikasi lewat fence di titik
// backpressure (compositor: ghal_fence_wait sebelum upload frame berikutnya).
uint64_t virtio_gpu_dev_submit2(const void* cmd1, uint32_t cmd1_len,
                                const void* cmd2, uint32_t cmd2_len) {
    if (!g_vgpu.initialized) return 0;
    if (cmd1_len == 0 || cmd2_len == 0) return 0;
    if (cmd1_len > 4096 || cmd2_len > 4096) return 0;
    if (!g_vgpu.async_ready) return 0;

    // Reserve/reap/publish under ONE queue lock. Try-lock is essential for a
    // timer flush while another CPU is using the synchronous command buffer.
    uint64_t lock_flags;
    if (!spinlock_try_lock_irqsave(&g_vgpu.cmd_lock, &lock_flags)) return 0;
    dev_poll_fences_locked();
    if (g_vgpu.controlq.num_free < 4) {
        spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, lock_flags);
        return 0;
    }

    // Pinjam satu pasang buffer eksklusif untuk batch ini. Tanpa pasangan
    // bebas tidak ada submit (bukan timpa buffer outstanding — itulah wedge
    // yang diperbaiki). Pool >= controlq penuh, jadi ini murni berarti
    // device tidak me-reap.
    int pair = -1;
    for (uint32_t i = 0; i < VGPU_ASYNC_PAIRS; i++) {
        if (g_vgpu.async_outstanding[i] == 0) { pair = (int)i; break; }
    }
    if (pair < 0) {
        spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, lock_flags);
        return 0;
    }
    g_vgpu.async_outstanding[pair] = 2;   // dua chain; reap menghitung mundur

    g_vgpu.stats.cmd_count += 2;
    g_vgpu.stats.cmd_bytes += (uint64_t)cmd1_len + cmd2_len;

    memcpy(g_vgpu.async_cmd[pair][0].virt, cmd1, cmd1_len);
    memcpy(g_vgpu.async_cmd[pair][1].virt, cmd2, cmd2_len);

    uint64_t fence = ++g_vgpu.fence_counter;
    virtio_gpu_ctrl_hdr_t* ch1 = (virtio_gpu_ctrl_hdr_t*)g_vgpu.async_cmd[pair][0].virt;
    virtio_gpu_ctrl_hdr_t* ch2 = (virtio_gpu_ctrl_hdr_t*)g_vgpu.async_cmd[pair][1].virt;
    ch1->flags |= VIRTIO_GPU_FLAG_FENCE;
    ch1->fence_id = fence;
    ch2->flags |= VIRTIO_GPU_FLAG_FENCE;
    ch2->fence_id = fence;

    // Response storage has the same lifetime as command storage. Round-robin
    // response slots alias a slow pair after enough newer pairs complete.
    uint8_t s1 = (uint8_t)(pair * 2 + 1);
    uint8_t s2 = (uint8_t)(s1 + 1);
    uint64_t resp1 = g_vgpu.resp_page.phys + VGPU_RESP_SYNC_MAX + (uint32_t)s1 * VGPU_RESP_SLOT;
    uint64_t resp2 = g_vgpu.resp_page.phys + VGPU_RESP_SYNC_MAX + (uint32_t)s2 * VGPU_RESP_SLOT;
    memset((uint8_t*)g_vgpu.resp_page.virt + VGPU_RESP_SYNC_MAX + (uint32_t)s1 * VGPU_RESP_SLOT,
           0, 2 * VGPU_RESP_SLOT);

    uint64_t addrs[2];
    uint32_t lens[2];
    uint16_t flags[2];

    addrs[0] = g_vgpu.async_cmd[pair][0].phys; lens[0] = cmd1_len; flags[0] = 0;
    addrs[1] = resp1; lens[1] = VGPU_RESP_SLOT; flags[1] = VIRTQ_DESC_F_WRITE;
    int head1 = virtq_submit(&g_vgpu.controlq, addrs, lens, flags, 2);

    addrs[0] = g_vgpu.async_cmd[pair][1].phys; lens[0] = cmd2_len;
    addrs[1] = resp2;
    int head2 = (head1 >= 0) ? virtq_submit(&g_vgpu.controlq, addrs, lens, flags, 2) : -1;

    if (head1 >= 0) {
        g_vgpu.fence_of_head[head1] = fence;
        g_vgpu.slot_of_head[head1] = s1;
        g_vgpu.pair_of_head[head1] = (uint8_t)(pair + 1);
    }
    if (head2 >= 0) {
        g_vgpu.fence_of_head[head2] = fence;
        g_vgpu.slot_of_head[head2] = s2;
        g_vgpu.pair_of_head[head2] = (uint8_t)(pair + 1);
    }

    if (head1 >= 0 && head2 >= 0) {
        g_vgpu.stats.notify_count++;
        virtq_notify(&g_vgpu.controlq);   // SATU notify untuk dua chain
        spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, lock_flags);
        return fence;
    }

    // With the locked four-descriptor reservation this cannot normally fail.
    // If only TRANSFER was published, still return its ownership fence; the
    // error counter makes the compositor retry after that DMA has retired.
    serial_log("[vgpu] submit2 partial submit\n");
    g_vgpu.stats.notify_count++;
    g_vgpu.stats.err_count++;
    virtq_notify(&g_vgpu.controlq);
    if (head1 >= 0) {
        // Satu chain masuk: reap-nya nanti yang membebaskan pasangan.
        g_vgpu.async_outstanding[pair] = 1;
    } else {
        g_vgpu.async_outstanding[pair] = 0;
    }
    spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, lock_flags);
    return head1 >= 0 ? fence : 0;
}

static int dev_poll_fences_locked(void) {
    // Wedge detector (C): device yang minta RESET/FAILED mematikan SEMUA
    // queue — tanpa cek ini gejalanya hanya "timeout di mana-mana" yang
    // butuh menitan untuk disimpulkan. Di-log sekali; submit tetap dicoba
    // (fail-open) supaya recovery device tidak dikunci driver.
    if (!g_vgpu.dev_wedged && g_vgpu.common != NULL) {
        uint8_t st = g_vgpu.common->device_status;
        if (st & (VIRTIO_STATUS_DEVICE_NEEDS_RESET | VIRTIO_STATUS_FAILED)) {
            g_vgpu.dev_wedged = 1;
            serial_log("[vgpu] device NEEDS_RESET/FAILED status=");
            serial_log_hex32((uint32_t)st);
            serial_log("\n");
        }
    }
    int n = 0;
    for (;;) {
        uint32_t h = 0, l = 0;
        if (virtq_poll(&g_vgpu.controlq, &h, &l) != 0) break;
        dev_on_reap(h, l);
        n++;
    }
    return n;
}

int virtio_gpu_dev_poll_fences(void) {
    if (!g_vgpu.initialized) return -1;
    uint64_t flags;
    if (!spinlock_try_lock_irqsave(&g_vgpu.cmd_lock, &flags)) return 0;
    int n = dev_poll_fences_locked();
    spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, flags);
    return n;
}

int virtio_gpu_dev_fence_done(uint64_t fence) {
    if (!g_vgpu.initialized || fence == 0) return 0;
    uint64_t flags;
    if (!spinlock_try_lock_irqsave(&g_vgpu.cmd_lock, &flags)) return 0;
    dev_poll_fences_locked();
    int done = fence <= g_vgpu.last_fence_done;
    spinlock_unlock_irqrestore(&g_vgpu.cmd_lock, flags);
    return done;
}

int virtio_gpu_dev_fence_wait(uint64_t fence) {
    if (!g_vgpu.initialized || fence == 0) return 0;
    uint64_t t0;
    stats_wait_begin(&t0);
    uint32_t iter = 0;
    int r = -1;
    for (;;) {
        if (virtio_gpu_dev_fence_done(fence)) { r = 0; break; }
        if (++iter > VIRTQ_POLL_MAX_ITER) break;
        __asm__ volatile("pause");
    }
    stats_wait_end(t0);
    return r;
}

// ------------------------------------------------------------
// Phase 2C §9.4 — command lewat CURSORQ (UPDATE/MOVE_CURSOR)
// ------------------------------------------------------------

// Synchronous submit→notify→wait di cursorq. Lock sendiri (cursor_lock):
// dipanggil dari dua konteks — compositor_flush (timer IRQ, cursor_move)
// dan syscall kwm_set_cursor (cursor_update). Response dibaca dari offset
// 512 halaman cursor (cmd di offset 0).
int virtio_gpu_dev_cursor_command(const void* cmd, uint32_t cmd_len) {
    if (!g_vgpu.initialized) return -1;
    if (cmd_len == 0 || cmd_len > 512) return -1;
    if (g_vgpu.cursor_page.virt == NULL) return -1;   // alloc probe gagal
    if (g_vgpu.cursor_dead) return -1;   // cursorq tak pernah selesai — hw off

    uint64_t lock_flags = spinlock_lock_irqsave(&g_vgpu.cursor_lock);
    // Command sebelumnya masih outstanding: jangan timpa cursor_page
    // (hazard yang sama dengan pool async — satu buffer, satu pemilik).
    // Caller (move per-frame) cukup melewatkan frame ini.
    if (g_vgpu.cursor_busy) {
        spinlock_unlock_irqrestore(&g_vgpu.cursor_lock, lock_flags);
        return -1;
    }
    g_vgpu.cursor_busy = 1;
    g_vgpu.stats.cmd_count++;
    g_vgpu.stats.cmd_bytes += cmd_len;

    memcpy(g_vgpu.cursor_page.virt, cmd, cmd_len);
    uint8_t* rbase = (uint8_t*)g_vgpu.cursor_page.virt;
    memset(rbase + 512, 0, 64);

    uint64_t addrs[2];
    uint32_t lens[2];
    uint16_t flags[2];
    addrs[0] = g_vgpu.cursor_page.phys; lens[0] = cmd_len; flags[0] = 0;
    addrs[1] = g_vgpu.cursor_page.phys + 512; lens[1] = 64; flags[1] = VIRTQ_DESC_F_WRITE;

    int head = virtq_submit(&g_vgpu.cursorq, addrs, lens, flags, 2);
    if (head < 0) {
        serial_log("[vgpu] cursorq submit penuh\n");
        g_vgpu.cursor_busy = 0;
        spinlock_unlock_irqrestore(&g_vgpu.cursor_lock, lock_flags);
        return -1;
    }
    serial_log("[vgpu] cursorq submit head=");
    serial_log_hex32((uint32_t)head);
    serial_log("\n");
    g_vgpu.stats.notify_count++;
    virtq_notify(&g_vgpu.cursorq);

    uint64_t t0;
    stats_wait_begin(&t0);
    uint32_t iter = 0;
    uint32_t written = 0;
    int r = -1;
    for (;;) {
        uint32_t h = 0, l = 0;
        if (virtq_poll(&g_vgpu.cursorq, &h, &l) == 0) {
            if (h == (uint32_t)head) { r = 0; written = l; break; }
            continue;   // chain lama selesai lebih dulu — direclaim oleh poll
        }
        if (++iter > VIRTQ_POLL_MAX_ITER) {
            serial_log("[vgpu] cursorq WAIT TIMEOUT\n");
            break;
        }
        __asm__ volatile("pause");
    }
    stats_wait_end(t0);
    g_vgpu.cursor_busy = 0;

    // Timeout beruntun = cursorq mati (device wedge / queue tak diproses).
    // Dinyatakan dead supaya move per-frame berikutnya gagal CEPAT (tanpa
    // timeout 10 jt iterasi di TIMER IRQ) dan compositor memakai software
    // cursor. Sukses sekali me-reset hitungan (transien dimaafkan).
    if (r != 0) {
        g_vgpu.cursor_timeouts++;
        if (g_vgpu.cursor_timeouts >= VGPU_CURSOR_MAX_TIMEOUTS &&
            !g_vgpu.cursor_dead) {
            g_vgpu.cursor_dead = 1;
            serial_log("[vgpu] cursorq dead — hw cursor off\n");
        }
    } else {
        g_vgpu.cursor_timeouts = 0;
    }

    // Cursorq TIDAK punya response body di QEMU: virtio_gpu_handle_cursor()
    // hanya virtqueue_push(vq, elem, 0) — tidak ada virtio_gpu_ctrl_response()
    // seperti controlq. Jadi chain selesai dengan written==0 = SUKSES, dan
    // isi byte di rbase+512 hanyalah sisa buffer kita (0) — bukan error
    // device. Validasi hanya kalau device benar-benar menulis payload
    // (device lain/virtio-gpu 3D boleh jadi mengirim header).
    if (r == 0 && written >= sizeof(uint32_t)) {
        uint32_t type = 0;
        memcpy(&type, rbase + 512, sizeof(type));
        if (type != VIRTIO_GPU_RESP_OK_NODATA) {
            g_vgpu.stats.err_count++;
            serial_log("[vgpu] cursor cmd error resp type=");
            serial_log_hex32(type);
            serial_log("\n");
            r = -1;
        }
    }

    spinlock_unlock_irqrestore(&g_vgpu.cursor_lock, lock_flags);
    return r;
}

const virtio_gpu_stats_t* virtio_gpu_dev_stats(void) {
    return &g_vgpu.stats;
}
