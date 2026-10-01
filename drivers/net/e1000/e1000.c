/**
 * @file e1000.c
 * @brief Intel 82540EM (e1000) Gigabit Ethernet Driver — Implementation
 *
 * Kyuzen OS — Monolithic 64-bit Higher-Half Kernel
 * Toolchain : Clang/LLVM (x86_64-pc-none-elf), -ffreestanding, -mcmodel=kernel
 *
 * Referensi:
 *   - Intel 82540EM GbE Controller Software Developer's Manual (Ref: 317453-002)
 *   - QEMU hw/net/e1000.c (untuk behaviour khusus emulator)
 *   - OSDev Wiki: https://wiki.osdev.org/Intel_8254x_Driver
 *
 * Arsitektur driver ini:
 *   ┌────────────────────────────────────────────────────────┐
 *   │  kyuzen_netif_output()  →  e1000_send()               │
 *   │                             └─ TX descriptor ring      │
 *   │                                  └─ MMIO TDT write     │
 *   │                                       └─ DMA → NIC     │
 *   │                                                        │
 *   │  PIT timer callback     →  e1000_poll()               │
 *   │                             └─ RX descriptor ring      │
 *   │                                  └─ copy to pbuf       │
 *   │                                       └─ netif->input()│
 *   └────────────────────────────────────────────────────────┘
 *
 * Memory layout:
 *   - TX/RX descriptor rings: dialokasikan via PMM page fisik + HHDM
 *   - TX/RX data buffers    : dialokasikan via PMM page fisik + HHDM
 *   - MMIO BAR0             : physical addr + hhdm_offset → virtual addr
 */

#include "e1000.h"
#include <stdint.h>
#include <stddef.h>
#include "kprint.h"

/* =========================================================================
 * KERNEL DEPENDENCIES
 *
 * Kita forward-declare semua yang kita butuhkan dari kernel.
 * Tidak ada #include dari kernel di sini untuk menjaga portabilitas driver.
 * =========================================================================*/

/* Dari kernel/kernel.c — higher-half direct map offset */
extern uint64_t hhdm_offset;

/* Dari kernel/pmm.c — physical page allocator untuk DMA memory */
extern uint64_t pmm_alloc_page(void);  /* returns phys_addr_t (uint64_t) */

/* Dari kernel/kernel.c — kernel log output */

/* Dari drivers/pci.c — membaca PCI config space */
extern uint32_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* Dari include/string.h — kernel string utilities */
extern void *memcpy(void *dest, const void *src, uint64_t n);
extern void *memset(void *dest, int c, uint64_t n);

#define E1000_DMA_PAGE_SIZE 4096ULL

/* Dari drivers/io.h — port I/O (untuk PCI config & delay) */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile ("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* =========================================================================
 * PCI HELPERS — 32-bit CONFIG READ
 *
 * pci_read_word() yang ada hanya membaca 16-bit. Kita butuh 32-bit untuk
 * membaca BAR0. Kita implementasikan sendiri di sini agar tidak mengubah
 * pci.c yang sudah ada.
 * =========================================================================*/

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)(
        (1U << 31)          |   /* Enable bit */
        ((uint32_t)bus  << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func <<  8) |
        (offset & 0xFC)
    );
    outl(0xCF8, address);
    return inl(0xCFC);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func,
                        uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)(
        (1U << 31) |
        ((uint32_t)bus  << 16) |
        ((uint32_t)slot << 11) |
        ((uint32_t)func <<  8) |
        (offset & 0xFC)
    );
    outl(0xCF8, address);
    outl(0xCFC, value);
}

/* Aktifkan Bus Master + Memory Space di PCI Command Register */
static void pci_enable_bus_master(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t cmd = pci_read32(bus, slot, func, 0x04);
    cmd |= (1U << 1) | (1U << 2); /* Memory Space Enable | Bus Master Enable */
    pci_write32(bus, slot, func, 0x04, cmd);
}

/* =========================================================================
 * DRIVER STATE (INTERNAL)
 * =========================================================================*/

/** State internal driver — satu NIC e1000 per kernel */
static struct {
    /* MMIO */
    volatile uint32_t *mmio;    /* Virtual address dari BAR0 (via hhdm_offset) */

