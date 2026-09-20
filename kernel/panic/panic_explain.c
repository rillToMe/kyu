// kernel/panic/panic_explain.c — mesin diagnostik BSOD.
//
// TANGGUNG JAWAB
//   * Nama exception (vektor 0..14).
//   * panic_explain(): menerjemahkan (vektor, error code, CR2, RIP) menjadi
//     VERDICT + petunjuk tindakan. Ini bagian yang paling berguna saat membaca
//     BSOD, jadi selalu terisi — bukan sekadar dump angka.
//   * Klasifikasi RIP (kernel text / di luar kernel text / ring-3).
//   * Backtrace: telusuri RBP chain atau scan stack, untuk layar (p_backtrace)
//     maupun untuk payload crashdump (panic_bt_collect).
//
// ATURAN ALAMAT (penting, harga mahal)
//   Jalur panic hanya boleh MEMBACA alamat higher-half. Fault aplikasi user
//   (ring 3) meninggalkan RSP/RBP di alamat USER (mis. RSP=0x0BFBFFB8);
//   membacanya dari ring 0 saat SMAP aktif menghasilkan #PF KEDUA → guard
//   panic bersarang → layar BSOD terpotong di baris BACKTRACE lalu sistem
//   freeze. Jadi jangan pernah dereference alamat di bawah higher-half dari
//   sini, sekalipun paging_is_mapped_nolock() menilainya "mapped" (PML4 proses
//   memang memetakan alamat user — penilaian itu benar, tapi tetap tidak boleh
//   dibaca).
//
// KONTRAK: tanpa heap/lock. Hanya varian _nolock yang boleh dipanggil.

#include <stdint.h>

#include "panic_internal.h"

// =====================================================================
// 1. NAMA EXCEPTION & VERDICT
// =====================================================================
const char* const exception_names[15] = {
    "Divide by Zero",         // 0
    "Debug",                  // 1
    "NMI",                    // 2
    "Breakpoint",             // 3
    "Overflow",               // 4
    "Bound Range Exceeded",   // 5
    "INVALID OPCODE",         // 6
    "Device Not Available",   // 7
    "DOUBLE FAULT",           // 8
    "Coprocessor Overrun",    // 9
    "Invalid TSS",            // 10
    "Segment Not Present",    // 11
    "STACK-SEGMENT FAULT",    // 12
    "GENERAL PROTECTION",     // 13
    "PAGE FAULT",             // 14
};

int panic_is_canonical(uint64_t a) {
    return a < 0x0000800000000000ull || a >= 0xFFFF800000000000ull;
}

int panic_in_kernel(uint64_t rip) { return rip >= KERNEL_VMA; }

