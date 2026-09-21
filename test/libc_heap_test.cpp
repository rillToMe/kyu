// ============================================================
// Host test heap user-space dinamis — KyuzenOS Phase 9.5
//
// Yang diuji: libs/libc-port/src/kyuzen_heap.hpp APA ADANYA — header yang sama
// yang dikompilasi ke crt.o setiap app. Yang di-mock hanya backing store
// (region_alloc/region_free), yaitu satu-satunya bagian yang di target memakai
// syscall #9/#10. Karena itu test ini menjalankan: kebijakan arena/growth,
// routing multi-arena, reuse, alignment, jalur gagal, DAN algoritma
// FreeListHeap LLVM 22.1.8 yang asli (freelist.cpp + freetrie.cpp ikut
// di-link) — bukan allocator tiruan.
//
// Regresi yang dijaga (insiden Phase 9): permintaan 1920*1080*4 = ~8.3 MiB
// (buffer layar wallpaper) yang dulu SELALU NULL lewat arena tunggal 1 MiB.
//
// CATATAN HEADER HOST: TU ini TIDAK boleh meng-include header C/C++ milik
// toolchain host (mingw). Deklarasi `free` milik mingw tidak `noexcept`
// sedangkan deklarasi libc (hdr/func/free.h, ikut lewat header heap) `noexcept`
// — dua-duanya tak bisa hidup di satu TU, dan itu benar: file ini bukan app
// host biasa, ia mengompilasi header internal libc. Karena itu printf/malloc/
// free/abort dideklarasikan sendiri di bawah.
//
// Jalankan: make test-libc-heap
// ============================================================

#include "kyuzen_heap.hpp"  // WAJIB lebih dulu (header internal LLVM libc)

#include <stddef.h>  // size_t (header bawaan compiler, bukan libc host)

extern "C" int printf(const char *, ...);
extern "C" int fflush(void *);
extern "C" void *malloc(size_t);
extern "C" void free(void *) noexcept;
extern "C" [[noreturn]] void abort() noexcept;

