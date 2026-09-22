// ============================================================
// KyuzenOS — LLVM libc 22.1.8 port layer (Phase 1–5: exit + errno + malloc +
// stdio + time + init_array walk · Phase 9.5: heap dinamis multi-arena +
// entry point malloc/free/calloc/realloc/aligned_alloc)
//
// Satu-satunya file yang tahu cara menyambungkan LLVM libc 22.1.8 (baremetal,
// target x86_64-pc-none-elf) ke KyuzenOS. Tidak ada header/kernel/libc Kyuzen
// yang diubah; semua akses OS lewat syscall int 0x80 yang sudah ada.
//
// Kontrak vendor LLVM libc 22.1.8 yang dipenuhi file ini:
//   1. `__llvm_libc_exit(int)`   — src/__support/OSUtil/baremetal/exit.cpp.
//   2. `__llvm_libc_errno()`     — LIBC_ERRNO_MODE_EXTERNAL (config baremetal).
//   3. `freelist_heap`           — instance heap yang dipakai malloc/calloc/
//                                  realloc/free/aligned_alloc baremetal.
//                                  Phase 9.5: heap nyata = daftar arena di
//                                  kyuzen_heap.hpp; variabel ini tinggal alias
//                                  kompatibilitas ke arena #0, dan kelima
//                                  entry point-nya disediakan file ini.
//   4. `_start`                  — crt minimal Kyuzen (bukan bagian libc);
//                                  Phase 5: + menjalankan .init_array C++
//                                  (weak symbol → app C tidak terpengaruh).
//   5. `__llvm_libc_stdio_cookie` + 3 objek cookie + `__llvm_libc_stdio_read`/
//      `__llvm_libc_stdio_write` — src/__support/OSUtil/baremetal/io.h.
//   6. `__llvm_libc_timespec_get_active/utc` — src/time/baremetal/clock.cpp
//      dan timespec_get.cpp (Phase 4: → syscall #14/#20).
//
// Runtime C++ (operator new/delete, guard, __dso_handle, pure_virtual) ada
// di file terpisah kyuzen_cxx_runtime.cpp — file ini hanya menambahkan walk
// init_array agar _start tunggal melayani C dan C++.
//
// Arena malloc — Phase 9.5: heap DINAMIS multi-arena (libs/c/libc-port/src/
// kyuzen_heap.hpp). Ringkasnya, dan alasan historisnya:
//
//   Phase 1..9 mengambil SATU region uheap (`sys_alloc(#9)`, ukuran
//   KYUZEN_LIBC_ARENA_BYTES = 1 MiB) dan membangun SATU `FreeListHeap` LLVM di
//   atasnya. `FreeListHeap` (src/__support/freelist_heap.h) adalah best-fit
//   freelist di atas satu region dengan batas atas tetap dan TIDAK punya grow
//   hook, jadi malloc/new di atas ~1 MiB selalu NULL:
//
//     new uint32_t[1920*1080]  (8.3 MiB buffer layar wallpaper Phase 9)
//         -> malloc -> FreeListHeap::allocate -> nullptr
//         -> operator new[] -> abort() -> ud2 -> INT 6 (BSOD)
//
//   Padahal sys_alloc(#9) sendiri sanggup 8.3 MiB (UHEAP_MAX_ALLOC 64 MiB di
//   include/uheap.h). Yang kurang bukan kernel, melainkan port ini: ia hanya
//   punya SATU arena. Phase 9.5 menggantinya dengan daftar arena (setiap arena
//   = satu region sys_alloc + satu instance FreeListHeap), tumbuh on-demand,
//   plus jalur arena khusus untuk permintaan besar. Algoritma freelist LLVM
//   tetap dipakai apa adanya — bukan allocator kedua/baru.
//
//   `freelist_heap.cpp` bawaan libc (yang membuat instance statis `_end..limit`)
//   sengaja TIDAK ikut ter-link: definisi kuat `freelist_heap` di bawah ini
//   sudah memenuhi simbolnya, sehingga member archive itu tidak pernah ditarik
//   dan simbol `_end`/`__llvm_libc_heap_limit` tidak dibutuhkan sama sekali.
//
//   Sejak Phase 9.5 entry point stdlib (malloc/free/calloc/realloc/
//   aligned_alloc) juga DISEDIAKAN di sini, bukan lagi dari libc.a. Alasannya
//   arsitektural: malloc.cpp/free.cpp/... baremetal hanyalah pembungkus satu
//   baris `freelist_heap->allocate(...)` (non-virtual, statically dispatched ke
//   SATU arena), sehingga routing multi-arena mustahil tanpa mengganti titik
//   masuk itu. Bukti bahwa ini aman (arsip libc.a 22.1.8, `llvm-nm`):
//     - tidak ada satu pun member yang mereferensikan `_ZN19__llvm_libc_22_1_8_
//       {6mallocEm,4freeEPv,6callocEmm,7reallocEPvm,13aligned_allocEmm}`;
//     - pemanggil internal libc memakai simbol C `malloc` (mis. strdup.cpp.obj);
//     - kelima member stdlib/baremetal adalah SATU-SATUNYA pemakai global
//       `freelist_heap` (tetap didefinisikan kuat di bawah untuk kompatibilitas).
//   Karena definisi kuat di object reguler (crt.o, selalu lebih dulu dari
//   libc.a pada urutan link SDK) sudah memenuhi simbolnya, kelima member itu
//   tidak pernah ditarik — dan bila kelak ada member libc baru yang memanggil
//   `malloc` dari dalam namespace libc (mis. asprintf/vasprintf_internal.h),
//   alias namespace-nya pun sudah disediakan di bagian bawah file ini.
//
// Referensi audit: docs/design/audit-llvm-libc-22-freestanding.md (§13).
// ============================================================

