#include "pci.h"
#include "io.h"
#include "acpi.h"   // acpi_poweroff_raw()/acpi_reset_raw() (FADT asli)

// Pinjam fungsi kprint dari kernel untuk nge-log ke layar
extern void kprint(const char* str);
extern void print_hex(uint32_t num); 

// Fungsi inti untuk membaca register hardware PCI
uint32_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    uint32_t lbus  = (uint32_t)bus;
    uint32_t lslot = (uint32_t)slot;
    uint32_t lfunc = (uint32_t)func;
    
    // Format alamat standar PCI Configuration Space
    address = (uint32_t)((lbus << 16) | (lslot << 11) | (lfunc << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    outl(0xCF8, address);
    return (inl(0xCFC) >> ((offset & 2) * 8)) & 0xFFFF;
}

// Baca 32-bit dari PCI Configuration Space
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((uint32_t)bus << 16) | (slot << 11) |
                       (func << 8) | (offset & 0xFC) | 0x80000000;
    outl(0xCF8, address);
    return inl(0xCFC);
}

// Tulis 32-bit ke PCI Configuration Space
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((uint32_t)bus << 16) | (slot << 11) |
                       (func << 8) | (offset & 0xFC) | 0x80000000;
    outl(0xCF8, address);
    outl(0xCFC, val);
}

// Mengecek apakah di slot ini ada perangkat yang ditancapkan
void pci_check_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor_id = pci_read_word(bus, slot, func, 0);
    if (vendor_id == 0xFFFF) return; // 0xFFFF = Kosong/Tidak ada hardware

    uint16_t device_id = pci_read_word(bus, slot, func, 2);
    uint32_t class_info = pci_read_word(bus, slot, func, 0x0A);
    uint8_t class_code = (class_info >> 8) & 0xFF;
    uint8_t subclass = class_info & 0xFF;

    // --- Cetak Log Hardware ---
    kprint("PCI: "); print_hex(vendor_id); kprint(":"); print_hex(device_id);
    kprint(" | Class: "); print_hex(class_code);
    
    // Deteksi jenis perangkat
    if (class_code == 0x01 && subclass == 0x01) kprint(" [IDE/SATA Controller]");
    else if (class_code == 0x03) kprint(" [VGA Display]");
    else if (class_code == 0x02) kprint(" [Network Card (LAN)]");
    else if (class_code == 0x06 && subclass == 0x80) kprint(" [Power Management]");
    
    kprint("\n");
}

// Menyalakan Radar PCI
void pci_probe(void) {
    kprint("\n--- Scanning Hardware (PCI Bus) ---\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                pci_check_device((uint8_t)bus, slot, func);
            }
        }
    }
    kprint("--- Selesai Scanning ---\n");
}

// Fungsi Sakti untuk Matiin PC (Power Off)
void acpi_poweroff(void) {
    kprint("\nMenyimpan state dan mematikan OS...\n");

    // KyuzenFS V4: flush seluruh block dirty + metadata sebelum listrik
    // hilang (write-back cache boleh saja belum mem-flush).
    extern void kfs_sync_all(void);
    kfs_sync_all();

    // 1. ACPI S5 dari FADT asli (hardware nyata: SLP_TYP dari _S5 + PM1_CNT
    //    dari firmware). Kalau mesin mati di sini, tidak ada yang kembali.
    (void)acpi_poweroff_raw();

    // 2. Port standar ACPI emulator (QEMU, Bochs, VirtualBox).
    outw(0xB004, 0x2000); // Bochs / versi QEMU lama
    outw(0x604, 0x2000);  // QEMU modern
    outw(0x4004, 0x3400); // VirtualBox

    // Jika gagal mati (misal FADT tak terparse di PC fisik), bekukan mesin.
    __asm__ volatile("cli; hlt");
    while(1);
}

// Fungsi Sakti untuk Restart PC (Reboot)
//
// Rantai reset standar industri (setiap tahap jatuh ke berikutnya bila mesin
// masih hidup — reset yang berhasil tidak pernah kembali):
//   1. RESET_REG FADT (nilai generik firmware, biasanya 0xCF9=0x06)
//   2. Chipset Reset Control 0xCF9 (0x02 lalu 0x06 = SYS_RST|RST_CPU)
//   3. Keyboard controller 8042 (0xFE), tunggu BERBATAS (laptop tanpa 8042
//      membaca 0xFF selamanya — tanpa batas = hang sebelum triple fault)
//   4. Triple fault (IDT kosong + int3)
void system_reboot(void) {
    kprint("\nMerestart OS...\n");

    // KyuzenFS V4: flush data sebelum reset.
    extern void kfs_sync_all(void);
    kfs_sync_all();

    // 1. Cara standar ACPI (lihat drivers/acpi.c untuk asal nilai).
    (void)acpi_reset_raw();

    // 2. Chipset Reset Control Register.
    outb(0xCF9, 0x02);
    for (volatile uint32_t d = 0; d < 1000u; d++) (void)inb(0x80); // ~1ms
    outb(0xCF9, 0x06);
    for (volatile uint32_t d = 0; d < 1000u; d++) (void)inb(0x80);

    // 3. Keyboard controller 8042 — kuras BERBATAS lalu tembak 0xFE.
    for (uint32_t guard = 0; guard < 100000u; guard++) {
        uint8_t temp = inb(0x64);
        if ((temp & 0x01) != 0) (void)inb(0x60);
        if ((temp & 0x02) == 0) break;
    }
    outb(0x64, 0xFE); // Tembak perintah Reset!

    // 4. Cara Kasar (Fallback): pancing "Triple Fault"
    // Ini akan sengaja membuat error fatal pada CPU sehingga CPU otomatis me-reboot PC.
    __asm__ volatile ("cli"); // Matikan interupsi
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr = {0, 0};
    __asm__ volatile ("lidt %0" : : "m"(idtr)); // Hancurkan tabel interupsi (IDT)
    __asm__ volatile ("int $3"); // Pancing interupsi

    // Bekukan jika masih gagal
    while (1) __asm__ volatile("hlt");
}