    /* MAC address */
    uint8_t mac[6];

    /* PCI location */
    uint8_t bus, slot, func;

    /* TX ring */
    volatile e1000_tx_desc_t *tx_descs;     /* Virtual addr ring descriptor */
    uint8_t                  *tx_bufs[E1000_NUM_TX_DESC]; /* Virtual addr buffer */
    uint16_t                  tx_tail;      /* Index yang akan kita tulis berikutnya */

    /* RX ring */
    volatile e1000_rx_desc_t *rx_descs;     /* Virtual addr ring descriptor */
    uint8_t                  *rx_bufs[E1000_NUM_RX_DESC]; /* Virtual addr buffer */
    uint16_t                  rx_tail;      /* Index tail terakhir yang kita berikan ke HW */

    /* lwIP netif reference (diset oleh kyuzen_netif setelah init) */
    void                     *netif;        /* struct netif* — void* agar tidak perlu include lwip header */

    int                       initialized;
} e1000 = {0};

/* =========================================================================
 * MMIO REGISTER ACCESS
 * =========================================================================*/

static inline uint32_t e1000_reg_read(uint32_t offset) {
    return e1000.mmio[offset >> 2];
}

static inline void e1000_reg_write(uint32_t offset, uint32_t val) {
    e1000.mmio[offset >> 2] = val;
}

static void *e1000_dma_alloc_page(uint64_t *phys_out) {
    uint64_t paddr = pmm_alloc_page();
    if (paddr == 0) return NULL;

    void *virt = (void *)(paddr + hhdm_offset);
    memset(virt, 0, E1000_DMA_PAGE_SIZE);

    if (phys_out != NULL) *phys_out = paddr;
    return virt;
}

/* Spin-wait hingga kondisi terpenuhi (max ~100000 iterasi) */
static int e1000_wait_bits(uint32_t reg, uint32_t mask, uint32_t expected) {
    for (int i = 0; i < 100000; i++) {
        if ((e1000_reg_read(reg) & mask) == expected) return 0;
        /* Tiny delay via port 0x80 (POST card port, ~1µs) */
        outb(0x80, 0);
    }
    return -1; /* timeout */
}

/* =========================================================================
 * EEPROM — Baca MAC Address
 *
 * e1000 menyimpan MAC address di EEPROM pada word 0, 1, 2.
 * Kita baca via EERD register dengan polling (tanpa interrupt).
 *
 * CATATAN: QEMU mendukung EEPROM emulation — ini PASTI bekerja.
 * =========================================================================*/

/**
 * Deteksi apakah EEPROM ada (EERD bit 4 vs bit 1 untuk HW berbeda).
 * QEMU menggunakan skema lama (bit 4).
 */
static int e1000_has_eeprom(void) {
    e1000_reg_write(E1000_REG_EERD, E1000_EERD_START);
    for (int i = 0; i < 1000; i++) {
        uint32_t eerd = e1000_reg_read(E1000_REG_EERD);
        if (eerd & E1000_EERD_DONE) return 1;
        outb(0x80, 0);
    }
    return 0;
}

/** Baca satu word (16-bit) dari EEPROM pada alamat `addr`. */
static uint16_t e1000_eeprom_read(uint8_t addr) {
    uint32_t eerd = E1000_EERD_START | ((uint32_t)addr << E1000_EERD_ADDR_SHIFT);
    e1000_reg_write(E1000_REG_EERD, eerd);

    /* Poll hingga done */
    uint32_t val = 0;
    for (int i = 0; i < 100000; i++) {
        val = e1000_reg_read(E1000_REG_EERD);
        if (val & E1000_EERD_DONE) break;
        outb(0x80, 0);
    }
    return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
}