#include <errno.h>   // deklarasi publik hook errno (dicek compiler)
#include <stddef.h>  // size_t

#include "kyuzen_heap.hpp"  // heap dinamis multi-arena (Phase 9.5)
#include "src/__support/CPP/new.h"  // placement new gaya libc (pengganti <new>)
#include "src/__support/CPP/span.h"
#include "src/__support/freelist_heap.h"
#include "src/__support/macros/config.h"
// Deklarasi hook stdio vendor (dicek compiler: tipe cookie + tanda tangan
// read/write harus sama persis dengan yang dipakai objek libc).
#include "src/__support/OSUtil/io.h"

// ------------------------------------------------------------
// ABI syscall KyuzenOS (kernel/syscall.c): int 0x80
//   RAX = nomor, RBX/RCX/RDX/RSI/RDI = arg1..5, hasil di RAX.
// ------------------------------------------------------------
#define KZ_SYS_ALLOC      9    // RBX = ukuran byte -> alamat user / 0
#define KZ_SYS_FREE      10    // RBX = alamat region (base) -> bebas ke pmm
#define KZ_SYS_EXIT_CODE 34    // RBX = status -> proc_exit(status)
#define KZ_SYS_READ      48    // RBX = fd, RCX = buf, RDX = count -> byte / -1
#define KZ_SYS_WRITE     49    // RBX = fd, RCX = buf, RDX = count -> byte / -1
#define KZ_SYS_UPTIME    14    // -> ms sejak boot (timer_get_ms, uint64, no wrap)
#define KZ_SYS_GET_TIME  20    // RBX = ptr uint32_t[6] -> [thn,bln,hari,jam,menit,detik]

static inline void *kyuzen_sys_alloc(size_t size) {
  void *ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)KZ_SYS_ALLOC), "b"(size)
                   : "memory");
  return ret;
}

// Bebaskan region uheap milik task ini (kernel/mm/uheap.c mencocokkan BASE region;
// pointer asing/stale diabaikan kernel, bukan panic).
static inline void kyuzen_sys_free(void *region) {
  __asm__ volatile("int $0x80"
                   :
                   : "a"((long)KZ_SYS_FREE), "b"(region)
                   : "memory");
}

// ------------------------------------------------------------
// Knob heap (semua di kyuzen_heap.hpp, bisa di-override dengan -D):
//   KYUZEN_LIBC_ARENA_BYTES      ukuran arena pertama + satuan deret (1 MiB)
//   KYUZEN_LIBC_ARENA_MAX_BYTES  plafon deret geometris (4 MiB)
//   KYUZEN_LIBC_MAX_ARENAS       jumlah slot tabel arena (32)
//   KYUZEN_LIBC_MAX_ALLOC_BYTES  plafon satu permintaan = UHEAP_MAX_ALLOC (64 MiB)
// Tidak ada lagi status keluar "arena gagal": arena diambil LAZY saat alokasi
// pertama, dan kegagalannya = malloc mengembalikan NULL (bukan exit saat boot).
// ------------------------------------------------------------

