#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

// =====================================================================
// ACPI minimal (drivers/acpi.c) — BUKAN interpreter AML.
//
// Tujuannya satu: power-off (S5) yang benar di hardware nyata untuk aksi [S]
// di layar panic. Alur: RSDP (dari Limine) → RSDT/XSDT → FADT ("FACP") →
// mencari alamat blok kontrol PM1a/PM1b.
//
// Batasan yang disengaja:
//   * Hanya layout FADT revision >= 2 yang dipakai (offset PM1a_CNT_BLK = 0x40,
//     PM1_CNT_LEN = 0x59). FADT ACPI 1.0 punya offset berbeda dan sengaja
//     DILEWATI — lebih baik tidak power-off daripada menulis ke port salah.
//   * SLP_TYPa/b = 5 (nilai S5 yang konvensional). Sumber idealnya objek _S5
//     di DSDT, yang butuh interpreter AML — di luar cakupan.
//   * Tanpa alokasi dinamis; semua data di static/stack.
// =====================================================================

// Hasil parse FADT (port I/O + tipe S5).
typedef struct acpi_pm {
    uint16_t pm1a_cnt;   // 0 = tidak diketahui
    uint16_t pm1b_cnt;   // 0 = tidak ada
    uint8_t  slp_typa;
    uint8_t  slp_typb;
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

// Kirim SLP_TYPa|SLP_EN ke PM1a_CNT (dan PM1b kalau ada).
// Return 1 kalau perintah terkirim, 0 kalau ACPI tidak tersedia.
int acpi_poweroff_raw(void);

uint16_t acpi_pm1a_cnt_port(void);   // 0 = tidak diketahui

#endif
