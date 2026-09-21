// ============================================================
// KyuzenOS — minimal C++ runtime/foundation (Phase 5)
//
// Menyediakan HANYA simbol yang benar-benar diemisikan clang++ untuk
// program C++ sederhana (-fno-exceptions -fno-rtti, freestanding).
// Audit kebutuhan: docs/design/audit-llvm-libc-22-freestanding.md §17.
//
// BUKAN libc++: tanpa std::string/vector/iostream/STL, tanpa exceptions,
// tanpa RTTI, tanpa unwinding, tanpa TLS, tanpa dynamic linking.
//
// Kebijakan (disengaja, minimal):
//   - new/delete → malloc/free LLVM libc (TIDAK ada allocator kedua;
//     arena + failure policy mengikuti libc: NULL saat arena habis).
//   - operator new (throwing) yang kehabisan memori → abort() — sama
//     seperti perilaku -fno-exceptions generik (tidak bisa throw,
//     tidak boleh return NULL dari throwing-new).
//   - Guard static-local = single-threaded (satu proses Kyuzen =
//     satu thread; lihat §17 — tanpa primitif sinkronisasi kernel).
//   - __cxa_atexit/__cxa_finalize/atexit didefinisikan DI SINI (bukan dari
//     libc.a): versi LLVM 22.1.8 hanya memfinalisasi pendaftar dso==NULL
//     sementara clang selalu mendaftar dengan dso=&__dso_handle — memakai
//     versi libc apa adanya membuat dtor global tidak pernah jalan.
//   - __dso_handle: satu DSO statis → objek sentinel milik runtime ini.
//   - Init/fini array: dijalankan port C (_start → kyuzen_libc_start_c
//     memanggil __init_array via weak symbol); file ini tidak menyentuh
//     startup.
//
// Kompilasi: freestanding BIASA melawan header SDK C (bukan flag internal
// libc — file ini konsumen libc, sama seperti app user).
// ============================================================

#include <stddef.h>  // size_t (header freestanding clang, bukan libc++)
#include <stdlib.h>  // malloc/free/abort (LLVM libc Phase 1)
#include <new>       // std::nothrow_t (libc++; SDK men-stage header ini §18)

// ------------------------------------------------------------
// DSO handle untuk __cxa_atexit(fn, obj, dso). Satu executable statis =
// satu DSO; __cxa_finalize mengabaikan nilainya di jalur ini.
// ------------------------------------------------------------
extern "C" void *__dso_handle = nullptr;

// ------------------------------------------------------------
// Allocation: new/new[] → malloc; delete/delete[] → free.
// Varian sized-delete (C++14 default clang) → free (ukuran diabaikan,
// freelist LLVM tidak membutuhkannya). Varian nothrow → NULL, bukan abort.
// new(0) meminta 1 byte agar hasilnya non-NULL (mengikuti praktik umum;
// malloc(0) LLVM boleh mengembalikan NULL yang akan disalahartikan gagal).
// ------------------------------------------------------------
void *operator new(size_t n) {
  if (n == 0)
    n = 1;
  void *p = malloc(n);
  if (p == nullptr)
    abort(); // -fno-exceptions: tidak bisa throw, tidak boleh return NULL
  return p;
}

void *operator new[](size_t n) { return operator new(n); }

void operator delete(void *p) noexcept {
  if (p != nullptr)
    free(p);
}

void operator delete[](void *p) noexcept { operator delete(p); }

void operator delete(void *p, size_t) noexcept { operator delete(p); }

void operator delete[](void *p, size_t) noexcept { operator delete(p); }

// Nothrow: dideklarasikan di <new> SDK (std::nothrow).
const std::nothrow_t std::nothrow{};
void *operator new(size_t n, const std::nothrow_t &) noexcept {
  if (n == 0)
    n = 1;
  return malloc(n);
}

void *operator new[](size_t n, const std::nothrow_t &t) noexcept {
  return operator new(n, t);
}