// ------------------------------------------------------------
// errno — LIBC_ERRNO_MODE_EXTERNAL: libc tidak menyimpan errno sendiri, ia
// memanggil hook ini (`libc_errno.cpp`: `*__llvm_libc_errno() = a`). Satu
// variabel global per proses; TANPA TLS (Phase 1).
// ------------------------------------------------------------
static int kyuzen_errno_global = 0;

extern "C" int *__llvm_libc_errno() noexcept { return &kyuzen_errno_global; }

// ------------------------------------------------------------
// exit — satu-satunya jalur keluar proses di Phase 1. Dipakai oleh
// `_Exit()`, `exit()`, `abort()`, dan `_start` di bawah.
// ------------------------------------------------------------
extern "C" [[noreturn]] void __llvm_libc_exit(int status) {
  __asm__ volatile("int $0x80"
                   :
                   : "a"((long)KZ_SYS_EXIT_CODE), "b"((long)status)
                   : "memory");
  __builtin_unreachable();
}

// ------------------------------------------------------------
// stdio — hook vendor baremetal (`src/__support/OSUtil/baremetal/io.h`):
//
//   struct __llvm_libc_stdio_cookie;   // opaque, isi bebas (milik vendor)
//   __llvm_libc_stdin_cookie / _stdout_cookie / _stderr_cookie
//   ssize_t __llvm_libc_stdio_read(void *cookie, char *buf, size_t size);
//   ssize_t __llvm_libc_stdio_write(void *cookie, const char *buf, size_t size);
//
// Semantik return yang diharapkan libc (`baremetal/file_internal.h`):
// byte yang ditransfer, atau negatif `-errno` saat gagal (dijadikan
// `libc_errno` oleh pemanggil). Tidak ada buffering di libc baremetal:
// setiap fwrite/fputc langsung memanggil hook; printf memakai buffer stack
// 1024 B (FlushingBuffer) lalu flush — jadi tidak ada syarat flush/seek/
// close/file-positioning apa pun dari sisi vendor.
//
// Di baremetal, FILE* ADALAH alamat cookie (`OSUtil/baremetal/io.cpp`:
// `stdin = (FILE*)&__llvm_libc_stdin_cookie`, dst.), jadi `cookie` yang
// masuk selalu salah satu dari tiga objek di bawah. Cookie tak dikenal
// (mis. fprintf ke FILE* asing) ditolak dengan -EBADF, bukan ditebak.
//
// ssize_t LLVM 22.1.8 = `__PTRDIFF_TYPE__` = long di x86_64 LP64; hook
// dideklarasikan dengan `long` agar port tidak bergantung pada header
// generated (ABI identik, linkage C sehingga tidak ada masalah mangling).
//
// Catatan errno: kernel mengembalikan -1 polos (bukan -errno) saat syscall
// gagal, sehingga `libc_errno` sesudah stdio yang gagal = 1. Deteksi gagal
// (EOF/0/ret < 0) tetap benar; hanya kode errno-nya tidak presisi. Tidak
// bisa diperbaiki tanpa ubah kernel — di luar scope (lihat audit §14).
// ------------------------------------------------------------
// `struct __llvm_libc_stdio_cookie` WAJIB didefinisikan di dalam namespace
// libc (`io.h` mendeklarasikannya di sana): definisi di global scope adalah
// tipe yang berbeda dan ditolak compiler. Objek cookie ber-linkage C
// (nama tak ter-mangle, cocok dengan yang dirujuk `io.cpp.obj`), tipenya
// memakai nama terkualifikasi namespace.
namespace LIBC_NAMESPACE_DECL {
struct __llvm_libc_stdio_cookie {
  int fd;
};
} // namespace LIBC_NAMESPACE_DECL

extern "C" struct LIBC_NAMESPACE::__llvm_libc_stdio_cookie
    __llvm_libc_stdin_cookie = {0};
extern "C" struct LIBC_NAMESPACE::__llvm_libc_stdio_cookie
    __llvm_libc_stdout_cookie = {1};
extern "C" struct LIBC_NAMESPACE::__llvm_libc_stdio_cookie
    __llvm_libc_stderr_cookie = {2};

static inline long kyuzen_sys_read(int fd, char *buf, unsigned long size) {
  long ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)KZ_SYS_READ), "b"((long)fd),
                     "c"(buf), "d"(size)
                   : "memory");
  return ret;
}

static inline long kyuzen_sys_write(int fd, const char *buf,
                                    unsigned long size) {
  long ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)KZ_SYS_WRITE), "b"((long)fd),
                     "c"(buf), "d"(size)
                   : "memory");
  return ret;
}

