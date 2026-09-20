// drivers/acpi.c — ACPI minimal: temukan FADT untuk power-off S5.
//
// DIPAKAI SIAPA
//   Aksi [S] (shutdown) di layar panic (kernel/panic/panic_hw.c) — di QEMU/Bochs/VBox
//   port fallback biasanya sudah cukup, tapi di PC nyata power-off butuh
//   alamat blok kontrol PM1a dari FADT.
//
// ALUR
//   RSDP (dari limine_rsdp_request, fisik) → RSDT/XSDT → tabel "FACP" (FADT)
//   → PM1a_CNT_BLK (offset 0x40) + PM1b_CNT_BLK (0x44) + PM1_CNT_LEN (0x59).
//
// KENAPA REPUBLIKASI >= 2 SAJA: offset field FADT ACPI 1.0 berbeda (blok PM
// mulai 0x30, bukan 0x38). Menebak offset yang salah = menulis ke port I/O
// acak, jadi tabel rev < 2 sengaja DILEWATI (fallback port emulator tetap jalan).
//
// SLP_TYPa/b dibaca dari objek _S5 di DSDT (lihat acpi.h). Sumber yang benar
// adalah package _S5; SLP_TYP konvensional 5 hanya fallback.
//
// Semua akses tabel memakai pembacaan byte-wise (aman untuk alamat tidak
// selaras) dan TANPA alokasi dinamis.

#include "acpi.h"
#include "io.h"
#include "limine.h"
#include <stdint.h>

extern uint64_t hhdm_offset;

__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

static acpi_pm_t g_pm  = { 0, 0, 5, 5, 0, 0, 0 };
static int       g_ok  = 0;

// Batas bawah higher-half: alamat di atas ini dianggap sudah virtual.
#define ACPI_HIGHER_HALF_MIN 0x0000800000000000ull

const void* acpi_resolve_rsdp(void* raw, uint64_t hhdm) {
    if (!raw) return 0;
    uintptr_t p = (uintptr_t)raw;
    if (p >= ACPI_HIGHER_HALF_MIN) return (const void*)p;      // sudah virtual
    return (const void*)(uintptr_t)(p + hhdm);                 // alamat fisik
}

// =====================================================================
// Pembacaan tabel (byte-wise, little-endian)
// =====================================================================
static uint8_t  rd8 (const uint8_t* p, uint32_t off) { return p[off]; }
static uint32_t rd32(const uint8_t* p, uint32_t off) {
    return (uint32_t)p[off]
         | ((uint32_t)p[off + 1] << 8)
         | ((uint32_t)p[off + 2] << 16)
         | ((uint32_t)p[off + 3] << 24);
}
static uint64_t rd64(const uint8_t* p, uint32_t off) {
    return (uint64_t)rd32(p, off) | ((uint64_t)rd32(p, off + 4u) << 32);
}
static int sig4(const uint8_t* p, const char* s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}
static int sig8(const uint8_t* p, const char* s) {
    for (int i = 0; i < 8; i++) if (p[i] != (uint8_t)s[i]) return 0;
    return 1;
}
// Tabel ACPI: jumlah seluruh byte (termasuk checksum) harus 0 mod 256.
static int sum_ok(const uint8_t* p, uint32_t len) {
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++) s = (uint8_t)(s + p[i]);
    return s == 0;
}

// =====================================================================
// _S5 statis di DSDT (murni, host-testable)
//
// _S5 di firmware nyata hampir selalu Name statis (bukan Method), bentuknya:
//   NameOp(0x08) "_S5_" PackageOp(0x12) PkgLength NumElements Elt[0] Elt[1]
// Elemen adalah konstanta integer AML: ZeroOp(0x00), OneOp(0x01),
// BytePrefix(0x0A)+u8, WordPrefix(0x0B)+u16, DWordPrefix(0x0C)+u32.
// Nilai umum di UEFI modern: {7,7}. Tanpa interpreter AML pun ini cukup —
// yang dipindai hanya pola Name+Package, bukan eksekusi bytecode.
// =====================================================================
static int s5_const(const uint8_t* p, uint32_t dlen, uint32_t* cur,
                    uint32_t end, uint8_t* v) {
    if (*cur >= end || *cur >= dlen) return 0;
    uint8_t op = p[*cur];
    if (op == 0x00) { *v = 0; (*cur)++; return 1; }          // ZeroOp
    if (op == 0x01) { *v = 1; (*cur)++; return 1; }          // OneOp
    if (op == 0x0A && *cur + 1u < end && *cur + 1u < dlen) { // ByteConst
        *v = p[*cur + 1u]; *cur += 2u; return 1;
    }
    if (op == 0x0B && *cur + 2u < end && *cur + 2u < dlen) { // WordConst
        *v = (uint8_t)(p[*cur + 1u] & 0x07u); *cur += 3u; return 1;
    }
    if (op == 0x0C && *cur + 4u < end && *cur + 4u < dlen) { // DWordConst
        *v = (uint8_t)(p[*cur + 1u] & 0x07u); *cur += 5u; return 1;
    }
    return 0;
}

