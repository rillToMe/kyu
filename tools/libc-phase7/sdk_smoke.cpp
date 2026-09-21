// KyuzenOS — Phase 7 C++ Application SDK smoke app.
//
// Aplikasi normal yang hanya memakai boundary publik: header standar subset,
// header <kyuzen/...>, dan header C SDK. Tidak ada path LLVM/port/build di
// source ini; wrapper `kyuzen-c++` memiliki semua flag dan urutan link.
//
// Marker serial memakai pola fase lama agar harness QEMU tidak berubah.

#include <algorithm>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <kyuzen/app.hpp>
#include <kyuzen/config.hpp>
#include <kyuzen/panic.hpp>

static long kz_write_raw(int fd, const void* buf, unsigned long n) {
  long ret;
  __asm__ volatile("int $0x80"
                   : "=a"(ret)
                   : "a"((long)49), "b"((long)fd), "c"(buf), "d"(n)
                   : "memory");
  return ret;
}

static void say_raw(const char* s) {
  size_t n = 0;
  while (s[n] != '\0')
    n++;
  (void)kz_write_raw(1, s, n);
}

#define FAIL(tag)                                                            \
  do {                                                                       \
    say_raw("[phase7] FAIL ");                                               \
    say_raw(tag);                                                            \
    say_raw("\n");                                                           \
    return 42;                                                               \
  } while (0)

#define CHECK(cond, tag)                                                     \
  do {                                                                       \
    if (!(cond))                                                             \
      FAIL(tag);                                                             \
  } while (0)

namespace {

struct Tracker {
  int value;
  explicit Tracker(int x) : value(x) {}
  ~Tracker() { destructed++; }
  static int destructed;
};

int Tracker::destructed = 0;

int app_body(int argc, char** argv) {
  {
    std::vector<int> values{3, 1, 2};
    std::sort(values.begin(), values.end());
    CHECK(values.size() == 3 && values[0] == 1 && values[2] == 3, "sort");
    auto name = std::make_unique<std::string>("KyuzenOS");
    CHECK(name.get() != nullptr && *name == "KyuzenOS", "unique-string");
    std::string_view view(*name);
    CHECK(view.size() == 8 && view.substr(0, 6) == "Kyuzen", "string-view");
    printf("[phase7] libc++ ok\n");
  }

  {
    Tracker::destructed = 0;
    {
      Tracker local(7);
      CHECK(local.value == 7, "ctor");
      auto owned = std::make_unique<Tracker>(11);
      CHECK(owned->value == 11, "make-unique");
    }
    CHECK(Tracker::destructed == 2, "dtor");
    printf("[phase7] allocation ok\n");
  }

  CHECK(argc == 2, "argc");
  CHECK(argv != nullptr && argv[1] != nullptr, "argv-null");
  CHECK(strcmp(argv[1], "hello") == 0, "argv1");
  printf("[phase7] argv ok\n");
  return 0;
}

} // namespace

extern "C" int main(int argc, char** argv) {
  static_assert(kyuzen::sdk_major == 7, "SDK C++ Phase 7 diharapkan");
  static_assert(kyuzen::sdk_minor == 0, "versi minor SDK tak terduga");
  static_assert(std::is_same<decltype(&kyuzen::panic),
                             void (*)(const char*)>::value,
                "bentuk kyuzen::panic berubah");
  CHECK(kyuzen::sdk_language_version >= 201703L, "language");
  printf("[phase7] sdk config ok\n");

  CHECK(kyuzen::run(nullptr, argc, argv) == 1, "run-null");
  printf("[phase7] app api ok\n");

  int rc = kyuzen::run(app_body, argc, argv);
  if (rc != 0)
    return rc;
  printf("[phase7] PASS\n");
  return 0;
}