extern "C" long __llvm_libc_stdio_read(void *cookie, char *buf,
                                       unsigned long size) {
  if (cookie != static_cast<void *>(&__llvm_libc_stdin_cookie))
    return -9; // -EBADF: hanya stdin yang bisa dibaca
  if (size == 0)
    return 0;
  if (buf == nullptr)
    return -9; // -EBADF
  long n = kyuzen_sys_read(0, buf, size);
  if (n < 0)
    return n; // kernel: -1 polos (lihat catatan errno di atas)
  return n;
}

extern "C" long __llvm_libc_stdio_write(void *cookie, const char *buf,
                                        unsigned long size) {
  int fd;
  if (cookie == static_cast<void *>(&__llvm_libc_stdout_cookie))
    fd = 1;
  else if (cookie == static_cast<void *>(&__llvm_libc_stderr_cookie))
    fd = 2;
  else
    return -9; // -EBADF: stdin tidak bisa ditulis, cookie asing ditolak
  if (size == 0)
    return 0;
  if (buf == nullptr)
    return -9; // -EBADF
  long n = kyuzen_sys_write(fd, buf, size);
  if (n < 0)
    return n;
  return n;
}

// ------------------------------------------------------------
// time — hook vendor baremetal Phase 4 (`src/time/baremetal/clock.cpp` dan
// `timespec_get.cpp` mendeklarasikan persis):
//
//   extern "C" bool __llvm_libc_timespec_get_active(struct timespec *ts);
//   extern "C" bool __llvm_libc_timespec_get_utc(struct timespec *ts);
//
// `struct timespec` LLVM 22.1.8 x86_64 = { time_t tv_sec (64-bit),
// long tv_nsec } (`libc/include/llvm-libc-types/struct_timespec.h`). Port
// mendefinisikan struct ABI-identik sendiri (pola yang sama dengan hook stdio
// yang memakai `long`/`unsigned long` alih-alih ssize_t/size_t) agar tidak
// bergantung pada header generated.
//
// Pemetaan (tanpa syscall baru, tanpa klaim palsu):
//   active (monotonik, dipakai clock()) → #14 uptime ms (timer_get_ms,
//            uint64: overflow ≈ 584 juta tahun — tidak perlu khawatir wrap).
//            Uptime BUKAN wall clock: nol saat boot, tidak ada zona waktu.
//   utc (kalender, dipakai timespec_get(TIME_UTC)) → #20 RTC yang mengisi
//            uint32_t[6] = [tahun,bulan,hari,jam,menit,detik]. Konversi
//            sipil→epoch memakai days-from-civil (Howard Hinnant, integer
//            murni, tanpa tabel). Presisi detik (tv_nsec = 0).
//
// Kualifikasi RTC (drivers/rtc.c, jujur didokumentasikan, bukan diperbaiki di
// sini karena kernel tidak boleh diubah fase ini):
//   - CMOS dibaca sebagai UTC lalu +7 jam (WIB); jam yang overflow dibungkus
//     mod 24 TANPA carry ke hari. Konversi di bawah setia pada field yang
//     dilaporkan — di sekitar tengah malam WIB bisa selisih satu hari.
//   - Tahun = BCD + 2000 (rentang efektif 2000–2099).
// ------------------------------------------------------------
struct kyuzen_timespec {
  long tv_sec;
  long tv_nsec;
};

static inline unsigned long long kyuzen_sys_uptime_ms() {
  unsigned long long ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)KZ_SYS_UPTIME)
                   : "memory");
  return ret;
}

extern "C" bool __llvm_libc_timespec_get_active(struct kyuzen_timespec *ts) {
  if (ts == nullptr)
    return false;
  unsigned long long ms = kyuzen_sys_uptime_ms();
  ts->tv_sec = static_cast<long>(ms / 1000ULL);
  ts->tv_nsec = static_cast<long>((ms % 1000ULL) * 1000000ULL);
  return true;
}