int acpi_parse_s5(const uint8_t* dsdt, uint32_t dlen, uint8_t* a, uint8_t* b) {
    if (!dsdt || !a || !b || dlen < 40u) return 0;
    if (!(dsdt[0] == 'D' && dsdt[1] == 'S' && dsdt[2] == 'D' && dsdt[3] == 'T'))
        return 0;
    // Batas pindai = panjang tabel yang dinyatakan (dibatasi dlen).
    uint32_t tlen = (uint32_t)dsdt[4]
                  | ((uint32_t)dsdt[5] << 8)
                  | ((uint32_t)dsdt[6] << 16)
                  | ((uint32_t)dsdt[7] << 24);
    if (tlen < 40u || tlen > dlen) return 0;

    for (uint32_t i = 36u; i + 8u < tlen; i++) {
        if (dsdt[i] != 0x08) continue;                       // NameOp
        if (!(dsdt[i + 1] == '_' && dsdt[i + 2] == 'S' &&
              dsdt[i + 3] == '5' && dsdt[i + 4] == '_')) continue;
        uint32_t cur = i + 5u;
        if (dsdt[cur] != 0x12) continue;                     // PackageOp
        cur++;
        // PkgLength: 4 bit rendah + N byte lanjutan (2 bit tinggi).
        if (cur >= tlen) continue;
        uint8_t lead = dsdt[cur++];
        uint32_t nfol = (uint32_t)((lead >> 6) & 0x03u);
        if (cur + nfol > tlen) continue;
        uint32_t pkglen = lead & 0x0Fu;
        for (uint32_t k = 0; k < nfol; k++)
            pkglen |= (uint32_t)dsdt[cur++] << (4u + 8u * k);
        // cur kini di awal konten; akhir eksklusif = awal konten + pkglen.
        uint32_t pkg_end = cur + pkglen;
        if (pkg_end > tlen || pkg_end <= cur) continue;
        if (cur >= pkg_end) continue;
        uint8_t nelem = dsdt[cur++];                         // NumElements
        if (nelem < 2u) continue;
        uint8_t va = 0, vb = 0;
        if (!s5_const(dsdt, dlen, &cur, pkg_end, &va)) continue;
        if (!s5_const(dsdt, dlen, &cur, pkg_end, &vb)) continue;
        *a = va; *b = vb;
        return 1;
    }
    return 0;
}

