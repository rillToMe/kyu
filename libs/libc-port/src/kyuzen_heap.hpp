// ============================================================
// KyuzenOS — heap user-space dinamis (Phase 9.5)
//
// MASALAH YANG DIPERBAIKI (audit Phase 9.5)
// -----------------------------------------
// Phase 1..9 memakai SATU region tetap:
//
//     _start -> kyuzen_libc_heap_init()
//                    -> sys_alloc(#9, KYUZEN_LIBC_ARENA_BYTES)   // 1 MiB
//                    -> FreListHeap(span)                        // satu-satunya arena
//
// `LIBC_NAMESPACE::FreeListHeap` (LLVM libc 22.1.8,
// src/__support/freelist_heap.h) adalah best-fit freelist di atas SATU region
// contiguous dan TIDAK punya grow hook: `allocate()` mengembalikan nullptr
// begitu permintaan tak muat. Jadi malloc/new di atas ~1 MiB SELALU NULL:
//
//     new uint32_t[1920*1080]   (~8.3 MiB, buffer layar wallpaper Phase 9)
//         -> malloc -> FreeListHeap::allocate -> nullptr
//         -> operator new[] -> abort() -> ud2 -> INT 6 (BSOD)
//
// Padahal `sys_alloc(#9)` (kernel/uheap.c) sendiri sanggup 8.3 MiB
// (UHEAP_MAX_ALLOC 64 MiB). Yang salah bukan kernel, melainkan port layer
// yang hanya mengambil satu region sekali dan tak pernah menambahnya.
//
// DESAIN SEKARANG
// ---------------
// Heap = DAFTAR ARENA; setiap arena = satu region sys_alloc + satu instance
// FreeListHeap di atasnya (algoritma size-class LLVM TIDAK diganti; yang
// berubah hanya "berapa region yang dipakai dan bagaimana request dirutekan"):
//
//     malloc(size)
//         |
//         +-- cukup di salah satu arena? --> arena itu (best-fit LLVM)
//         |
//         +-- tidak --> tambah arena (sys_alloc) --> ulangi
//                          |
//                          +-- request besar (> plafon deret) --> arena
//                              khusus seukuran permintaan
//
// Kebijakan pertumbuhan (dipilih agar tidak ada reservasi besar di depan):
//   - ARENA PERTAMA diambil LAZY saat alokasi pertama (bukan di _start), jadi
//     app yang tidak pernah malloc tidak menahan satu halaman pun.
//   - Deret GEOMETRIS: 1 MiB -> 2 MiB -> 4 MiB -> 4 MiB -> ... (plafon
//     KYUZEN_LIBC_ARENA_MAX_BYTES). Arena tumbuh hanya saat permintaan benar-
//     benar tidak muat, dan ukuran arena baru selalu >= kebutuhan pemicunya.
//   - Permintaan di atas plafon deret mendapat ARENA KHUSUS seukuran
//     permintaan (bulat halaman) dan TIDAK menggeser deret geometris: satu
//     buffer besar tidak membuat arena berikutnya ikut membengkak.
//   - Arena khusus (permintaan > plafon deret) dibebaskan kembali ke uheap
//     (sys_free #10) begitu tak ada lagi blok hidup di dalamnya, sehingga
//     buffer besar sesaat tidak menahan memori fisik selamanya. Arena deret
//     geometris dipertahankan sebagai working set (di situ blok-blok kecil
//     dipakai ulang).
//
// CONTRACT ROUTING
//   - `free`/`realloc(ptr)` mencari arena pemilik lewat rentang alamat
//     [base, base+bytes) — O(jumlah arena), bukan asumsi "semua di arena #0".
//   - Pointer yang bukan milik arena mana pun (mis. hasil sys_alloc langsung
//     atau pointer sampah) DIABAIKAN: perilakunya UB di C, dan mengabaikan
//     tidak bisa merusak heap. Tidak ada jalur yang mengembalikan pointer
//     tidak valid atau menulis di luar blok.
//
// THREAD / SMP
//   Tanpa lock: di KyuzenOS satu proses = satu task = satu thread
//   (LIBC_THREAD_MODE_SINGLE; kernel tak mengekspos primitif sinkronisasi ke
//   user). Asumsi yang sama sudah dipakai errno + guard static-local Phase 1/5.
//   Bila kelak ada app multi-thread, arena table-lah titik lock yang benar.
//
// PEMAKAI HEADER INI
//   Header ini HARUS di-include tepat satu TU per program (pemilik heap) dan
//   TU itu WAJIB mendefinisikan backing store di namespace kyuzen_heap:
//
//     void *kyuzen_heap::region_alloc(size_t bytes);  // >= 4096, page-aligned
//     void  kyuzen_heap::region_free(void *region);   // alamat yang sama
//
//   Target   : libs/libc-port/src/kyuzen_libc_port.cpp (syscall #9/#10).
//   Host test: test/libc_heap_test.cpp (mock berbasis malloc host).
// ============================================================
#ifndef KYUZEN_LIBC_PORT_HEAP_HPP
#define KYUZEN_LIBC_PORT_HEAP_HPP