// ------------------------------------------------------------
// Mock backing store: region 4096-aligned dari malloc host, dengan plafon
// (g_bytes_cap) supaya jalur OOM uheap bisa disimulasikan.
// ------------------------------------------------------------
namespace {

using u64 = unsigned long long;  // alamat/ukuran (LP64 maupun LLP64 sama-sama 64-bit)

const size_t kMockAlign = 4096;

size_t g_region_calls = 0;
size_t g_region_frees = 0;
size_t g_bytes_live = 0;
size_t g_bytes_cap = static_cast<size_t>(-1);

[[noreturn]] void fail(const char *expr, const char *file, int line) {
  printf("FAIL %s:%d: %s\n", file, line, expr);
  fflush(nullptr);  // abort() tidak flush stdout (buffer blok saat di-pipe)
  abort();
}

#define CHECK(cond)                    \
  do {                                 \
    if (!(cond)) fail(#cond, __FILE__, __LINE__); \
  } while (false)

// Layout: [raw malloc ptr (8 B)][bytes (8 B)] lalu region yang di-align 4096.
void *host_region_alloc(size_t bytes) {
  unsigned char *raw = static_cast<unsigned char *>(
      malloc(bytes + kMockAlign + 2 * sizeof(size_t)));
  if (raw == nullptr) return nullptr;
  u64 p = reinterpret_cast<u64>(raw) + 2 * sizeof(size_t);
  p = (p + (kMockAlign - 1)) & ~static_cast<u64>(kMockAlign - 1);
  size_t *hdr = reinterpret_cast<size_t *>(p);
  hdr[-2] = reinterpret_cast<size_t>(raw);
  hdr[-1] = bytes;
  return reinterpret_cast<void *>(p);
}

void host_region_free(void *region) {
  size_t *hdr = reinterpret_cast<size_t *>(region);
  free(reinterpret_cast<void *>(hdr[-2]));
}

}  // namespace

namespace kyuzen_heap {

void *region_alloc(size_t bytes) {
  g_region_calls++;
  if (bytes > g_bytes_cap - g_bytes_live) return nullptr;  // uheap penuh/OOM
  void *region = host_region_alloc(bytes);
  if (region == nullptr) return nullptr;
  g_bytes_live += bytes;
  return region;
}

void region_free(void *region) {
  g_region_frees++;
  size_t *hdr = static_cast<size_t *>(region);
  g_bytes_live -= hdr[-1];
  host_region_free(region);
}

}  // namespace kyuzen_heap

// ------------------------------------------------------------
// Stub simbol yang di target datang dari libc.a (dan sebagian dari port):
//
//   - `freelist_heap`: di target didefinisikan kyuzen_libc_port.cpp; di sini
//     test ini yang berperan sebagai "port" (heap-nya sendiri di kyuzen_heap).
//   - `write_to_stderr` + `internal::exit`: dipakai LIBC_ASSERT internal libc.
//     Assert itu SENGAJA dibiarkan AKTIF di test ini (deteksi double free /
//     pointer rusak / region terlalu kecil), jadi keduanya distub: pesan
//     dicetak lalu abort() — sama seperti perilaku exit(0xFF) di target yang
//     berakhir di __llvm_libc_exit.
// ------------------------------------------------------------
namespace LIBC_NAMESPACE_DECL {

FreeListHeap *freelist_heap = nullptr;

void write_to_stderr(cpp::string_view view) {
  printf("%.*s", static_cast<int>(view.size()), view.data());
}

namespace internal {

[[noreturn]] void exit(int status) {
  printf("\n[heap] invariant allocator dilanggar (exit %d)\n", status);
  abort();
}

}  // namespace internal
}  // namespace LIBC_NAMESPACE_DECL

namespace {

using kyuzen_heap::kInitialArenaBytes;
using kyuzen_heap::kMaxArenaBytes;
using kyuzen_heap::kMaxAllocBytes;

// ------------------------------------------------------------
// Helper: tulis + verifikasi pola per blok (deteksi korupsi/overlap).
// ------------------------------------------------------------
void fill_pattern(void *p, size_t n, unsigned char seed) {
  unsigned char *b = static_cast<unsigned char *>(p);
  for (size_t i = 0; i < n; ++i)
    b[i] = static_cast<unsigned char>(seed + (i % 251));
}

bool check_pattern(const void *p, size_t n, unsigned char seed) {
  const unsigned char *b = static_cast<const unsigned char *>(p);
  for (size_t i = 0; i < n; ++i)
    if (b[i] != static_cast<unsigned char>(seed + (i % 251))) return false;
  return true;
}

size_t live_total() {
  size_t total = 0;
  for (size_t i = 0; i < kyuzen_heap::arena_count(); ++i)
    total += kyuzen_heap::arena_live(i);
  return total;
}

void reset_mock() {
  kyuzen_heap::test_reset();
  g_region_calls = 0;
  g_region_frees = 0;
  g_bytes_live = 0;
  g_bytes_cap = static_cast<size_t>(-1);
}

// ------------------------------------------------------------
// 1. Dasar: ukuran kecil, tulis-baca, bebas.
// ------------------------------------------------------------
void test_basic() {
  reset_mock();
  void *a = kyuzen_heap::alloc(16);
  void *b = kyuzen_heap::alloc(256);
  void *c = kyuzen_heap::alloc(4096);
  CHECK(a && b && c);
  CHECK(a != b && b != c && a != c);
  fill_pattern(a, 16, 1);
  fill_pattern(b, 256, 2);
  fill_pattern(c, 4096, 3);
  CHECK(check_pattern(a, 16, 1) && check_pattern(b, 256, 2) &&
        check_pattern(c, 4096, 3));
  kyuzen_heap::free(b);  // bebas di tengah
  CHECK(check_pattern(a, 16, 1) && check_pattern(c, 4096, 3));
  kyuzen_heap::free(a);
  kyuzen_heap::free(c);
  // Blok kecil tak butuh arena kedua, dan arena pertama diambil LAZY.
  CHECK(kyuzen_heap::arena_count() == 1);
  CHECK(kyuzen_heap::arena_bytes(0) == kInitialArenaBytes);
  CHECK(kyuzen_heap::mapped_bytes() == kInitialArenaBytes);
  CHECK(live_total() == 0);
  // malloc(0) = NULL (paritas baremetal LLVM), free(NULL) = no-op.
  CHECK(kyuzen_heap::alloc(0) == nullptr);
  kyuzen_heap::free(nullptr);
}

// ------------------------------------------------------------
// 2. Pertumbuhan arena: total > 1 arena pertama.
// ------------------------------------------------------------
void test_growth() {
  reset_mock();
  const size_t big = 700 * 1024;
  void *p1 = kyuzen_heap::alloc(big);
  CHECK(p1 != nullptr);
  const size_t after_first = kyuzen_heap::arena_count();
  void *p2 = kyuzen_heap::alloc(big);
  CHECK(p2 != nullptr);  // dulu: NULL (arena tunggal 1 MiB)
  CHECK(after_first >= 1);
  CHECK(kyuzen_heap::arena_count() > after_first);  // arena kedua dibuat
  CHECK(kyuzen_heap::mapped_bytes() >= 2 * big);
  fill_pattern(p1, big, 11);
  fill_pattern(p2, big, 22);
  CHECK(check_pattern(p1, big, 11) && check_pattern(p2, big, 22));
  kyuzen_heap::free(p1);
  kyuzen_heap::free(p2);
  CHECK(live_total() == 0);
}

// ------------------------------------------------------------
// 3. Alokasi besar: 2 / 8 / 16 MiB (plafon uheap 64 MiB).
// ------------------------------------------------------------
void test_large() {
  const size_t sizes[3] = {2u << 20, 8u << 20, 16u << 20};
  for (int i = 0; i < 3; ++i) {
    const size_t s = sizes[i];
    reset_mock();
    unsigned char *p = static_cast<unsigned char *>(kyuzen_heap::alloc(s));
    CHECK(p != nullptr);
    // Tulis di awal & akhir (bukan seluruh blok: 16 MiB terlalu lambat di CI).
    p[0] = 0xAB;
    p[s - 1] = 0xCD;
    CHECK(p[0] == 0xAB && p[s - 1] == 0xCD);
    CHECK(kyuzen_heap::mapped_bytes() >= s);
    kyuzen_heap::free(p);
    CHECK(live_total() == 0);
  }
}

// ------------------------------------------------------------
// 4. Regresi Phase 9: buffer layar wallpaper 1920x1080x4 (~8.3 MiB).
// ------------------------------------------------------------
void test_wallpaper_8mb() {
  reset_mock();
  const size_t npx = 1920u * 1080u;
  unsigned int *px =
      static_cast<unsigned int *>(kyuzen_heap::alloc(npx * 4u));
  CHECK(px != nullptr);  // inti regresi: dulu operator new[] -> abort -> INT 6
  px[0] = 0xFF00FF00u;
  px[npx - 1] = 0x00FF00FFu;
  CHECK(px[0] == 0xFF00FF00u && px[npx - 1] == 0x00FF00FFu);
  CHECK(g_bytes_live >= npx * 4u);
  kyuzen_heap::free(px);
  // Arena khusus 8.3 MiB dikembalikan ke backing store saat kosong.
  CHECK(g_region_frees >= 1);
  CHECK(g_bytes_live == 0);
}

// ------------------------------------------------------------
// 5. Reuse: bebas lalu minta ukuran sama TIDAK meminta region baru.
// ------------------------------------------------------------
void test_reuse() {
  reset_mock();
  const size_t s = 2u << 20;
  void *p1 = kyuzen_heap::alloc(s);
  CHECK(p1 != nullptr);
  const size_t calls = g_region_calls;
  const size_t arenas = kyuzen_heap::arena_count();
  kyuzen_heap::free(p1);
  void *p2 = kyuzen_heap::alloc(s);
  CHECK(p2 != nullptr);
  CHECK(g_region_calls == calls);  // tanpa sys_alloc baru
  CHECK(kyuzen_heap::arena_count() == arenas);
  CHECK(p2 == p1);                 // blok pertama dipakai ulang
  kyuzen_heap::free(p2);
}

// ------------------------------------------------------------
// 6. Multi-arena + fragmentasi: blok tengah dibebaskan, dipakai ulang.
// ------------------------------------------------------------
void test_multi_arena_reuse() {
  reset_mock();
  const size_t s = 600 * 1024;  // 3 blok ini butuh > 1 arena
  void *a = kyuzen_heap::alloc(s);
  void *b = kyuzen_heap::alloc(s);
  void *c = kyuzen_heap::alloc(s);
  CHECK(a && b && c);
  CHECK(kyuzen_heap::arena_count() >= 2);
  fill_pattern(a, s, 3);
  fill_pattern(b, s, 4);
  fill_pattern(c, s, 5);
  kyuzen_heap::free(b);
  CHECK(check_pattern(a, s, 3) && check_pattern(c, s, 5));
  const size_t calls = g_region_calls;
  void *small = kyuzen_heap::alloc(64 * 1024);
  CHECK(small != nullptr);
  CHECK(g_region_calls == calls);  // ruang bebas dipakai ulang, bukan arena baru
  fill_pattern(small, 64 * 1024, 6);
  CHECK(check_pattern(a, s, 3) && check_pattern(c, s, 5));
  kyuzen_heap::free(small);
  kyuzen_heap::free(a);
  kyuzen_heap::free(c);
  CHECK(live_total() == 0);
}

// ------------------------------------------------------------
// 7. Alignment.
// ------------------------------------------------------------
void test_alignment() {
  reset_mock();
  // malloc = MIN_ALIGN Block (alignof(max_align_t)) — cukup untuk semua tipe
  // dasar di OS ini (tanpa long double/SSE).
  const size_t sizes[5] = {1, 7, 15, 64, 1000};
  for (int i = 0; i < 5; ++i) {
    void *p = kyuzen_heap::alloc(sizes[i]);
    CHECK(p != nullptr);
    CHECK(reinterpret_cast<u64>(p) % 16 == 0);
    kyuzen_heap::free(p);
  }
  void *p64 = kyuzen_heap::aligned_alloc(64, 4096);
  CHECK(p64 != nullptr);
  CHECK(reinterpret_cast<u64>(p64) % 64 == 0);
  kyuzen_heap::free(p64);
  void *p1m = kyuzen_heap::aligned_alloc(1u << 20, 1u << 20);
  CHECK(p1m != nullptr);
  CHECK(reinterpret_cast<u64>(p1m) % (1u << 20) == 0);
  kyuzen_heap::free(p1m);
  // Paritas LLVM: alignment bukan pangkat dua / size bukan kelipatan -> NULL.
  CHECK(kyuzen_heap::aligned_alloc(3, 9) == nullptr);
  CHECK(kyuzen_heap::aligned_alloc(64, 100) == nullptr);
  CHECK(kyuzen_heap::aligned_alloc(0, 64) == nullptr);
  CHECK(live_total() == 0);
}

// ------------------------------------------------------------
// 8. Jalur gagal: OOM nyata -> NULL (bukan abort), plafon -> NULL.
// ------------------------------------------------------------
void test_failure() {
  reset_mock();
  // Lebih besar dari plafon uheap: ditolak SEBELUM menyentuh backing store.
  const size_t calls = g_region_calls;
  CHECK(kyuzen_heap::alloc(kMaxAllocBytes + 4096) == nullptr);
  CHECK(g_region_calls == calls);
  // Cap kecil: permintaan besar yang sah gagal dengan NULL, heap tetap sehat.
  g_bytes_cap = 64 * 1024;
  CHECK(kyuzen_heap::alloc(2u << 20) == nullptr);
  kyuzen_heap::free(nullptr);  // tetap aman
  g_bytes_cap = static_cast<size_t>(-1);
  void *p = kyuzen_heap::alloc(2u << 20);  // pulih setelah OOM
  CHECK(p != nullptr);
  kyuzen_heap::free(p);
  // Pointer asing (mis. hasil sys_alloc langsung) diabaikan, bukan korupsi.
  int local = 0;
  kyuzen_heap::free(&local);
  CHECK(live_total() == 0);
  CHECK(kyuzen_heap::realloc(&local, 128) == nullptr);
}

// ------------------------------------------------------------
// 9. calloc: nol + deteksi overflow.
// ------------------------------------------------------------
void test_calloc() {
  reset_mock();
  unsigned char *p = static_cast<unsigned char *>(kyuzen_heap::calloc(1000, 8));
  CHECK(p != nullptr);
  for (size_t i = 0; i < 8000; ++i) CHECK(p[i] == 0);
  kyuzen_heap::free(p);
  CHECK(kyuzen_heap::calloc(static_cast<size_t>(-1), 4) == nullptr);
  CHECK(kyuzen_heap::calloc(0, 8) == nullptr);  // ukuran 0 -> NULL (paritas)
  // calloc besar (lintas arena) ikut benar.
  unsigned int *big =
      static_cast<unsigned int *>(kyuzen_heap::calloc(1920u * 1080u, 4));
  CHECK(big != nullptr);
  CHECK(big[0] == 0 && big[1920u * 1080u - 1] == 0);
  kyuzen_heap::free(big);
}

// ------------------------------------------------------------
// 10. realloc: in-place, lintas arena, NULL, dan nol.
// ------------------------------------------------------------
void test_realloc() {
  reset_mock();
  unsigned char *p = static_cast<unsigned char *>(kyuzen_heap::alloc(1024));
  CHECK(p != nullptr);
  fill_pattern(p, 1024, 7);
  // Mengecil: pointer sama (in-place, paritas FreeListHeap::realloc).
  void *same = kyuzen_heap::realloc(p, 512);
  CHECK(same == p);
  CHECK(check_pattern(p, 512, 7));
  // Membesar lintas arena: isi lama harus ikut.
  const size_t big = 2u << 20;
  unsigned char *q = static_cast<unsigned char *>(kyuzen_heap::realloc(p, big));
  CHECK(q != nullptr);
  CHECK(q != p);  // pindah arena (1024 B tak bisa jadi 2 MiB di arena kecil)
  CHECK(check_pattern(q, 512, 7));
  fill_pattern(q, big, 8);
  CHECK(check_pattern(q, big, 8));
  // realloc(NULL, n) = malloc(n); realloc(p, 0) = free + NULL.
  unsigned char *r = static_cast<unsigned char *>(kyuzen_heap::realloc(nullptr, 64));
  CHECK(r != nullptr);
  CHECK(kyuzen_heap::realloc(r, 0) == nullptr);
  CHECK(kyuzen_heap::realloc(q, 0) == nullptr);
  CHECK(live_total() == 0);
}

// ------------------------------------------------------------
// 11. Kembalinya memori: arena khusus kosong -> sys_free; working set tetap.
// ------------------------------------------------------------
void test_memory_return() {
  reset_mock();
  void *small = kyuzen_heap::alloc(4096);
  CHECK(small != nullptr);
  const size_t mapped_small = kyuzen_heap::mapped_bytes();
  void *huge = kyuzen_heap::alloc(8u << 20);
  CHECK(huge != nullptr);
  CHECK(kyuzen_heap::mapped_bytes() > mapped_small);
  kyuzen_heap::free(huge);
  CHECK(kyuzen_heap::mapped_bytes() == mapped_small);  // dikembalikan ke uheap
  CHECK(g_region_frees == 1);
  kyuzen_heap::free(small);
  // Working set (arena deret geometris) sengaja dipertahankan untuk reuse.
  CHECK(kyuzen_heap::mapped_bytes() == kInitialArenaBytes);
  CHECK(g_bytes_live == kInitialArenaBytes);
}

// ------------------------------------------------------------
// 12. Aplikasi mini: campuran alokasi/bebas, semua dibebaskan -> nol blok
//     hidup, pola tetap utuh (deteksi korupsi/overlap).
// ------------------------------------------------------------
void test_mixed_workload() {
  reset_mock();
  const int N = 24;
  void *ptrs[N] = {nullptr};
  size_t sizes[N];
  for (int i = 0; i < N; ++i) {
    sizes[i] = static_cast<size_t>(64 + i * 37) * 1024;  // 64 KiB .. ~890 KiB
    ptrs[i] = kyuzen_heap::alloc(sizes[i]);
    CHECK(ptrs[i] != nullptr);
    fill_pattern(ptrs[i], sizes[i], static_cast<unsigned char>(i));
  }
  for (int i = 0; i < N; i += 2) {  // bebas separuh
    kyuzen_heap::free(ptrs[i]);
    ptrs[i] = nullptr;
  }
  for (int i = 1; i < N; i += 2)      // sisanya masih utuh
    CHECK(check_pattern(ptrs[i], sizes[i], static_cast<unsigned char>(i)));
  for (int i = 0; i < N; ++i) {
    if (ptrs[i] != nullptr) {
      kyuzen_heap::free(ptrs[i]);
      ptrs[i] = nullptr;
    }
  }
  CHECK(live_total() == 0);
  // Setelah semua bebas, permintaan kecil tak butuh arena baru.
  const size_t calls = g_region_calls;
  void *again = kyuzen_heap::alloc(256 * 1024);
  CHECK(again != nullptr);
  CHECK(g_region_calls == calls);
  kyuzen_heap::free(again);
}

}  // namespace

int main() {
  test_basic();
  test_growth();
  test_large();
  test_wallpaper_8mb();
  test_reuse();
  test_multi_arena_reuse();
  test_alignment();
  test_failure();
  test_calloc();
  test_realloc();
  test_memory_return();
  test_mixed_workload();

  reset_mock();
  CHECK(kyuzen_heap::arena_count() == 0);
  CHECK(g_bytes_live == 0);

  // Observasi memori (laporan Phase 9.5 §16): footprint tumbuh sesuai
  // kebutuhan, bukan reservasi di depan.
  printf("[heap] arena pertama %zu B (lazy, di ambil saat alokasi pertama)\n",
         kInitialArenaBytes);
  printf("[heap] deret geometris sampai %zu B, max satu permintaan %zu B\n",
         kMaxArenaBytes, kMaxAllocBytes);
  printf("libc heap phase9.5: OK\n");
  return 0;
}