/** Baca MAC address dari EEPROM (word 0, 1, 2) atau dari RAL/RAH register. */
static void e1000_read_mac(void) {
    if (e1000_has_eeprom()) {
        /* Metode EEPROM: setiap word = 2 byte MAC, little-endian */
        uint16_t w0 = e1000_eeprom_read(0);
        uint16_t w1 = e1000_eeprom_read(1);
        uint16_t w2 = e1000_eeprom_read(2);
        e1000.mac[0] = (uint8_t)(w0 & 0xFF);
        e1000.mac[1] = (uint8_t)(w0 >> 8);
        e1000.mac[2] = (uint8_t)(w1 & 0xFF);
        e1000.mac[3] = (uint8_t)(w1 >> 8);
        e1000.mac[4] = (uint8_t)(w2 & 0xFF);
        e1000.mac[5] = (uint8_t)(w2 >> 8);
    } else {
        /* Fallback: baca dari RAL0/RAH0 yang sudah di-set oleh firmware/QEMU */
        uint32_t ral = e1000_reg_read(E1000_REG_RAL0);
        uint32_t rah = e1000_reg_read(E1000_REG_RAH0);
        e1000.mac[0] = (uint8_t)(ral & 0xFF);
        e1000.mac[1] = (uint8_t)(ral >> 8);
        e1000.mac[2] = (uint8_t)(ral >> 16);
        e1000.mac[3] = (uint8_t)(ral >> 24);
        e1000.mac[4] = (uint8_t)(rah & 0xFF);
        e1000.mac[5] = (uint8_t)(rah >> 8);
    }

    kprint("[e1000] MAC: ");
    for (int i = 0; i < 6; i++) {
        /* Cetak hex byte manual (tanpa printf) */
        uint8_t hi = (e1000.mac[i] >> 4) & 0xF;
        uint8_t lo = (e1000.mac[i] >> 0) & 0xF;
        char hbuf[2] = { (char)(hi < 10 ? '0'+hi : 'a'+hi-10), '\0' };
        char lbuf[2] = { (char)(lo < 10 ? '0'+lo : 'a'+lo-10), '\0' };
        kprint(hbuf); kprint(lbuf);
        if (i < 5) kprint(":");
    }
    kprint("\n");
}

/* =========================================================================
 * TX RING INITIALIZATION
 * =========================================================================*/

static int e1000_tx_init(void) {
    uint64_t phys_ring = 0;
    e1000.tx_descs = (volatile e1000_tx_desc_t *)e1000_dma_alloc_page(&phys_ring);
    if (e1000.tx_descs == NULL) {
        kprint("[e1000] ERROR: TX descriptor allocation failed\n");
        return -1;
    }

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        uint64_t phys_buf = 0;
        e1000.tx_bufs[i] = (uint8_t *)e1000_dma_alloc_page(&phys_buf);
        if (e1000.tx_bufs[i] == NULL) {
            kprint("[e1000] ERROR: TX buffer allocation failed\n");
            return -1;
        }

        e1000.tx_descs[i].addr    = phys_buf;
        e1000.tx_descs[i].length  = 0;
        e1000.tx_descs[i].cso     = 0;
        e1000.tx_descs[i].cmd     = 0;
        e1000.tx_descs[i].status  = E1000_TXSTA_DD; /* Mark as done (free) */
        e1000.tx_descs[i].css     = 0;
        e1000.tx_descs[i].special = 0;
    }
    e1000.tx_tail = 0;

    /* Program TX ring ke hardware */
    e1000_reg_write(E1000_REG_TDBAL, (uint32_t)(phys_ring & 0xFFFFFFFF));
    e1000_reg_write(E1000_REG_TDBAH, (uint32_t)(phys_ring >> 32));
    e1000_reg_write(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_reg_write(E1000_REG_TDH, 0);
    e1000_reg_write(E1000_REG_TDT, 0);

    /* TX Control:
     *   EN=1   : Enable transmitter
     *   PSP=1  : Pad Short Packets ke minimum 64 byte
     *   CT=15  : Collision threshold (QEMU: diabaikan, tapi tetap set)
     *   COLD=63: Full-duplex collision distance
     */
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (15U << E1000_TCTL_CT_SHIFT) |
                    (63U << E1000_TCTL_COLD_SHIFT);
    e1000_reg_write(E1000_REG_TCTL, tctl);

    /* Inter-Packet Gap: nilai standar dari datasheet untuk 802.3
     * IPGR=0x6, IPGR2=0x4, IPGT=0xA */
    e1000_reg_write(E1000_REG_TIPG, 0x0060200A);

    kprint("[e1000] TX ring initialized\n");
    return 0;
}