#include <stddef.h>  // size_t (header freestanding, sama dengan hdr/types/size_t.h)

#include "src/__support/block.h"
#include "src/__support/CPP/span.h"
#include "src/__support/freelist_heap.h"
#include "src/__support/macros/config.h"
#include "src/string/memory_utils/inline_memcpy.h"
#include "src/string/memory_utils/inline_memset.h"

// ------------------------------------------------------------
// Knob kompilasi (bisa di-override dengan -DKYUZEN_LIBC_...=...)
// ------------------------------------------------------------
// Ukuran arena pertama (= perilaku Phase 1) sekaligus satuan deret geometris.
#ifndef KYUZEN_LIBC_ARENA_BYTES
#define KYUZEN_LIBC_ARENA_BYTES (1u << 20)  // 1 MiB = 256 halaman uheap
#endif
// Plafon deret geometris. Di atas ini arena TIDAK digandakan lagi; permintaan
// besar dilayani arena khusus seukuran permintaan.
#ifndef KYUZEN_LIBC_ARENA_MAX_BYTES
#define KYUZEN_LIBC_ARENA_MAX_BYTES (4u << 20)
#endif
// Jumlah slot tabel arena. 32 x 4 MiB = 128 MiB plafon working set deret.
#ifndef KYUZEN_LIBC_MAX_ARENAS
#define KYUZEN_LIBC_MAX_ARENAS 32u
#endif
// Plafon satu permintaan. == UHEAP_MAX_ALLOC (include/uheap.h): di atas ini
// kernel pasti mengembalikan 0, jadi port menolak lebih awal (tanpa syscall)
// supaya aritmetika ukuran tidak pernah overflow.
#ifndef KYUZEN_LIBC_MAX_ALLOC_BYTES
#define KYUZEN_LIBC_MAX_ALLOC_BYTES (64u << 20)
#endif
// Granularitas region uheap (selalu awal halaman; 1 halaman guard antar region).
#ifndef KYUZEN_LIBC_REGION_ALIGN
#define KYUZEN_LIBC_REGION_ALIGN 4096u
#endif

