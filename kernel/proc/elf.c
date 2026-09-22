#include "elf.h"
#include "kyuzenfs.h"
#include "heap.h"
#include "string.h"
#include "paging.h"
#include "smap.h"

extern void kprint(const char* str);
extern void kprint_num(uint32_t num);

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
    // 25/33/57, kernel_userlib) meng-copy filename ke buffer kernel dulu,
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

            // PRE-MAP: map semua 4KB pages yang dicakup segmen
            for (uint64_t page = seg_vaddr & ~0xFFFULL; page < seg_end; page += 4096) {
                if (!paging_is_mapped_into(page, target_pml4)) {
                    if (!vmm_alloc_page_into(page, 7, target_pml4)) {
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
            for (uint64_t page = stack_bottom; page < USER_STACK_TOP; page += 4096) {
                phys_addr_t pa = pmm_alloc_page();
                if (pa == PHYS_NULL ||
                    !vmm_map_page_into(page, pa, 7, target_pml4)) {
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