// Terjemahkan fault menjadi sebab + tindakan.
panic_cause_t panic_explain(uint64_t intno, uint64_t err, uint64_t cr2, uint64_t rip) {
    panic_cause_t c = { "Tidak dapat disimpulkan otomatis",
                        "lihat BACKTRACE + register untuk konteks pemanggil",
                        "(rincian teknis ada di bagian DETAIL di bawah)" };

    if (intno == 14) {                                    // PAGE FAULT
        if (!panic_is_canonical(cr2)) {
            c.verdict = "ALAMAT NON-KANONIKAL - pointer korup / cast salah";
            c.hint1 = "(nilai 32-bit tanpa sign-extend, atau stack rusak)";
            c.hint2 = "return address / function pointer menunjuk ke data";
        } else if (err & 0x10) {                          // instruction fetch
            c.verdict = "FETCH INSTRUKSI dari halaman tanpa izin exec / belum termap";
            c.hint1 = "return address / function pointer menunjuk ke data";
            c.hint2 = "lihat BACKTRACE + register untuk konteks pemanggil";
        } else if (err & 0x08) {                          // reserved bit PTE
            c.verdict = "ENTRY PAGE-TABLE RESERVED RUSAK (bit RSVD)";
            c.hint1 = "korupsi struktur page table";
            c.hint2 = 0;
        } else if (!(err & 0x01)) {                        // halaman belum termap
            if (err & 0x04) {
                c.verdict = "APLIKASI user mengakses alamat BELUM TERMAP";
                c.hint1 = "kemungkinan stack overflow (periksa kedalaman rekursi)";
                c.hint2 = "heap/stack overflow, atau pointer ilegal dari app";
            } else {
                c.verdict = "KERNEL mengakses alamat yang BELUM TERMAP";
                c.hint1 = "kemungkinan dereference pointer NULL / liar";
                c.hint2 = 0;
            }
        } else if (err & 0x02) {                           // present + write
            if (err & 0x04) {
                c.verdict = "PROTECTION VIOLATION pada akses user";
                c.hint1 = "app menulis area read-only (mis. .rodata / kode)";
                c.hint2 = 0;
            } else {
                c.verdict = "KERNEL MENULIS ke halaman READ-ONLY";
                c.hint1 = "umumnya tulis ke .text/.rodata ELF (CoW belum di-copy?)";
                c.hint2 = 0;
            }
        } else {                                           // present + read
            c.verdict = (err & 0x04) ? "PROTECTION VIOLATION pada akses user"
                                     : "PROTECTION VIOLATION (halaman ada, akses ditolak)";
            c.hint1 = "lihat BACKTRACE + register untuk konteks pemanggil";
            c.hint2 = 0;
        }
    } else if (intno == 13) {                              // #GP
        if (err == 0) {
            c.verdict = "GPF dengan selector 0 - eksekusi instruksi privileged";
            c.hint1 = "null selector atau alamat non-kanonik";
            c.hint2 = "periksa GDT/IDT atau segment register yang korup";
        } else {
            c.verdict = "GPF pada selector/segmen tidak sah";
            c.hint1 = "periksa GDT/IDT atau segment register yang korup";
            c.hint2 = "atau alamat non-kanonik di segmen/selector tak sah";
        }
    } else if (intno == 0) {
        c.verdict = "DIVIDE BY ZERO di kernel";
        c.hint1 = "periksa pembagi yang berasal dari input luar";
        c.hint2 = 0;
    } else if (intno == 6) {
        c.verdict = "INVALID OPCODE - kode korup atau instruksi tak didukung";
        c.hint1 = "kernel dibangun -mno-sse: instruksi SSE = kode/mismatch";
        c.hint2 = 0;
    } else if (intno == 8) {
        c.verdict = "DOUBLE FAULT - handler fault sebelumnya gagal";
        c.hint1 = "korupsi struktur page table";
        c.hint2 = "biasanya stack (RSP) rusak / tidak valid saat handler jalan";
    } else if (intno == 11 || intno == 12) {
        c.verdict = "STACK/SEGMENT tidak present - RSP atau SS rusak";
        c.hint1 = "biasanya stack (RSP) rusak / tidak valid saat handler jalan";
        c.hint2 = 0;
    } else {
        c.verdict = "Exception tidak tertangani di kernel";
        c.hint1 = "lihat BACKTRACE + register untuk konteks pemanggil";
        c.hint2 = "dump stack trace lengkap ada di serial COM1 (-serial stdio)";
    }

    // RIP di luar kernel text = lompatan ke pointer tak sah; ini keterangan
    // paling berguna, jadi ditampilkan walau sudah ada hint lain.
    if (!panic_in_kernel(rip)) {
        c.hint2 = "RIP di LUAR kernel text saat CPL=0: kernel melompat ke pointer tak sah";
    }
    return c;
}

// Klasifikasi RIP terhadap kernel text & CPL pemanggil.
const char* panic_rip_class(uint64_t rip, uint64_t cs) {
    if ((cs & 3u) == 3u) return "   RING-3 (aplikasi user)";
    if (panic_in_kernel(rip)) return "   KERNEL TEXT";
    return "   DI LUAR KERNEL TEXT - lompat ke pointer tak sah";
}

uint32_t panic_rip_class_color(uint64_t rip, uint64_t cs) {
    if ((cs & 3u) == 3u) return C_KEY;
    return panic_in_kernel(rip) ? C_OK : C_TITLE;
}

// =====================================================================
// 2. BACKTRACE
// =====================================================================
static uint32_t p_trace_count = 0;

static int panic_ptr_readable(uint64_t a) {
    return panic_is_canonical(a) && a >= PANIC_HIGHER_HALF;
}

static void p_trace_addr(uint64_t addr) {
    p_cont_begin();
    p_str("  #", C_FAINT);
    p_dec(p_trace_count, C_FAINT);
    if (addr >= KERNEL_VMA) {
        p_str(" KERNEL+", C_DIM);
        p_hex(addr - KERNEL_VMA, C_FG);
    } else {
        p_str(" NONKERNEL ", C_DIM);
        p_hex(addr, C_FG);
    }
    p_trace_count++;
}