namespace kyuzen_heap {

using LIBC_NAMESPACE::Block;
using LIBC_NAMESPACE::FreeListHeap;
using LIBC_NAMESPACE::cpp::byte;
using LIBC_NAMESPACE::cpp::span;

constexpr size_t kInitialArenaBytes = KYUZEN_LIBC_ARENA_BYTES;
constexpr size_t kMaxArenaBytes = KYUZEN_LIBC_ARENA_MAX_BYTES;
constexpr size_t kMaxArenas = KYUZEN_LIBC_MAX_ARENAS;
constexpr size_t kMaxAllocBytes = KYUZEN_LIBC_MAX_ALLOC_BYTES;
constexpr size_t kRegionAlign = KYUZEN_LIBC_REGION_ALIGN;
constexpr size_t kNoArena = static_cast<size_t>(-1);

// ------------------------------------------------------------
// Backing store — WAJIB disediakan TU pemakai (lihat catatan di atas).
// ------------------------------------------------------------
void *region_alloc(size_t bytes);
void region_free(void *region);

// ------------------------------------------------------------
// Tabel arena.
//
// `heap()` dihitung (bukan disimpan) supaya Arena tetap POD dan bisa
// dipindahkan saat slot dikompaksi — pointer hasil placement-new selalu
// menunjuk storage arena itu sendiri.
// ------------------------------------------------------------
struct Arena {
  alignas(FreeListHeap) unsigned char storage[sizeof(FreeListHeap)];
  byte *base;    // awal region dari region_alloc
  size_t bytes;  // ukuran region (yang diminta ke uheap, page-aligned)
  size_t live;   // jumlah blok yang masih dipegang pemanggil

