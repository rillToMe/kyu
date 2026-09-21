// ============================================================
// KyuzenOS — Phase 5 C++ runtime smoke app (tools/libc-phase5/sdk_smoke.cpp)
//
// Dibangun MURNI lewat Kyuzen C++ SDK (build/sdk/cpp + build/sdk/c):
//   clang++ -nostdinc++ -isystem sdk/cpp/include -isystem sdk/c/include
// File ini TIDAK boleh menyebut path implementasi LLVM maupun libc++ —
// diverifikasi Makefile (guard menolak bila ada rujukan ke dalam tree LLVM).
//
// ATURAN SDK (didokumentasikan, bukan keterbatasan tersembunyi):
//   - main BERLINKAGE C: `extern "C" int main(...)` (crt memanggil `main`).
//   - -fno-exceptions -fno-rtti selama Phase 5.
//
// Yang dibuktikan (setiap `ok` = assertion runtime eksak):
//   [phase5] global ctor ok   — .init_array dijalankan _start sblm main
//   [phase5] new/delete ok    — new/new[]/delete/delete[] + vtable + nothrow
//   [phase5] static local ok  — __cxa_guard_*: inisialisasi tepat sekali
//   [phase5] c coexistence ok — printf/malloc/strlen/strcmp + argv
//   [phase5] PASS             — akhir main
//   [phase5] global dtor ok   — __cxa_atexit → __cxa_finalize saat exit()
// ============================================================

#include <new> // std::nothrow + placement new (header SDK C++, bukan libc++)
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long kz_write_raw(int fd, const void *buf, unsigned long n) {
  long ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)49), "b"((long)fd), "c"(buf), "d"(n)
                   : "memory");
  return ret;
}

static void say_raw(const char *s) {
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  (void)kz_write_raw(1, s, n);
}

#define FAIL(tag)                                                              \
  do {                                                                         \
    say_raw("[phase5] FAIL ");                                                 \
    say_raw(tag);                                                              \
    say_raw("\n");                                                             \
    return 42;                                                                 \
  } while (0)

#define CHECK(cond, tag)                                                       \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(tag);                                                               \
  } while (0)

// ---- Global dengan ctor/dtor non-foldable (printf = side effect yang ----
// ---- tidak bisa di-constant-fold -O2 → memaksa .init_array + --------
// ---- __cxa_atexit/__dso_handle sungguhan, bukan inisialisasi statis. ----
struct Logger {
  int tag;
  Logger();
  ~Logger();
};
Logger::Logger() : tag(0xC005) {
  printf("[phase5] global ctor ok\n");
}
Logger::~Logger() {
  // Berjalan saat exit() → __cxa_finalize, SETELAH "[phase5] PASS".
  printf("[phase5] global dtor ok\n");
}
Logger global_logger;

// ---- Hierarki virtual (vtable murni data; tanpa RTTI/exception). ----
struct Shape {
  int id;
  Shape() : id(7) {}
  virtual ~Shape() {}
  virtual int area() { return id; }
};
struct Square : Shape {
  int side;
  Square() : side(6) {}
  int area() override { return side * side; }
};

// ---- Static local: guard memastikan tepat sekali. ----
static int static_init_runs = 0;
int *shared_slot() {
  static int *slot = nullptr;
  if (slot == nullptr) {
    static_init_runs++;
    static int backing = 1234;
    slot = &backing;
  }
  return slot;
}
int call_once_value() {
  static int v = ++static_init_runs * 100;
  return v;
}

// main WAJIB linkage C: crt (_start → kyuzen_libc_start_c) memanggil `main`.
extern "C" int main(int argc, char **argv) {
  CHECK(global_logger.tag == 0xC005, "global-ctor-tag");

  // ---- new/delete/new[]/delete[] + dispatch virtual ----
  {
    Shape *s = new Shape();
    Square *q = new Square();
    CHECK(s != nullptr && q != nullptr, "new-null");
    CHECK(s->area() == 7, "virtual-base");
    Shape *poly = q;
    CHECK(poly->area() == 36, "virtual-override");
    int *arr = new int[16];
    CHECK(arr != nullptr, "new-array-null");
    for (int i = 0; i < 16; i++)
      arr[i] = i * 3;
    CHECK(arr[0] == 0 && arr[15] == 45, "new-array-content");
    delete s;
    delete q;
    delete[] arr;
    // Nothrow + placement (header <new> SDK).
    int *nt = new (std::nothrow) int(99);
    CHECK(nt != nullptr && *nt == 99, "new-nothrow");
    delete nt;
    alignas(int) unsigned char buf[sizeof(int)];
    int *pl = new (buf) int(55);
    CHECK(pl == (void *)buf && *pl == 55, "new-placement");
    printf("[phase5] new/delete ok\n");
  }

  // ---- static local: guard init tepat sekali ----
  {
    int *a = shared_slot();
    int *b = shared_slot();
    CHECK(a == b && *a == 1234, "static-ptr");
    CHECK(static_init_runs == 1, "static-once-ptr");
    int v1 = call_once_value();
    int v2 = call_once_value();
    CHECK(v1 == 200 && v2 == 200, "static-value");
    CHECK(static_init_runs == 2, "static-once-val");
    printf("[phase5] static local ok\n");
  }

  // ---- Koeksistensi C: printf/malloc/strlen/strcmp + argv ----
  {
    CHECK(argc == 2, "argc");
    CHECK(argv != nullptr && argv[1] != nullptr, "argv-null");
    CHECK(strcmp(argv[1], "hello") == 0, "argv1");
    CHECK(strlen("kyuzen") == 6, "strlen");
    char *m = (char *)malloc(64);
    CHECK(m != nullptr, "malloc-null");
    strcpy(m, "c-and-cxx");
    CHECK(strcmp(m, "c-and-cxx") == 0, "malloc-str");
    free(m);
    printf("[phase5] argv0=%s\n", argv[0]);
    printf("[phase5] c coexistence ok\n");
  }

  printf("[phase5] PASS\n");
  return 0; // → exit() → __cxa_finalize → ~Logger → "[phase5] global dtor ok"
}
