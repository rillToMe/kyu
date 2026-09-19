#ifndef PCI_H
#define PCI_H

#include <stdint.h>

// Struktur untuk menampung info perangkat yang terdeteksi
typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
} pci_device_t;

// Deklarasi fungsi utama
uint32_t pci_read_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);
void pci_probe(void);

void acpi_poweroff(void);
void system_reboot(void);
#endif