  FreeListHeap *heap() {
    return reinterpret_cast<FreeListHeap *>(static_cast<void *>(storage));
  }
};

inline Arena *table() {
  static Arena arenas[kMaxArenas];
  return arenas;
}
inline size_t &count() {
  static size_t n = 0;
  return n;
}
inline size_t &next_arena_bytes() {
  static size_t n = kInitialArenaBytes;
  return n;
}

// ------------------------------------------------------------
// Observability (dipakai host test; tak menambah biaya runtime).
// ------------------------------------------------------------
inline size_t arena_count() { return count(); }
inline size_t arena_bytes(size_t i) { return table()[i].bytes; }
inline size_t arena_live(size_t i) { return table()[i].live; }
inline size_t mapped_bytes() {
  size_t total = 0;
  for (size_t i = 0; i < count(); ++i) total += table()[i].bytes;
  return total;
}
// Kembalikan seluruh arena ke backing store. HANYA untuk test (butuh semua
// blok sudah dibebaskan; kalau tidak, arena yang dilepas masih dipakai).
inline void test_reset() {
  while (count() > 0) {
    Arena a = table()[count() - 1];
    count() = count() - 1;
    region_free(a.base);
  }
  next_arena_bytes() = kInitialArenaBytes;
  LIBC_NAMESPACE::freelist_heap = nullptr;
}

namespace detail {

LIBC_INLINE size_t round_up(size_t value, size_t align) {
  return (value + align - 1) & ~(align - 1);
}

LIBC_INLINE size_t pow2_at_least(size_t value) {
  size_t p = 1;
  while (p < value) p <<= 1;
  return p;
}

// `freelist_heap` milik libc selalu menunjuk arena #0 (kompatibilitas simbol:
// port mendefinisikan instance kuat-nya, dan setiap objek libc yang dulu
// memakainya tetap menemukan heap yang sah). Tak ada lagi objek libc yang
// memakainya setelah port menyediakan malloc/free/calloc/realloc/aligned_alloc
// sendiri — lihat catatan panjang di kyuzen_libc_port.cpp.
LIBC_INLINE void sync_libc_global() {
  LIBC_NAMESPACE::freelist_heap =
      count() > 0 ? table()[0].heap() : nullptr;
}

// ------------------------------------------------------------
// Tambah satu arena. `need` = byte region minimal yang dibutuhkan pemicunya
// (sudah termasuk margin header block + padding alignment).
//
// Return nullptr bila: slot habis, sys_alloc gagal (OOM task/uheap), atau
// region terlalu kecil untuk dibentuk menjadi block (praktis tak terjadi:
// minimum 1 MiB).
// ------------------------------------------------------------
LIBC_INLINE Arena *grow(size_t need) {
  if (count() >= kMaxArenas) return nullptr;

  const bool dedicated = need > kMaxArenaBytes;
  size_t bytes;
  if (dedicated) {
    // Arena khusus: seukuran permintaan, page-aligned. Deret geometris TIDAK
    // digeser (buffer besar sesaat tidak membesarkan arena berikutnya).
    bytes = round_up(need, kRegionAlign);
  } else {
    bytes = next_arena_bytes();
    if (bytes < need) {
      bytes = pow2_at_least(need);
      if (bytes > kMaxArenaBytes) bytes = kMaxArenaBytes;
    }
    if (bytes < kInitialArenaBytes) bytes = kInitialArenaBytes;
    // Pertumbuhan berikutnya: dua kali (dibatasi plafon).
    size_t next = bytes > kMaxArenaBytes / 2 ? kMaxArenaBytes : bytes * 2;
    next_arena_bytes() = next < kInitialArenaBytes ? kInitialArenaBytes : next;
  }
  bytes = round_up(bytes, kRegionAlign);

  void *region = region_alloc(bytes);
  if (region == nullptr) return nullptr;

  Arena &a = table()[count()];
  a.base = static_cast<byte *>(region);
  a.bytes = bytes;
  a.live = 0;
  ::new (static_cast<void *>(a.storage))
      FreeListHeap(span<byte>(a.base, bytes));
  count() = count() + 1;
  sync_libc_global();
  return &a;
}

// Lepas arena `idx` ke backing store. `allow_first` = izin melepas arena #0
// (indeks 0 bukan arena istimewa: `sync_libc_global` selalu mengarahkan ulang
// `freelist_heap` ke arena yang tersisa).
LIBC_INLINE void drop_arena(size_t idx, bool allow_first) {
  if (idx == kNoArena || idx >= count()) return;
  if (idx == 0 && !allow_first) return;
  Arena a = table()[idx];  // salinan: base dibutuhkan untuk region_free
  const size_t last = count() - 1;
  if (idx != last) table()[idx] = table()[last];
  count() = last;
  region_free(a.base);
  sync_libc_global();
}

LIBC_INLINE size_t arena_index_of(const void *ptr) {
  const byte *p = static_cast<const byte *>(ptr);
  for (size_t i = 0; i < count(); ++i) {
    const Arena &a = table()[i];
    if (p >= a.base && p < a.base + a.bytes) return i;
  }
  return kNoArena;
}

// Satu percobaan alokasi dari satu arena. alignment == 0 -> allocate biasa.
LIBC_INLINE void *try_arena(Arena &a, size_t alignment, size_t size) {
  void *p = alignment == 0 ? a.heap()->allocate(size)
                           : a.heap()->aligned_allocate(alignment, size);
  if (p != nullptr) a.live = a.live + 1;
  return p;
}

// Coba semua arena dulu; kalau tak ada yang muat, tambah arena lalu ulangi.
// Margin = tempat untuk header Block + sentinel + padding alignment.
LIBC_INLINE void *alloc_impl(size_t alignment, size_t size) {
  for (size_t i = 0; i < count(); ++i) {
    void *p = try_arena(table()[i], alignment, size);
    if (p != nullptr) return p;
  }

  size_t margin = kRegionAlign;
  if (alignment != 0 && alignment > margin) margin = alignment + kRegionAlign;

  // Loop ini praktis sekali jalan: arena baru selalu >= size + margin. Iterasi
  // tambahan hanya jaring pengaman bila ukuran region ternyata kurang (mis.
  // pembulatan internal Block) — dibatasi supaya tidak ada loop pertumbuhan
  // tak berujung saat alokasi memang mustahil.
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (margin > kMaxAllocBytes || size > kMaxAllocBytes - margin)
      return nullptr;  // plafon uheap / cegah overflow aritmetika
    Arena *a = grow(size + margin);
    if (a == nullptr) return nullptr;  // OOM nyata: NULL, bukan abort
    void *p = try_arena(*a, alignment, size);
    if (p != nullptr) return p;
    // grow() selalu menambahkan di slot terakhir; region ini ternyata kurang.
    drop_arena(count() - 1, /*allow_first=*/a == table());
    margin *= 2;
  }
  return nullptr;
}

LIBC_INLINE size_t usable_bytes(void *ptr) {
  return Block::from_usable_space(ptr)->inner_size();
}

}  // namespace detail