/* =========================================================================
 * RX RING INITIALIZATION
 * =========================================================================*/

static int e1000_rx_init(void) {
    /* Set MAC address ke Receive Address filter agar hanya paket untuk kita
     * yang diterima (+ broadcast yang dihandle oleh RCTL_BAM) */
    uint32_t ral = (uint32_t)e1000.mac[0] | ((uint32_t)e1000.mac[1] << 8) |
                   ((uint32_t)e1000.mac[2] << 16) | ((uint32_t)e1000.mac[3] << 24);
    uint32_t rah = (uint32_t)e1000.mac[4] | ((uint32_t)e1000.mac[5] << 8) |
                   (1U << 31); /* AV bit = Address Valid */
    e1000_reg_write(E1000_REG_RAL0, ral);
    e1000_reg_write(E1000_REG_RAH0, rah);

    uint64_t phys_ring = 0;
    e1000.rx_descs = (volatile e1000_rx_desc_t *)e1000_dma_alloc_page(&phys_ring);
    if (e1000.rx_descs == NULL) {
        kprint("[e1000] ERROR: RX descriptor allocation failed\n");
        return -1;
    }

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        uint64_t phys_buf = 0;
        e1000.rx_bufs[i] = (uint8_t *)e1000_dma_alloc_page(&phys_buf);
        if (e1000.rx_bufs[i] == NULL) {
            kprint("[e1000] ERROR: RX buffer allocation failed\n");
            return -1;
        }

        e1000.rx_descs[i].addr     = phys_buf;
        e1000.rx_descs[i].status   = 0; /* HW akan set DD=1 saat paket tiba */
        e1000.rx_descs[i].length   = 0;
        e1000.rx_descs[i].checksum = 0;
        e1000.rx_descs[i].errors   = 0;
        e1000.rx_descs[i].special  = 0;
    }
    /* Tail dimulai dari descriptor terakhir — kita serahkan semua ke HW */
    e1000.rx_tail = E1000_NUM_RX_DESC - 1;

    /* Program RX ring ke hardware */
    e1000_reg_write(E1000_REG_RDBAL, (uint32_t)(phys_ring & 0xFFFFFFFF));
    e1000_reg_write(E1000_REG_RDBAH, (uint32_t)(phys_ring >> 32));
    e1000_reg_write(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_reg_write(E1000_REG_RDH, 0);
    e1000_reg_write(E1000_REG_RDT, e1000.rx_tail);

    /* RX Control:
     *   EN=1   : Enable receiver
     *   BAM=1  : Accept broadcast (wajib untuk ARP)
     *   SECRC=1: Strip CRC (kita tidak mau 4 byte CRC di paket)
     *   BSIZE  : 2048 byte buffer (default, bits [17:16] = 00)
     *   UPE=1  : Unicast promiscuous (untuk debugging awal)
     *   MPE=1  : Multicast promiscuous
     */
    uint32_t rctl = E1000_RCTL_EN  | E1000_RCTL_BAM | E1000_RCTL_SECRC |
                    E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BSIZE_2048;
    e1000_reg_write(E1000_REG_RCTL, rctl);

    kprint("[e1000] RX ring initialized\n");
    return 0;
}

/* =========================================================================
 * PCI DISCOVERY
 *
 * Scan bus 0-3, slot 0-31 untuk menemukan e1000.
 * QEMU biasanya meletakkan e1000 di bus=0, slot=3 (tergantung QEMU version).
 * =========================================================================*/

static int e1000_pci_find(void) {
    for (uint8_t bus = 0; bus < 8; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            /* Baca Vendor ID (offset 0x00) */
            uint32_t id = pci_read32(bus, slot, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);

            if (vendor != E1000_VENDOR_ID) continue;
            if (device != E1000_DEVICE_82540EM &&
                device != E1000_DEVICE_82545EM &&
                device != E1000_DEVICE_82574L) continue;

            e1000.bus  = bus;
            e1000.slot = slot;
            e1000.func = 0;

            kprint("[e1000] Found at PCI ");
            kprint_num(bus); kprint(":"); kprint_num(slot); kprint(".0");
            kprint(" device=0x"); kprint_num(device); kprint("\n");
            return 0;
        }
    }
    return -1;
}