void operator delete(void *p, const std::nothrow_t &) noexcept {
  operator delete(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept {
  operator delete(p);
}

// ------------------------------------------------------------
// Guard static-local (Itanium C++ ABI, disederhanakan single-thread).
// Layout: 8 byte, byte pertama = flag inisialisasi.
// Acquire → 1 = panggil ctor; 0 = sudah diinisialisasi, lewati.
// Release menandai selesai. Abort tidak pernah terjadi tanpa exception
// (disediakan agar linker tidak mengeluh bila direferensikan).
// ------------------------------------------------------------
extern "C" int __cxa_guard_acquire(unsigned long long *guard) {
  return *reinterpret_cast<unsigned char *>(guard) == 0 ? 1 : 0;
}

extern "C" void __cxa_guard_release(unsigned long long *guard) {
  *reinterpret_cast<unsigned char *>(guard) = 1;
}

extern "C" void __cxa_guard_abort(unsigned long long *) {
  // Tanpa exception tidak ada jalur batal; abort menandai bug runtime.
  abort();
}

// ------------------------------------------------------------
// Pure virtual call = bug program (tidak ada dispatch valid ke sini).
// Pola yang sama dengan libs/widget/src/runtime/runtime.cpp (ODR: simbol
// ini hanya boleh didefinisikan SEKALI di seluruh program — milik runtime).
// ------------------------------------------------------------
extern "C" void __cxa_pure_virtual() { abort(); }

// ------------------------------------------------------------
// __cxa_atexit / __cxa_finalize / atexit — dimiliki runtime C++, BUKAN
// libc.a, dengan alasan yang terdokumentasi dan terverifikasi:
//
//   libc/src/stdlib/atexit.cpp (22.1.8):
//     void __cxa_finalize(void *dso) {
//       if (!dso) call_exit_callbacks(atexit_callbacks);
//     }
//   clang SELALU memanggil __cxa_atexit(dtor, obj, &__dso_handle) dengan
//   dso = &__dso_handle (NON-NULL) untuk setiap global ber-dtor. Akibatnya
//   memakai versi LLVM apa adanya = destruktor global TIDAK PERNAH jalan
//   (dibuktikan: "[phase5] global dtor ok" hilang). Versi LLVM hanya benar
//   untuk pendaftar dso==NULL.
//
//   Implementasi di bawah adalah Itanium ABI yang benar untuk SATU DSO
//   statis: finalize(d) menjalankan LIFO semua entri dengan dso yang cocok
//   (NULL = semua, sesuai standar). Kapasitas 64 entri, statis, tanpa lock
//   (satu thread per proses — model yang sama dengan guard § atas).
//
//   Definisi kuat di object reguler mengalahkan member archive (linker
//   tidak pernah menarik atexit.cpp.obj milik libc selama simbol-simbol
//   ini sudah terpenuhi — termasuk `atexit`, yang ikut disediakan agar
//   tidak terjadi definisi ganda bila app C++ memanggilnya). App C murni
//   tidak me-link file ini sehingga versi LLVM tetap berlaku di sana.
// ------------------------------------------------------------
namespace {
struct cxa_entry {
  void (*fn)(void *);
  void *arg;
  void *dso;
};
constexpr int CXA_MAX = 64;
cxa_entry cxa_list[CXA_MAX];
int cxa_count = 0;
} // namespace

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
  if (fn == nullptr || cxa_count >= CXA_MAX)
    return -1;
  cxa_list[cxa_count++] = {fn, arg, dso};
  return 0;
}

extern "C" void __cxa_finalize(void *dso) {
  // LIFO; entri yang didaftarkan dtor SELAMA finalisasi ikut terambil
  // oleh loop luar (count dibaca ulang tiap iterasi).
  while (cxa_count > 0) {
    int i = cxa_count - 1;
    while (i >= 0 && dso != nullptr && cxa_list[i].dso != dso)
      --i;
    if (i < 0)
      break;
    cxa_entry e = cxa_list[i];
    for (int j = i; j < cxa_count - 1; ++j)
      cxa_list[j] = cxa_list[j + 1];
    --cxa_count;
    e.fn(e.arg);
  }
}

extern "C" void __cxa_atexit_trampoline(void *p) {
  reinterpret_cast<void (*)()>(p)();
}

extern "C" int atexit(void (*fn)(void)) {
  if (fn == nullptr)
    return -1;
  return __cxa_atexit(__cxa_atexit_trampoline,
                      reinterpret_cast<void *>(fn), &__dso_handle);
}

// ------------------------------------------------------------
// __throw_bad_alloc — satu-satunya __throw_* yang TIDAK datang dari
// libcxxrt.a, dengan alasan: definisi hulu (new_helpers.cpp) berada satu
// file dengan `const nothrow_t nothrow` yang SUDAH didefinisikan di sini
// (Phase 5; mengompilasi file itu = definisi ganda). Perilaku no-exceptions
// disamakan dengan operator new di atas: gagal = abort() (keras, jujur,
// tanpa fake exception). __throw_length_error/out_of_range/dll datang dari
// stdexcept.cpp (→ verbose_abort) — lihat audit §18.
// ------------------------------------------------------------
#include <__config> // _LIBCPP_BEGIN_NAMESPACE_STD (namespace ikut konfigurasi)
_LIBCPP_BEGIN_NAMESPACE_STD
[[noreturn]] void __throw_bad_alloc() { abort(); }
_LIBCPP_END_NAMESPACE_STD
