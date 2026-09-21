// ============================================================
// KyuzenOS — LLVM libc 22.1.8 port layer (Phase 1: exit + errno + malloc)
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
//   4. `_start`                  — crt minimal Kyuzen (bukan bagian libc).
//   5. `__llvm_libc_stdio_cookie` + 3 objek cookie + `__llvm_libc_stdio_read`/
//      `__llvm_libc_stdio_write` — src/__support/OSUtil/baremetal/io.h.
//
// Arena malloc (desain Phase 1, lihat docs/design/audit-llvm-libc-22-freestanding.md §13):
//   LLVM `FreeListHeap` (src/__support/freelist_heap.h) adalah allocator di atas
//   SATU region contiguous dengan batas atas tetap; ctor default-nya menunjuk
//   `_end`..`__llvm_libc_heap_limit` (arena statis di dalam citra ELF) dan TIDAK
//   ada hook "minta tambahan memori". Karena itu tidak mungkin memetakan satu
//   syscall #9 per malloc(): yang benar adalah mengambil satu region dari uheap
//   Kyuzen sekali, lalu membiarkan algoritma freelist LLVM membagi-bagikannya.
//
//   `freelist_heap.cpp` bawaan libc (yang membuat instance statis `_end..limit`)
//   sengaja TIDAK ikut ter-link: definisi kuat `freelist_heap` di bawah ini
//   sudah memenuhi simbolnya, sehingga member archive itu tidak pernah ditarik
//   dan simbol `_end`/`__llvm_libc_heap_limit` tidak dibutuhkan sama sekali.
//   Dengan kata lain: tidak ada arena statis di citra, tidak ada allocator
//   kedua — hanya instance FreeListHeap di atas region sys_alloc(9).
//
// Referensi audit: docs/design/audit-llvm-libc-22-freestanding.md (§13).
// ============================================================

#include <errno.h>   // deklarasi publik hook errno (dicek compiler)
#include <stddef.h>  // size_t

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
#define KZ_SYS_EXIT_CODE 34    // RBX = status -> proc_exit(status)
#define KZ_SYS_READ      48    // RBX = fd, RCX = buf, RDX = count -> byte / -1
#define KZ_SYS_WRITE     49    // RBX = fd, RCX = buf, RDX = count -> byte / -1

static inline void *kyuzen_sys_alloc(size_t size) {
  void *ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)KZ_SYS_ALLOC), "b"(size)
                   : "memory");
  return ret;
}

// ------------------------------------------------------------
// Ukuran arena malloc. Bisa di-override saat kompilasi
// (`-DKYUZEN_LIBC_ARENA_BYTES=...`); 1 MiB = 256 halaman uheap.
//
// Plafon ini adalah konsekuensi API FreeListHeap 22.1.8 (tidak ada grow hook),
// bukan pilihan bebas: permintaan di atasnya mengembalikan NULL. Ruang alamat
// uheap sendiri jauh lebih besar (UHEAP_BASE 0x10000000 .. UHEAP_END 0x40000000),
// jadi menaikkan angka ini hanya soal berapa halaman fisik yang mau ditahan.
// ------------------------------------------------------------
#ifndef KYUZEN_LIBC_ARENA_BYTES
#define KYUZEN_LIBC_ARENA_BYTES (1u << 20)
#endif

// Status keluar bila arena tidak bisa diambil dari uheap (bukan kesalahan app,
// tapi tidak ada gunanya lanjut: malloc akan selalu NULL).
#define KZ_EXIT_ARENA_FAILED 70

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

namespace LIBC_NAMESPACE_DECL {

// Definisi kuat yang MENGGANTIKAN `freelist_heap.cpp` milik libc (simbol ini
// hanya boleh punya satu definisi di seluruh program; definisi di object
// reguler menang atas member archive). Null sampai `kyuzen_libc_heap_init()`
// dipanggil — `_start` selalu melakukannya sebelum `main`, jadi malloc tidak
// pernah melihat pointer kosong.
FreeListHeap *freelist_heap = nullptr;

// Storage untuk objek FreeListHeap (FreeListHeap tidak copyable: FreeStore
// menghapus copy/assign — lihat src/__support/freestore.h). Konstruksi in-place
// lewat placement new di bawah.
alignas(FreeListHeap) static unsigned char
    kyuzen_heap_storage[sizeof(FreeListHeap)];

static bool kyuzen_heap_ready = false;

// Ambil satu region contiguous dari uheap Kyuzen (syscall #9) dan bangun
// FreeListHeap di atasnya. Idempoten.
//
// Kegagalan syscall #9 (OOM task / task tanpa AS) TIDAK memunculkan allocator
// cadangan: program keluar dengan status KZ_EXIT_ARENA_FAILED. Alternatifnya
// adalah arena statis di .bss/citra — bertentangan dengan desain Phase 1
// ("bukan _end..__llvm_libc_heap_limit").
void kyuzen_libc_heap_init() {
  if (kyuzen_heap_ready)
    return;

  void *region = kyuzen_sys_alloc(KYUZEN_LIBC_ARENA_BYTES);
  if (region == nullptr)
    __llvm_libc_exit(KZ_EXIT_ARENA_FAILED);

  cpp::byte *begin = static_cast<cpp::byte *>(region);
  // uheap selalu mengembalikan awal halaman (UHEAP_BASE + kelipatan 4096),
  // jadi syarat alignment Block (MIN_ALIGN) sudah terpenuhi.
  freelist_heap = ::new (static_cast<void *>(kyuzen_heap_storage))
      FreeListHeap(cpp::span<cpp::byte>(begin, begin + KYUZEN_LIBC_ARENA_BYTES));
  kyuzen_heap_ready = true;
}

} // namespace LIBC_NAMESPACE_DECL

// ------------------------------------------------------------
// crt minimal. Kontrak proses KyuzenOS: e_entry dipanggil dengan RDI = argc,
// RSI = argv, RSP % 16 == 0, ELF statis tanpa PT_TLS/PT_DYNAMIC.
//
// `and $-16, %rsp` menormalkan stack; `call` lalu mendorong return address
// sehingga kyuzen_libc_start_c melihat RSP % 16 == 8 — keadaan ABI SysV yang
// benar untuk sebuah fungsi. Tidak ada .init_array/.fini_array di Phase 1
// (tidak ada global constructor di subset ini — lihat known limitations §13).
// ------------------------------------------------------------
extern "C" int main(int argc, char **argv);

extern "C" [[noreturn]] void kyuzen_libc_start_c(long argc, char **argv) {
  LIBC_NAMESPACE::kyuzen_libc_heap_init();
  __llvm_libc_exit(main(static_cast<int>(argc), argv));
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
