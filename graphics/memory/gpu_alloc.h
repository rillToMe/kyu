#ifndef GPU_ALLOC_H
#define GPU_ALLOC_H

// ============================================================
// GPU memory backing (graphics/memory/gpu_alloc.h)
//
// Alokasi memori fisik untuk resource VirtIO-GPU. Karena PMM tidak
// menjamin physical contiguity antar halaman, dan VirtIO ATTACH_BACKING
// menerima DAFTAR mem_entry, backing dialokasikan sebagai list halaman
// individual (tiap entry = satu halaman 4096). Virtqueue memakai
// satu-page alloc (region < 4096 byte selalu muat satu halaman).
// ============================================================

#include <stdint.h>

// Satu halaman fisik contiguous (4096 byte).
typedef struct {
    uint64_t phys;      // physical address
    void*    virt;      // HHDM virtual address (phys + hhdm_offset)
} gpu_page_t;

// Alokasikan `count` halaman fisik untuk backing. Setiap halaman independen
// (tidak harus contiguous). Return jumlah sukses (0 = gagal total).
// pages[] diisi dari index 0..n. Caller menyediakan pages[count].
uint32_t gpu_alloc_pages(uint32_t count, gpu_page_t* pages);

// Alokasikan `count` halaman fisik yang BERURUTAN (contiguous). Diperlukan
// oleh surface backing yang di-akses sebagai satu buffer linear
// (`backing_virt + y*width`). PMM mengalokasi berurutan dari hint, jadi
// biasanya langsung contiguous; bila tidak, blok dilepas dan dicoba ulang
// (bounded). Return 0 sukses (pages[0..count-1] diisi), <0 gagal.
int gpu_alloc_pages_contiguous(uint32_t count, gpu_page_t* pages);

// Alokasikan SATU halaman fisik (untuk region virtqueue).
// Return 0 sukses (out diisi), <0 gagal.
int gpu_alloc_page(gpu_page_t* out);

// Bebaskan halaman yang sebelumnya dialokasikan.
void gpu_free_pages(gpu_page_t* pages, uint32_t count);

// Batas dimensi permukaan (pola audit overflow 5.6).
#define GHAL_MAX_DIM 8192

#endif // GPU_ALLOC_H