extern "C" bool __llvm_libc_timespec_get_utc(struct kyuzen_timespec *ts) {
  if (ts == nullptr)
    return false;
  unsigned int civil[6] = {0, 0, 0, 0, 0, 0};
  __asm__ volatile("int $0x80"
                   :
                   : "a"((long)KZ_SYS_GET_TIME), "b"(civil)
                   : "memory");
  unsigned long year = civil[0], mon = civil[1], day = civil[2];
  unsigned long hour = civil[3], min = civil[4], sec = civil[5];
  // Validasi struktural (bukan wall-clock host): tolak field mustahil agar
  // timespec_get mengembalikan 0 alih-alih epoch sampah.
  if (year < 2000 || year > 2100 || mon < 1 || mon > 12 || day < 1 ||
      day > 31 || hour > 23 || min > 59 || sec > 60)
    return false;
  // days-from-civil → hari sejak 1970-01-01 (integer murni).
  long y = static_cast<long>(mon <= 2 ? year - 1 : year);
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned long yoe = static_cast<unsigned long>(y - era * 400);
  unsigned long mp = (mon + 9) % 12;
  unsigned long doy = (153 * mp + 2) / 5 + day - 1;
  unsigned long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097 + static_cast<long>(doe) - 719468;
  ts->tv_sec =
      days * 86400L +
      static_cast<long>(hour * 3600UL + min * 60UL + sec);
  ts->tv_nsec = 0;
  return true;
}

// ------------------------------------------------------------
// Backing store heap: satu region uheap per arena.
//
// syscall #9 mengembalikan awal halaman (UHEAP_BASE + kelipatan 4096) dengan
// 1 halaman guard tak ter-map antar region, dan #10 (sys_free) memvalidasi
// kepemilikan lewat BASE region (kernel/mm/uheap.c: uheap_free cocokkan `base`),
// jadi pointer yang dikembalikan harus persis pointer yang diterima.
//
// Kegagalan = NULL: heap menolak permintaan itu, TIDAK mematikan proses (beda
// dengan Phase 1..9 yang exit(70) saat arena boot gagal diambil).
// ------------------------------------------------------------
namespace kyuzen_heap {

void *region_alloc(size_t bytes) { return kyuzen_sys_alloc(bytes); }
void region_free(void *region) { kyuzen_sys_free(region); }

} // namespace kyuzen_heap

namespace LIBC_NAMESPACE_DECL {

// Definisi kuat yang MENGGANTIKAN `freelist_heap.cpp` milik libc (simbol ini
// hanya boleh punya satu definisi di seluruh program; definisi di object
// reguler menang atas member archive).
//
// Sejak Phase 9.5 heap nyata hidup di kyuzen_heap.hpp (daftar arena); variabel
// ini hanya kompatibilitas simbol dan selalu disinkronkan ke arena #0
// (kyuzen_heap::detail::sync_libc_global). Null selama belum ada arena —
// artinya belum ada alokasi sama sekali, bukan heap yang tak siap.
FreeListHeap *freelist_heap = nullptr;

// Dipanggil `_start` sebelum `main` (kontrak Phase 1). Sejak Phase 9.5 heap
// LAZY: tidak ada sys_alloc saat boot, arena pertama diambil saat alokasi
// pertama. Fungsi sengaja tetap ada supaya crt tetap satu jalur dan app yang
// tidak pernah malloc tidak menahan satu halaman pun.
void kyuzen_libc_heap_init() {}

} // namespace LIBC_NAMESPACE_DECL

// ------------------------------------------------------------
// Entry point stdlib (Phase 9.5): malloc/free/calloc/realloc/aligned_alloc.
//
// Semantik SETIA pada versi baremetal LLVM 22.1.8 yang digantikan (lihat
// src/stdlib/baremetal/*.cpp) supaya perilaku app tidak berubah selain
// hilangnya plafon 1 MiB:
//   - size 0 -> NULL (bukan pointer unik); errno TIDAK disentuh;
//   - operator new/new[] (kyuzen_cxx_runtime.cpp) memanggil malloc ini, jadi
//     jalur C++ ikut membaik tanpa perubahan terpisah;
//   - free(ptr asing) = no-op (perilakunya UB di C; no-op tak bisa merusak).
//
// `noexcept` disamakan dengan deklarasi hdr/func/*.h libc (sudah ikut lewat
// CPP/new.h) — deklarasi ulang tanpa noexcept akan ditolak compiler.
// ------------------------------------------------------------
extern "C" void *malloc(size_t size) noexcept {
  return kyuzen_heap::alloc(size);
}

extern "C" void free(void *ptr) noexcept { kyuzen_heap::free(ptr); }

extern "C" void *calloc(size_t num, size_t size) noexcept {
  return kyuzen_heap::calloc(num, size);
}

extern "C" void *realloc(void *ptr, size_t size) noexcept {
  return kyuzen_heap::realloc(ptr, size);
}