// =====================================================================
// Parser FADT murni (dipakai host test juga)
// =====================================================================
int acpi_parse_rsdp(const void* rsdp_v, uint64_t hhdm, acpi_pm_t* out) {
    if (!rsdp_v || !out) return 0;
    const uint8_t* rsdp = (const uint8_t*)rsdp_v;

    if (!sig8(rsdp, "RSD PTR ")) return 0;
    if (!sum_ok(rsdp, 20)) return 0;                 // checksum RSDP 1.0

    uint8_t  rev       = rd8(rsdp, 15);
    uint32_t rsdt_phys = rd32(rsdp, 16);
    uint64_t xsdt_phys = 0;
    if (rev >= 2) {                                  // RSDP 2.0+: XSDT + len
        uint32_t len = rd32(rsdp, 20);
        if (len >= 36 && len <= 4096 && !sum_ok(rsdp, len)) return 0;
        xsdt_phys = rd64(rsdp, 24);
    }

    // Pilih XSDT (entri 64-bit) kalau ada, kalau tidak RSDT (entri 32-bit).
    const uint8_t* tbl = 0;
    int entry64 = 0;
    if (xsdt_phys) { tbl = (const uint8_t*)(uintptr_t)(hhdm + xsdt_phys); entry64 = 1; }
    else if (rsdt_phys) { tbl = (const uint8_t*)(uintptr_t)(hhdm + rsdt_phys); }
    if (!tbl) return 0;
    if (entry64 ? !sig4(tbl, "XSDT") : !sig4(tbl, "RSDT")) return 0;

    uint32_t tlen = rd32(tbl, 4);
    if (tlen < 36 || tlen > (1u << 20)) return 0;
    if (!sum_ok(tbl, tlen)) return 0;

    uint32_t esize = entry64 ? 8u : 4u;
    uint32_t count = (tlen - 36u) / esize;
    if (count > 64u) count = 64u;                    // batas scan

    for (uint32_t i = 0; i < count; i++) {
        uint64_t phys = entry64 ? rd64(tbl, 36u + i * 8u)
                                : (uint64_t)rd32(tbl, 36u + i * 4u);
        if (!phys) continue;
        const uint8_t* t = (const uint8_t*)(uintptr_t)(hhdm + phys);
        if (!sig4(t, "FACP")) continue;

        uint32_t flen = rd32(t, 4);
        if (flen < 0x5Au || flen > 4096u) continue;  // harus mencakup 0x59
        if (rd8(t, 8) < 2) continue;                 // layout ACPI 1.0: lewati
        if (!sum_ok(t, flen)) continue;

        uint16_t pm1a = (uint16_t)(rd32(t, 0x40) & 0xFFFFu);
        uint16_t pm1b = (uint16_t)(rd32(t, 0x44) & 0xFFFFu);
        uint8_t  clen = rd8(t, 0x59);
        if (pm1a < 0x400u || clen < 2u) continue;    // port tidak masuk akal

        out->pm1a_cnt = pm1a;
        out->pm1b_cnt = (pm1b >= 0x400u) ? pm1b : 0;
        out->slp_typa = 5;                          // fallback konvensi S5
        out->slp_typb = 5;
        out->has_reset = 0;
        out->reset_port = 0;
        out->reset_value = 0;

        // RESET_REG (GAS @0x74, RESET_VALUE @0x80, butuh flen >= 0x81):
        // hanya SystemIO 8-bit yang dipakai. Nilai reset generik (biasanya
        // port 0xCF9, value 0x06) — cara reset standar ACPI, dicoba pertama
        // dalam rantai reboot sebelum 0xCF9 hardcoded/8042/triple-fault.
        if (flen >= 0x81u && rd8(t, 0x74) == 1 && rd8(t, 0x75) == 8) {
            uint16_t rport = (uint16_t)(rd64(t, 0x78) & 0xFFFFu);
            if (rport >= 0x400u || rport == 0xCF9u) {
                out->reset_port  = rport;
                out->reset_value = rd8(t, 0x80);
                out->has_reset   = 1;
            }
        }

        // _S5 dari DSDT: X_DSDT (@0x8C, butuh flen >= 0x94) diutamakan,
        // DSDT 32-bit (@0x28) sebagai cadangan. Gagal validasi = fallback 5.
        uint64_t dsdt_phys = 0;
        if (flen >= 0x94u) dsdt_phys = rd64(t, 0x8C);
        if (!dsdt_phys) dsdt_phys = rd32(t, 0x28);
        if (dsdt_phys && dsdt_phys < 0x100000000ull) {
            const uint8_t* d = (const uint8_t*)(uintptr_t)(hhdm + dsdt_phys);
            // Panjang dinyatakan DSDT, dibatasi 1MB supaya pemindai berhenti.
            uint32_t dlen = rd32(d, 4);
            if (dlen >= 40u && dlen <= (1u << 20) && sum_ok(d, dlen)) {
                uint8_t sa = 5, sb = 5;
                if (acpi_parse_s5(d, dlen, &sa, &sb)) {
                    out->slp_typa = (uint8_t)(sa & 0x07u);
                    out->slp_typb = (uint8_t)(sb & 0x07u);
                }
            }
        }
        return 1;
    }
    return 0;
}

// =====================================================================
// Jalur kernel
// =====================================================================
void acpi_early_init(void) {
    g_ok = 0;
    if (!rsdp_request.response || !rsdp_request.response->address) return;

    // PENTING — alamat dari Limine sudah VIRTUAL (lihat acpi_resolve_rsdp()):
    // menambah hhdm_offset lagi menghasilkan pointer non-kanonik → #GP(0) pada
    // byte pertama RSDP. Tabel di dalam (entri XSDT/RSDT dan FADT) tetap alamat
    // FISIK sesuai spesifikasi ACPI, jadi parser masih menerima hhdm.
    const void* rsdp = acpi_resolve_rsdp(rsdp_request.response->address, hhdm_offset);

    acpi_pm_t pm = { 0, 0, 5, 5, 0, 0, 0 };
    if (!acpi_parse_rsdp(rsdp, hhdm_offset, &pm)) return;

    g_pm = pm;
    g_ok = 1;
}

int acpi_poweroff_raw(void) {
    if (!g_ok || !g_pm.pm1a_cnt) return 0;
    // PM1_CNT: bit 10-12 = SLP_TYP, bit 13 = SLP_EN.
    outw(g_pm.pm1a_cnt, (uint16_t)(((uint16_t)g_pm.slp_typa << 10) | 0x2000u));
    if (g_pm.pm1b_cnt)
        outw(g_pm.pm1b_cnt, (uint16_t)(((uint16_t)g_pm.slp_typb << 10) | 0x2000u));
    return 1;
}

int acpi_reset_raw(void) {
    if (!g_ok || !g_pm.has_reset || !g_pm.reset_port) return 0;
    outb(g_pm.reset_port, g_pm.reset_value);
    return 1;
}

uint16_t acpi_pm1a_cnt_port(void) { return g_pm.pm1a_cnt; }