// ------------------------------------------------------------
// API heap (dipakai kyuzen_libc_port.cpp untuk entry point C, dan host test).
// Semantik tiap fungsi sengaja MENGIKUTI versi baremetal LLVM libc 22.1.8
// yang digantikan (lihat kyuzen_libc_port.cpp), supaya perilaku app tak
// berubah selain hilangnya plafon 1 MiB:
//   - size 0 -> nullptr (bukan pointer unik seperti glibc),
//   - errno TIDAK disentuh (versi LLVM juga tidak),
//   - realloc(nullptr, n) = malloc(n); realloc(p, 0) = free(p) + nullptr;
//   - aligned_alloc non-pow2 / size % alignment != 0 -> nullptr.
// ------------------------------------------------------------

LIBC_INLINE void *alloc(size_t size) {
  if (size == 0) return nullptr;
  if (size > kMaxAllocBytes) return nullptr;
  return detail::alloc_impl(0, size);
}

LIBC_INLINE void free(void *ptr) {
  if (ptr == nullptr) return;
  const size_t idx = detail::arena_index_of(ptr);
  if (idx == kNoArena) return;  // bukan milik heap: UB pemanggil, ABAIKAN
  Arena &a = table()[idx];
  a.heap()->free(ptr);
  if (a.live > 0) a.live = a.live - 1;
  // Arena khusus (dibuat untuk satu permintaan besar di atas plafon deret) yang
  // sudah kosong dikembalikan ke uheap: buffer besar sesaat tidak menahan
  // memori fisik selamanya. Arena deret geometris justru DIPERTAHANKAN sebagai
  // working set — di sanalah blok-blok kecil berikutnya dipakai ulang tanpa
  // syscall (syarat §9 "reuse").
  if (a.live == 0 && a.bytes > kMaxArenaBytes)
    detail::drop_arena(idx, /*allow_first=*/true);
}

LIBC_INLINE void *calloc(size_t num, size_t size) {
  size_t bytes;
  if (__builtin_mul_overflow(num, size, &bytes)) return nullptr;
  void *p = alloc(bytes);
  if (p != nullptr) LIBC_NAMESPACE::inline_memset(p, 0, bytes);
  return p;
}

LIBC_INLINE void *realloc(void *ptr, size_t size) {
  if (ptr == nullptr) return alloc(size);
  if (size == 0) {  // paritas FreeListHeap::realloc
    free(ptr);
    return nullptr;
  }
  const size_t idx = detail::arena_index_of(ptr);
  if (idx == kNoArena) return nullptr;  // bukan milik heap: gagal, tak utak-atik
  Arena &a = table()[idx];
  // Jalur biasa: arena pemilik yang menangani (in-place bila muat, atau
  // alokasi + salin + bebas di DALAM arena yang sama).
  if (void *p = a.heap()->realloc(ptr, size)) return p;
  // Arena pemilik tak cukup -> lintas arena (inti perbaikan Phase 9.5).
  const size_t old = detail::usable_bytes(ptr);
  void *fresh = alloc(size);
  if (fresh == nullptr) return nullptr;  // blok lama tetap utuh (paritas LLVM)
  LIBC_NAMESPACE::inline_memcpy(fresh, ptr, old < size ? old : size);
  free(ptr);
  return fresh;
}

LIBC_INLINE void *aligned_alloc(size_t alignment, size_t size) {
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
  if (size == 0 || size % alignment != 0) return nullptr;
  return detail::alloc_impl(alignment, size);
}

}  // namespace kyuzen_heap

#endif  // KYUZEN_LIBC_PORT_HEAP_HPP