extern "C" void *aligned_alloc(size_t alignment, size_t size) {
  return kyuzen_heap::aligned_alloc(alignment, size);
}

// Alias namespace libc: LLVM_LIBC_FUNCTION mendefinisikan `malloc` C *dan*
// `LIBC_NAMESPACE::malloc` (lihat src/__support/common.h, alias gnu). Tanpa
// definisi ini, member libc yang memanggil malloc dari dalam namespace (mis.
// vasprintf_internal.h) akan menarik malloc.cpp.obj → tabrakan definisi
// `malloc` dengan milik port. Implementasinya memakai `::malloc` (bukan
// rekursi ke dirinya sendiri).
namespace LIBC_NAMESPACE_DECL {

void *malloc(size_t size) noexcept { return ::malloc(size); }
void free(void *ptr) noexcept { ::free(ptr); }
void *calloc(size_t num, size_t size) noexcept { return ::calloc(num, size); }
void *realloc(void *ptr, size_t size) noexcept { return ::realloc(ptr, size); }
void *aligned_alloc(size_t alignment, size_t size) {
  return ::aligned_alloc(alignment, size);
}

} // namespace LIBC_NAMESPACE_DECL

// ------------------------------------------------------------
// crt minimal. Kontrak proses KyuzenOS: e_entry dipanggil dengan RDI = argc,
// RSI = argv, RSP % 16 == 0, ELF statis tanpa PT_TLS/PT_DYNAMIC.
//
// `and $-16, %rsp` menormalkan stack; `call` lalu mendorong return address
// sehingga kyuzen_libc_start_c melihat RSP % 16 == 8 — keadaan ABI SysV yang
// benar untuk sebuah fungsi.
//
// Phase 5: SEBELUM main, _start menjalankan constructor global C++ dari
// .init_array (linker script libs/cpp; app C tidak punya section ini).
// Simbol dibaca via weak reference: app C melihat start==end==NULL dan
// melewati loop — perilaku C IDENTIK dengan Phase 1–4 (bukti: regresi
// [phase1..4] PASS tak berubah).
//
// Phase 5b: return dari main MASUK exit(), bukan langsung ke hook
// __llvm_libc_exit. exit() = __cxa_finalize + hook. Untuk app C ini identik
// (daftar finalizer kosong → finalize no-op; bukti: regresi tak berubah).
// Untuk app C++ inilah yang menjalankan dtor global: memotong jalur ini
// (return → hook langsung) membuat dtor tidak pernah jalan meskipun
// terdaftar benar. compiler mendaftarkan dtor via __cxa_atexit (cxxrt.o di
// app C++, libc.a di app C) — teardown = LIFO atexit yang benar.
// ------------------------------------------------------------
extern "C" int main(int argc, char **argv);
extern "C" [[noreturn]] void exit(int status); // LLVM libc: finalize + hook

// Fungsi init C++: void(int, char**, char**) menurut konvensi; sebagian besar
// mengabaikan argumen.
//
// Simbol __init_array_start/end didefinisikan linker script (libs/cpp).
// Dideklarasikan sebagai data weak: &simbol = alamatnya (0 bila tak
// didefinisikan — kasus app C yang memakai script libs/c). Loop membandingkan
// ALAMAT, tidak pernah me-dereference batas akhir (isi setelah section bisa
// nol/apa saja — membacanya sebagai syarat akan melewatkan array 1-entri).
typedef void (*kyuzen_init_fn_t)(int, char **, char **);
extern "C" void *__init_array_start __attribute__((weak));
extern "C" void *__init_array_end __attribute__((weak));

extern "C" [[noreturn]] void kyuzen_libc_start_c(long argc, char **argv) {
  LIBC_NAMESPACE::kyuzen_libc_heap_init();
  kyuzen_init_fn_t *fn =
      reinterpret_cast<kyuzen_init_fn_t *>(&__init_array_start);
  kyuzen_init_fn_t *end =
      reinterpret_cast<kyuzen_init_fn_t *>(&__init_array_end);
  for (; fn != end; ++fn) {
    if (*fn != nullptr)
      (*fn)(static_cast<int>(argc), argv, nullptr);
  }
  exit(main(static_cast<int>(argc), argv));
}

__asm__(".text\n"
        ".globl _start\n"
        ".type _start, @function\n"
        "_start:\n"
        "  xorl %ebp, %ebp\n"
        "  andq $-16, %rsp\n"
        "  call kyuzen_libc_start_c\n"
        "  ud2\n"
        ".size _start, .-_start\n");