void p_backtrace(uint64_t rbp, uint64_t rsp, uint64_t cs) {
    p_label("BACKTRACE");
    p_trace_count = 0;

    // Fault dari ring 3: stack yang aktif milik aplikasi user. Menelusuri
    // alamat user dari kernel tidak menghasilkan frame yang berguna DAN
    // melanggar SMAP, jadi lewati secara eksplisit (bukan coba-coba baca).
    if ((cs & 3u) == 3u) {
        p_str("(konteks user - backtrace kernel dilewati, stack di alamat user)", C_FAINT);
        p_newline();
        return;
    }

    // 1) RBP chain (kalau frame pointer tersimpan; -O2 sering menghilangkannya).
    uint64_t frame = rbp;
    for (uint32_t depth = 0; depth < 6u; depth++) {
        if (!frame || !panic_ptr_readable(frame)) break;
        if (!paging_is_mapped_nolock(frame) || !paging_is_mapped_nolock(frame + 8u)) break;
        uint64_t* f = (uint64_t*)frame;
        uint64_t ret = f[1];
        if (!ret) break;
        p_trace_addr(ret);
        p_newline();
        uint64_t next = f[0];
        if (next <= frame) break;
        frame = next;
    }

    // 2) Fallback: scan stack mencari alamat yang menunjuk ke kernel text.
    if (p_trace_count == 0 && rsp) {
        for (uint32_t i = 0; i < 256u && p_trace_count < 6u; i++) {
            uint64_t a = rsp + (uint64_t)i * 8u;
            if (!panic_ptr_readable(a) || !paging_is_mapped_nolock(a)) break;
            uint64_t v = *(uint64_t*)a;
            if (v >= KERNEL_VMA && v < rbp + 0x100000ull) {   // dekat = masuk akal
                p_trace_addr(v);
                p_newline();
            }
        }
    }

    if (p_trace_count == 0) {
        p_str("(tidak ada frame valid - RSP/RBP korup?)", C_FAINT);
        p_newline();
    }
}

// Varian untuk kernel_panic() (tidak punya registers_t): tidak ada yang bisa
// ditelusuri, jadi cukup reset penghitung dan tulis alasannya. Dipisah supaya
// penghitung frame tetap milik file ini.
void panic_backtrace_unavailable(void) {
    p_label("BACKTRACE");
    p_trace_count = 0;
    p_str("(tidak ada frame valid - RSP/RBP korup?)", C_FAINT);
    p_newline();
}

// Kumpulkan alamat backtrace untuk crashdump (versi teks; layar punya rutin
// gambar sendiri di atas).
uint32_t panic_bt_collect(uint64_t rbp, uint64_t rsp, uint64_t cs,
                          uint64_t* out, uint32_t max) {
    uint32_t n = 0;

    // Batas lama (0x1000..0x7FFF_FFFF_FFFF) menerima alamat USER, dan buffer
    // crashdump dulu benar-benar membacanya — dengan SMAP itu = #PF kedua di
    // tengah penulisan dump. Aturan sekarang sama dengan p_backtrace(): hanya
    // higher-half boleh dibaca, dan konteks user dilewati.
    if ((cs & 3u) == 3u) return 0;

    if (panic_ptr_readable(rbp) && rbp < 0xFFFFFFFFFFFFF000ull) {
        const uint64_t* bp = (const uint64_t*)rbp;
        for (uint32_t depth = 0; depth < max && n < max; depth++) {
            uint64_t next = bp[0];
            uint64_t ret  = bp[1];
            if (ret == 0) break;
            out[n++] = ret;
            if (next <= rbp) break;
            bp = (const uint64_t*)next;
        }
    }
    if (n == 0 && panic_ptr_readable(rsp)) {
        // Fallback: -O2 sering menghilangkan frame pointer -> scan stack.
        const uint64_t* sp = (const uint64_t*)rsp;
        for (uint32_t i = 0; i < 768u && n < max; i++) {
            uint64_t q = sp[i];
            if (q >= KERNEL_VMA && q < KERNEL_VMA + 0x2000000ull) out[n++] = q;
        }
    }
    return n;
}
