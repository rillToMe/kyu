#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

// =====================================================================
// ACPI minimal (drivers/acpi.c) — BUKAN interpreter AML.
//
// Tujuannya satu: power-off (S5) yang benar di hardware nyata untuk aksi [S]
// di layar panic, plus reset hardware standar untuk reboot. Alur: RSDP (dari
// Limine) → RSDT/XSDT → FADT ("FACP") → blok kontrol PM1a/PM1b + RESET_REG.
//
// Offset FADT diverifikasi terhadap ACPICA actbl.h (struct acpi_table_fadt):
// DSDT=0x28, PM1a_CNT_BLK=0x40, PM1b_CNT_BLK=0x44, PM1_CNT_LEN=0x59,
// RESET_REG(GAS,12B)=0x74, RESET_VALUE=0x80, X_DSDT=0x8C. Hanya layout FADT
// revision >= 2 yang dipakai — FADT ACPI 1.0 punya offset berbeda dan sengaja
// DILEWATI (lebih baik tidak power-off daripada menulis ke port salah).
//
// SLP_TYPa/b dibaca dari objek _S5 di DSDT (Name statis — cukup pemindai pola,
// tanpa interpreter AML); kalau tidak ketemu, fallback 5 (konvensi S5).
// RESET_REG hanya dipakai bila GAS-nya SystemIO 8-bit (port I/O); varian
// MemoryMapped/PciConfig dilewati (tanpa pemetaan MMIO di modul ini).
//
// Tanpa alokasi dinamis; semua data di static/stack.
// =====================================================================

// Hasil parse FADT (port I/O + tipe S5 + register reset).
typedef struct acpi_pm {
    uint16_t pm1a_cnt;   // 0 = tidak diketahui
    uint16_t pm1b_cnt;   // 0 = tidak ada
    uint8_t  slp_typa;
    uint8_t  slp_typb;
    uint16_t reset_port; // valid bila has_reset = 1
    uint8_t  reset_value;
    uint8_t  has_reset;
} acpi_pm_t;

// Parser murni — bisa diuji di host (lihat test/panic_test.c): `rsdp` adalah
// pointer ke RSDP (sudah di-HHDM-kan), `hhdm` offset untuk mengakses tabel
// fisik berikutnya. Return 1 kalau FADT valid ditemukan.
int acpi_parse_rsdp(const void* rsdp, uint64_t hhdm, acpi_pm_t* out);

// Ubah alamat RSDP mentah dari bootloader menjadi pointer yang bisa dibaca.
//
// Limine memberi alamat VIRTUAL (sudah di-HHDM), jadi menambah hhdm_offset
// lagi = salah dua kali → pointer non-kanonik → #GP saat byte pertama dibaca
// (persis bug yang pernah terjadi di QEMU BIOS: 0xFFFF8000_000F52E0 +
// 0xFFFF8000_00000000 = 0xFFFF0000_000F52E0). Fungsi ini memakai nilai apa
// adanya bila sudah di higher-half, dan hanya menambah hhdm_offset kalau
// bootloader memberi alamat fisik. Dipisah supaya bisa diuji di host.
const void* acpi_resolve_rsdp(void* raw, uint64_t hhdm);

// Versi kernel: ambil RSDP dari Limine (limine_rsdp_request) dan simpan hasilnya.
void acpi_early_init(void);

// Pindai objek _S5 statis di DSDT (NameOp 0x08 "_S5_" + PackageOp 0x12 +
// 2 konstanta integer). Murni, bisa diuji di host. Return 1 bila ketemu
// (isi *a*B), 0 bila tidak (panggilBur fallback ke konvensi 5).
int acpi_parse_s5(const uint8_t* dsdt, uint32_t dlen, uint8_t* a, uint8_t* b);

// Kirim SLP_TYPa|SLP_EN ke PM1a_CNT (dan PM1b kalau ada).
// Return 1 kalau perintah terkirim, 0 kalau ACPI tidak tersedia.
int acpi_poweroff_raw(void);

// Tulis RESET_VALUE ke RESET_REG (cara reset standar ACPI, dicoba pertama
// dalam rantai reboot). Return 1 kalau terkirim, 0 kalau tidak tersedia.
int acpi_reset_raw(void);

uint16_t acpi_pm1a_cnt_port(void);   // 0 = tidak diketahui

#endif
