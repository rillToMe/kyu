#include "elf.h"
#include "kyuzenfs.h"
#include "heap.h"
#include "string.h"
#include "paging.h"
#include "smap.h"
#include "kprint.h"


// =======================================================================
// elf_load_file() — Load ELF64 user-app ke RAM, kembalikan entry point
//
// target_pml4: address space tujuan untuk mapping user page (FIX_002 —
// eksplisit, bukan global). PHYS_NULL = map ke kernel PML4 (fallback).
//
// CATATAN: User apps dikompilasi sebagai 64-bit ELF (e_machine = 0x3E = x86_64)
// ELF64 berbeda dari ELF32 dalam dua hal penting:
//   1. e_entry, e_phoff, e_shoff adalah uint64_t (bukan uint32_t)
//   2. Di Program Header, p_flags ada SEBELUM p_offset (bukan setelah!)
// Menggunakan struct ELF32 untuk ELF64 → semua field offset salah → crash!
// =======================================================================
uint64_t elf_load_file(char* filename, uint64_t* out_stack_top,
                       phys_addr_t target_pml4) {
    if (out_stack_top)  *out_stack_top  = 0;

    // Fase 3: app pindah ke /apps/. Bare name (tanpa '/') di-resolve ke
    // /apps/<name>; path absolut dipakai apa adanya. Semua caller (syscall
    // 25/33/57) meng-copy filename ke buffer kernel dulu,
    // jadi nama maksimal UC_MAX_FNAME dan buffer path[72] aman.
    char path[72];
    const char* load = filename;
    if (filename[0] != '/') {
        int k = 0;
        const char* ap = "/apps/";
        while (ap[k]) { path[k] = ap[k]; k++; }
        for (int i = 0; filename[i] && k < (int)sizeof(path) - 1; i++) path[k++] = filename[i];
        path[k] = '\0';
        load = path;
    }

    uint32_t file_size = kfs_get_file_size((char*)load);
    if (file_size == 0) {
        kprint("[ELF] Error: File kosong atau tidak ditemukan!\n");
        return 0;
    }

    uint8_t* file_buffer = (uint8_t*)kmalloc(file_size);
    if (!file_buffer) {
        kprint("[ELF] Error: Heap habis!\n");
        return 0;
    }
    kfs_read_to_buffer((char*)load, (char*)file_buffer, file_size);

    // Validasi ELF Magic Number
    if (file_buffer[0] != 0x7F || file_buffer[1] != 'E' ||
        file_buffer[2] != 'L'  || file_buffer[3] != 'F') {
        kprint("[ELF] Error: Bukan file ELF!\n");
        kfree(file_buffer);
        return 0;
    }

    // Deteksi class: byte[4] = 1 (ELF32) atau 2 (ELF64)
    uint8_t elf_class = file_buffer[4];

    if (elf_class == ELFCLASS64) {
        // ===========================
        //  LOAD ELF64 (x86_64)
        // ===========================
        // Bug 3.3: tanpa AS per-proses, memetakan vaddr user ke kernel PML4
        // mencemari tabel halaman kernel. Tolak di awal.
        if (target_pml4 == PHYS_NULL) {
            kprint("[ELF64] Error: butuh AS per-proses (target_pml4 null)!\n");
            kfree(file_buffer);
            return 0;
        }

        elf64_ehdr_t* hdr  = (elf64_ehdr_t*)file_buffer;

        // Bug 3.2: validasi header sebelum membaca program headers — offset &
        // jumlahnya harus berada di dalam file, dan e_phnum dibatasi.
        if (hdr->e_phoff > file_size ||
            (uint64_t)hdr->e_phnum * sizeof(elf64_phdr_t) > file_size - hdr->e_phoff) {
            kprint("[ELF64] Error: e_phoff/e_phnum out of range!\n");
            kfree(file_buffer);
            return 0;
        }
        elf64_phdr_t* phdr = (elf64_phdr_t*)(file_buffer + hdr->e_phoff);

        for (int i = 0; i < hdr->e_phnum; i++) {
            if (phdr[i].p_type != 1) continue; // Hanya PT_LOAD

            uint64_t seg_vaddr = phdr[i].p_vaddr;
            // Bug 3.2: cegah overflow vaddr+memsz (seg_end harus >= seg_vaddr).
            if (phdr[i].p_memsz > UINT64_MAX - seg_vaddr) {
                kprint("[ELF64] Error: segmen vaddr overflow!\n");
                kfree(file_buffer);
                return 0;
            }
            uint64_t seg_end = seg_vaddr + phdr[i].p_memsz;

            // Bug 3.2: data segmen harus berada di dalam file (p_offset+p_filesz).
            if (phdr[i].p_offset > file_size ||
                (uint64_t)phdr[i].p_filesz > file_size - phdr[i].p_offset) {
                kprint("[ELF64] Error: segmen melebihi ukuran file!\n");
                kfree(file_buffer);
                return 0;
            }

            // ============================================================
            // ARCH-5: W^X — hormati p_flags segmen.
            //
            // SEBELUMNYA semua segmen di-map flags=7 (P|RW|US) dan tidak ada
            // bit NX, sehingga SELURUH segmen user RWX: .text writable dan
            // .data/.bss executable. Satu bug memory-safety di app mana pun
            // (mis. overflow parser di browser) langsung jadi eksekusi kode.
            //
            // URUTAN PENTING — "map writable → isi → reprotect":
            // CR0.WP=1 (cpu_verify_wp) membuat KERNEL tidak bisa menulis
            // halaman yang RW=0, jadi halaman tidak bisa langsung di-map
            // read-only: penyalinan isi segmen akan #PF di kernel. Karena itu
            // halaman di-map writable dulu, diisi, lalu flag finalnya
            // dipasang lewat vmm_protect_page_into() di akhir blok ini.
            // Ini pola yang sama dengan Linux set_memory_ro() setelah load.
            //
            // Flag FINAL yang dituju:
            //   PF_X (bit 0) set   -> boleh dieksekusi -> NX dikosongkan
            //   PF_X tidak set     -> NX dipasang (data/rodata tidak executable)
            //   PF_W (bit 1) set   -> RW=1 (writable dari CPL3)
            //   PF_W tidak set     -> RW=0 (read-only dari CPL3)
            //
            // Catatan PF_R (bit 2): x86-64 tidak punya bit "read-only" yang
            // terpisah dari present; halaman selalu bisa dibaca. Jadi PF_R
            // tidak perlu diterjemahkan — ketiadaan PF_W sudah cukup.
            // ============================================================
            const uint32_t PF_X = 1u << 0;
            const uint32_t PF_W = 1u << 1;

            // Flag final (setelah isi segmen selesai ditulis).
            uint64_t seg_flags = PAGING_PRESENT | PAGING_USER;
            if (phdr[i].p_flags & PF_W) seg_flags |= PAGING_WRITABLE;
            if (!(phdr[i].p_flags & PF_X)) seg_flags |= paging_nx_flag();

            // Flag sementara selama pengisian: writable supaya kernel bisa
            // menulis (CR0.WP=1), executable supaya tidak ada jendela di mana
            // instruksi yang sudah tersalin ter-fault saat masih di-load.
            uint64_t fill_flags = PAGING_PRESENT | PAGING_WRITABLE | PAGING_USER;

            // PRE-MAP: map semua 4KB pages yang dicakup segmen
            for (uint64_t page = seg_vaddr & ~0xFFFULL; page < seg_end; page += 4096) {
                if (!paging_is_mapped_into(page, target_pml4)) {
                    if (!vmm_alloc_page_into(page, fill_flags, target_pml4)) {
                        kprint("[ELF64] FATAL: Tidak bisa map page 0x");
                        kprint_num((uint32_t)(page >> 32)); kprint_num((uint32_t)page);
                        kprint("\n");
                        kfree(file_buffer);
                        return 0;
                    }
                }
            }

            // COPY: salin data segmen ke virtual address tujuan
            // Tahap 4: halaman tujuan US=1 → tulis dalam jendela SMAP.
            user_access_begin();
            memcpy((void*)seg_vaddr,
                   file_buffer + phdr[i].p_offset,
                   (uint32_t)phdr[i].p_filesz);

            // ZERO BSS: isi sisa ruang dengan 0
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((uint8_t*)seg_vaddr + phdr[i].p_filesz,
                       0,
                       (uint32_t)(phdr[i].p_memsz - phdr[i].p_filesz));
            }
            user_access_end();

            // ------------------------------------------------------------
            // ARCH-5: REPROTECT — pasang flag final (W^X) setelah isi selesai.
            //
            // Dilakukan per-halaman karena satu segmen bisa menempati banyak
            // PTE, dan hanya halaman yang benar-benar bagian segmen ini yang
            // boleh diubah. Halaman milik segmen LAIN yang kebetulan berbagi
            // page terakhir (segmen ELF tidak selalu page-aligned) TIDAK
            // disentuh — kalau disentuh, proteksi segmen berikutnya bisa
            // terhapus sebelum segmen itu di-load.
            //
            // Kegagalan reprotect tidak fatal untuk keamanan (halaman tetap
            // writable/executable seperti perilaku lama), tapi dicatat supaya
            // tidak hilang diam-diam.
            // ------------------------------------------------------------
            if (seg_flags != fill_flags) {
                for (uint64_t page = seg_vaddr & ~0xFFFULL; page < seg_end; page += 4096) {
                    if (!vmm_protect_page_into(page, seg_flags, target_pml4)) {
                        kprint("[ELF64] WARN: reprotect gagal di page 0x");
                        kprint_num((uint32_t)(page >> 32)); kprint_num((uint32_t)page);
                        kprint("\n");
                    }
                }
            }
        }

        uint64_t entry = hdr->e_entry;
        kfree(file_buffer);

        // Alokasi stack per-app hanya untuk caller yang meng-handle RSP switch
        // (sys_exec). Caller NULL (shell direct-launch) pakai stack pemanggil.
        // FIX_005 Tahap 3: stack di USER RANGE AS target — kmalloc higher-half
        // kini US=0, ring 3 yang push ke sana langsung #PF. Frame dibebaskan
        // otomatis oleh vmm_destroy_address_space saat AS mati.
        if (out_stack_top) {
            if (target_pml4 == PHYS_NULL) {
                // Tanpa AS per-proses tidak ada tempat sah untuk stack ring 3.
                kprint("[ELF64] Error: stack butuh AS per-proses!\n");
                return 0;
            }
            extern uint64_t hhdm_offset;
            uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
            // ARCH-5: stack user = RW + NX. Stack yang executable adalah
            // target klasik shellcode (return-into-stack / injected buffer),
            // jadi ini bagian inti dari W^X — bukan sekadar pelengkap.
            uint64_t stack_flags = paging_user_rw_nx();
            for (uint64_t page = stack_bottom; page < USER_STACK_TOP; page += 4096) {
                phys_addr_t pa = pmm_alloc_page();
                if (pa == PHYS_NULL ||
                    !vmm_map_page_into(page, pa, stack_flags, target_pml4)) {
                    if (pa != PHYS_NULL) pmm_free_page(pa);
                    kprint("[ELF64] Error: Tidak bisa map user stack!\n");
                    return 0;   // halaman ter-map ikut bebas saat AS dihancurkan
                }
                // Zero via HHDM — frame bekas tidak boleh bocor ke ring 3.
                memset((void*)(pa + hhdm_offset), 0, 4096);
            }
            *out_stack_top = USER_STACK_TOP;
        }

        return entry;

    } else if (elf_class == ELFCLASS32) {
        // ===========================
        //  LOAD ELF32 (i386) — legacy
        // ===========================
        // Bug 3.3: sama seperti ELF64 — mapping user ke kernel PML4 salah.
        if (target_pml4 == PHYS_NULL) {
            kprint("[ELF32] Error: butuh AS per-proses (target_pml4 null)!\n");
            kfree(file_buffer);
            return 0;
        }

        elf32_ehdr_t* hdr  = (elf32_ehdr_t*)file_buffer;

        // Bug 3.2: validasi program header berada dalam file.
        if (hdr->e_phoff > file_size ||
            (uint64_t)hdr->e_phnum * sizeof(elf32_phdr_t) > file_size - hdr->e_phoff) {
            kprint("[ELF32] Error: e_phoff/e_phnum out of range!\n");
            kfree(file_buffer);
            return 0;
        }
        elf32_phdr_t* phdr = (elf32_phdr_t*)(file_buffer + hdr->e_phoff);

        for (int i = 0; i < hdr->e_phnum; i++) {
            if (phdr[i].p_type != 1) continue;

            uint32_t seg_vaddr = phdr[i].p_vaddr;
            uint32_t seg_end   = seg_vaddr + phdr[i].p_memsz;

            // Bug 3.2: data segmen harus berada di dalam file.
            if (phdr[i].p_offset > file_size ||
                (uint32_t)phdr[i].p_filesz > file_size - phdr[i].p_offset) {
                kprint("[ELF32] Error: segmen melebihi ukuran file!\n");
                kfree(file_buffer);
                return 0;
            }

            uint32_t block = seg_vaddr & 0xFFC00000;
            while (block < seg_end) {
                if (!vmm_alloc_page_into(block, 7, target_pml4)) {
                    kprint("[ELF32] FATAL: Tidak bisa map region!\n");
                    kfree(file_buffer);
                    return 0;
                }
                block += 0x400000;
            }

            user_access_begin();   // Tahap 4: halaman tujuan US=1
            memcpy((void*)(uint64_t)seg_vaddr,
                   file_buffer + phdr[i].p_offset,
                   phdr[i].p_filesz);

            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((uint8_t*)(uint64_t)seg_vaddr + phdr[i].p_filesz,
                       0,
                       phdr[i].p_memsz - phdr[i].p_filesz);
            }
            user_access_end();
        }

        kfree(file_buffer);
        return (uint64_t)hdr->e_entry;
    }

    kprint("[ELF] Error: ELF class tidak dikenal!\n");
    kfree(file_buffer);
    return 0;
}