/* =========================================================================
 * e1000_init() — Entry Point
 * =========================================================================*/

int e1000_init(void) {
    kprint("[e1000] Initializing Intel 82540EM driver...\n");

    /* 1. Temukan e1000 di PCI bus */
    if (e1000_pci_find() != 0) {
        kprint("[e1000] ERROR: Device not found on PCI bus!\n");
        kprint("[e1000] Make sure QEMU is started with: -nic user,model=e1000\n");
        return -1;
    }

    /* 2. Aktifkan Bus Master + Memory Space di PCI */
    pci_enable_bus_master(e1000.bus, e1000.slot, e1000.func);

    /* 3. Baca BAR0 — MMIO base address */
    uint32_t bar0_low  = pci_read32(e1000.bus, e1000.slot, e1000.func, 0x10);
    uint32_t bar0_high = pci_read32(e1000.bus, e1000.slot, e1000.func, 0x14);

    /* BAR0 bit 0 = 0 (memory mapped), bit 2:1 = type (00=32bit, 10=64bit) */
    int bar_is_64bit = ((bar0_low & 0x6) == 0x4);
    uint64_t bar0_phys;
    if (bar_is_64bit) {
        bar0_phys = ((uint64_t)bar0_high << 32) | (bar0_low & ~0xFULL);
    } else {
        bar0_phys = (bar0_low & ~0xFULL);
    }

    /* 4. Map ke virtual address melalui HHDM */
    uint64_t bar0_virt = bar0_phys + hhdm_offset;
    e1000.mmio = (volatile uint32_t *)bar0_virt;

    kprint("[e1000] MMIO @ phys=0x"); kprint_num(bar0_phys);
    kprint(" virt=0x"); kprint_num(bar0_virt); kprint("\n");

    /* 5. Software Reset — bersihkan semua state hardware sebelumnya */
    uint32_t ctrl = e1000_reg_read(E1000_REG_CTRL);
    ctrl |= E1000_CTRL_RST;
    e1000_reg_write(E1000_REG_CTRL, ctrl);

    /* Tunggu hingga RST bit cleared (hardware clear-nya sendiri) */
    for (int i = 0; i < 100000; i++) {
        outb(0x80, 0); /* ~1µs delay */
        if (!(e1000_reg_read(E1000_REG_CTRL) & E1000_CTRL_RST)) break;
    }

    /* 6. Set Link Up + Auto-Speed Detection */
    ctrl = e1000_reg_read(E1000_REG_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    ctrl &= ~E1000_CTRL_LRST;
    ctrl &= ~E1000_CTRL_ILOS;
    ctrl &= ~E1000_CTRL_PHY_RST;
    e1000_reg_write(E1000_REG_CTRL, ctrl);

    /* 7. Baca MAC address dari EEPROM */
    e1000_read_mac();

    /* 8. Init TX dan RX rings */
    if (e1000_tx_init() != 0 || e1000_rx_init() != 0) {
        kprint("[e1000] ERROR: DMA ring initialization failed\n");
        return -1;
    }

    /* 9. Enable interrupts untuk RX (RXT0) dan Link Change (LSC) */
    e1000_reg_write(E1000_REG_IMC, 0xFFFFFFFF); /* Matikan semua dulu */
    e1000_reg_write(E1000_REG_IMS, E1000_ICR_RXT0 | E1000_ICR_LSC | E1000_ICR_RXDMT0);

    e1000.initialized = 1;
    kprint("[e1000] Initialization complete!\n");
    return 0;
}

/* =========================================================================
 * e1000_send() — Transmit satu Ethernet frame
 * =========================================================================*/

int e1000_send(const void *data, uint16_t len) {
    if (!e1000.initialized) return -1;
    if (len == 0 || len > 1522) return -1;

    uint16_t idx = e1000.tx_tail;
    volatile e1000_tx_desc_t *desc = &e1000.tx_descs[idx];

    /* Cek apakah slot ini bebas (DD bit = 1 artinya hardware sudah selesai) */
    if (!(desc->status & E1000_TXSTA_DD)) {
        /* TX ring penuh — tidak ada slot yang tersedia */
        kprint("[e1000] WARN: TX ring full\n");
        return -1;
    }

    /* Salin data ke TX buffer (hardware butuh physical address yang stabil) */
    memcpy(e1000.tx_bufs[idx], data, len);

    /* Bersihkan status sebelum submit ke hardware */
    desc->status = 0;
    desc->length = len;

    /* CMD bits:
     *   EOP  = ini adalah satu-satunya (dan terakhir) descriptor untuk frame ini
     *   IFCS = hardware yang insert FCS/CRC
     *   RS   = set DD bit di status saat selesai (kita perlu ini untuk flow control)
     */
    desc->cmd = E1000_TXCMD_EOP | E1000_TXCMD_IFCS | E1000_TXCMD_RS;

    /* Advance tail ke slot berikutnya */
    e1000.tx_tail = (uint16_t)((idx + 1) % E1000_NUM_TX_DESC);

    /* Pastikan buffer + descriptor terlihat oleh DMA sebelum TDT digeser. */
    __asm__ volatile("sfence" ::: "memory");

    /* Tulis TDT ke hardware — ini yang menandakan ada paket baru untuk dikirim */
    e1000_reg_write(E1000_REG_TDT, e1000.tx_tail);

    return 0;
}

/* =========================================================================
 * e1000_poll() — Periksa RX ring dan inject ke lwIP
 *
 * Dipanggil dari timer callback tiap PIT tick (~1ms).
 * Tidak menggunakan interrupt-driven RX agar tetap sederhana dan deterministik
 * dalam NO_SYS=1 mode.
 * =========================================================================*/

/*
 * Forward declaration untuk fungsi lwIP yang kita panggil.
 * Ini menghindari #include lwip/netif.h dan lwip/pbuf.h di sini.
 * Kita definisikan sebagai weak symbol stub yang akan di-override oleh
 * kyuzen_netif.c setelah lwIP terinisialisasi.
 */
extern void e1000_rx_callback(void *data, uint16_t len) __attribute__((weak));

void e1000_rx_callback(void *data, uint16_t len) {
    /* Default no-op stub — kyuzen_netif.c akan override dengan versi yang
     * memanggil netif->input() via pbuf */
    (void)data;
    (void)len;
}

void e1000_poll(void) {
    if (!e1000.initialized) return;

    /* Baca head dari hardware (HW advance head saat menerima paket) */
    uint16_t head = (uint16_t)(e1000_reg_read(E1000_REG_RDH) % E1000_NUM_RX_DESC);
    uint16_t tail = e1000.rx_tail;

    /* Proses semua descriptor antara tail+1 ... head */
    while (1) {
        uint16_t next = (uint16_t)((tail + 1) % E1000_NUM_RX_DESC);

        volatile e1000_rx_desc_t *desc = &e1000.rx_descs[next];

        /* Cek apakah descriptor ini sudah diisi oleh hardware */
        if (!(desc->status & E1000_RXSTA_DD)) break; /* Tidak ada paket baru */

        uint16_t pkt_len = desc->length;

        /* Hanya proses paket lengkap (EOP=1) dan tidak error */
        if ((desc->status & E1000_RXSTA_EOP) && desc->errors == 0 && pkt_len > 0) {
            /* Inject ke lwIP via callback — kyuzen_netif.c meng-override ini */
            e1000_rx_callback(e1000.rx_bufs[next], pkt_len);
        }

        /* Kembalikan descriptor ke hardware: clear status, pertahankan buffer addr */
        desc->status = 0;
        desc->length = 0;
        desc->errors = 0;

        /* Advance tail — ini memberi tahu hardware bahwa descriptor siap dipakai lagi */
        tail = next;
        e1000_reg_write(E1000_REG_RDT, tail);
    }
    e1000.rx_tail = tail;
}

/* =========================================================================
 * PUBLIC HELPERS
 * =========================================================================*/

void e1000_get_mac(uint8_t out[6]) {
    if (!e1000.initialized) {
        memset(out, 0, 6);
        return;
    }
    memcpy(out, e1000.mac, 6);
}

int e1000_link_up(void) {
    if (!e1000.initialized) return 0;
    uint32_t status = e1000_reg_read(E1000_REG_STATUS);
    return (status & (1U << 1)) ? 1 : 0; /* Bit 1 = LU (Link Up) */
}
