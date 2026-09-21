// ============================================================
// KyuzenOS — Phase 6 libc++ subset smoke app (tools/libc-phase6/sdk_smoke.cpp)
//
// Dibangun MURNI lewat Kyuzen C++ SDK (build/sdk/cpp/include — SATU-SATUNYA
// include C++; memcpy internal libc++ TIDAK terlihat app). File ini TIDAK
// boleh menyebut path implementasi LLVM maupun header di luar subset Phase 6
// (array/algorithm/memory/string/string_view/type_traits/utility/vector +
// header C SDK) — diverifikasi Makefile.
//
// Setiap `ok` = assertion runtime eksak (CMP/string/memcmp, bukan "link ok").
// FAIL memakai syscall tulis mentah #49 agar terlihat walau stdio rusak.
// Destruktor non-trivial diverifikasi via counter global (akhir main,
// SEBELUM PASS — dtor unique_ptr/vector lokal; dtor global via exit
// seperti Phase 5).
// ============================================================

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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
    say_raw("[phase6] FAIL ");                                                 \
    say_raw(tag);                                                              \
    say_raw("\n");                                                             \
    return 42;                                                                 \
  } while (0)

#define CHECK(cond, tag)                                                       \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(tag);                                                               \
  } while (0)

// Pelacak dtor untuk objek non-trivial (vector<Bar> + unique_ptr).
static int dtor_runs = 0;
struct Bar {
  int v;
  Bar() : v(0) {}
  Bar(int x) : v(x) {}
  ~Bar() { dtor_runs++; }
};

extern "C" int main(int argc, char **argv) {
  // ---- type_traits (compile-time; static_assert = bukti build) ----
  {
    static_assert(std::is_same<int, int>::value, "is_same");
    static_assert(!std::is_same<int, long>::value, "is_same-neg");
    static_assert(std::is_trivial<int>::value, "is_trivial");
    static_assert(!std::is_trivial<Bar>::value, "is_trivial-neg");
    static_assert(std::is_trivially_copyable<int>::value, "trivial-copy");
    static_assert(std::is_move_constructible<std::string>::value, "move-ctor");
    static_assert(std::is_copy_constructible<std::vector<int>>::value,
                  "copy-ctor");
    static_assert(std::is_same<std::decay<const int &>::type, int>::value,
                  "decay");
    static_assert(
        std::is_same<std::remove_cv<const volatile int>::type, int>::value,
        "remove_cv");
    printf("[phase6] type_traits ok\n");
  }

  // ---- array ----
  {
    std::array<int, 5> a = {5, 1, 4, 2, 3};
    CHECK(a.size() == 5, "array-size");
    CHECK(!a.empty(), "array-empty");
    CHECK(a[0] == 5 && a[4] == 3, "array-index");
    CHECK(a.front() == 5 && a.back() == 3, "array-front-back");
    CHECK(a.at(2) == 4, "array-at");
    CHECK(a.data()[1] == 1, "array-data");
    int sum = 0;
    for (int x : a)
      sum += x;
    CHECK(sum == 15, "array-iter");
    std::array<int, 0> z;
    CHECK(z.empty() && z.size() == 0, "array-zero");
    printf("[phase6] array ok\n");
  }

  // ---- string_view (tanpa alokasi) ----
  {
    std::string_view v = "hello-world";
    CHECK(v.size() == 11, "sv-size");
    CHECK(!v.empty(), "sv-empty");
    CHECK(v[0] == 'h' && v[10] == 'd', "sv-index");
    CHECK(v.data()[1] == 'e', "sv-data");
    CHECK(v.compare("hello-world") == 0, "sv-compare-eq");
    CHECK(v.compare("hello") > 0, "sv-compare-gt");
    CHECK(v.find("world") == 6, "sv-find");
    CHECK(v.find("zzz") == std::string_view::npos, "sv-find-miss");
    CHECK(v.find('w') == 6, "sv-find-char");
    std::string_view sub = v.substr(6, 5);
    CHECK(sub == "world", "sv-substr");
    std::string_view tail = v.substr(6);
    CHECK(tail.size() == 5 && tail == "world", "sv-substr-tail");
    size_t n = 0;
    for (char c : v)
      n += (size_t)(c != '\0');
    CHECK(n == 11, "sv-iter");
    printf("[phase6] string_view ok\n");
  }

  // ---- string ----
  {
    std::string s("kyuzen");
    CHECK(s.size() == 6 && !s.empty(), "str-size");
    CHECK(s[0] == 'k' && s[5] == 'n', "str-index");
    CHECK(s.front() == 'k' && s.back() == 'n', "str-front-back");
    CHECK(s.data()[1] == 'y' && s.c_str()[6] == '\0', "str-data-cstr");
    std::string cpy = s; // copy
    CHECK(cpy == s && cpy.c_str() != s.c_str(), "str-copy");
    std::string mv = std::move(cpy); // move
    CHECK(mv == "kyuzen", "str-move");
    std::string from_view = std::string(std::string_view("abc"));
    CHECK(from_view == "abc", "str-from-view");
    s += "-os";
    CHECK(s == "kyuzen-os", "str-pluseq-cstr");
    s.append("!");
    CHECK(s.size() == 10 && s.back() == '!', "str-append");
    std::string more;
    more += s;
    CHECK(more.compare("kyuzen-os!") == 0, "str-pluseq-str");
    CHECK(s.find("os") == 7, "str-find");
    CHECK(s.find("zzz") == std::string::npos, "str-find-miss");
    size_t m = 0;
    for (char c : s)
      m += (size_t)(c != '\0');
    CHECK(m == 10, "str-iter");
    s.clear();
    CHECK(s.empty() && s.size() == 0, "str-clear");
    // String panjang (> SSO 22 char) = alokasi heap sungguhan.
    std::string big("0123456789abcdefghijklmnopqrstuvwxyz");
    CHECK(big.size() == 36, "str-big-size");
    CHECK(big.front() == '0' && big.back() == 'z', "str-big-content");
    CHECK(big.find("xyz") == 33, "str-big-find");
    printf("[phase6] string ok\n");
  }

  // ---- vector<int> ----
  {
    std::vector<int> v;
    CHECK(v.empty() && v.size() == 0 && v.capacity() == 0, "vec-init");
    v.reserve(16);
    CHECK(v.capacity() >= 16 && v.empty(), "vec-reserve");
    for (int i = 0; i < 10; i++)
      v.push_back(i * i);
    CHECK(v.size() == 10, "vec-push-size");
    CHECK(v[0] == 0 && v[9] == 81, "vec-index");
    CHECK(v.front() == 0 && v.back() == 81, "vec-front-back");
    CHECK(v.data()[5] == 25, "vec-data");
    v.emplace_back(100);
    CHECK(v.size() == 11 && v.back() == 100, "vec-emplace");
    int sum = 0;
    for (int x : v)
      sum += x;
    CHECK(sum == 385, "vec-iter"); // 0+1+4+...+81+100
    v.pop_back();
    CHECK(v.size() == 10 && v.back() == 81, "vec-pop");
    v.resize(12, 7);
    CHECK(v.size() == 12 && v[10] == 7 && v[11] == 7, "vec-resize-up");
    v.resize(3);
    CHECK(v.size() == 3 && v[2] == 4, "vec-resize-down");
    v.clear();
    CHECK(v.empty() && v.size() == 0, "vec-clear");
    printf("[phase6] vector ok\n");
  }

  // ---- vector<Bar> (non-trivial; dtor terhitung) ----
  {
    dtor_runs = 0;
    {
      std::vector<Bar> w;
      w.reserve(4);
      w.emplace_back(1);
      w.emplace_back(2);
      w.push_back(Bar(3));
      CHECK(w.size() == 3, "vecbar-size");
      CHECK(w[0].v == 1 && w[2].v == 3, "vecbar-content");
      int sv = 0;
      for (const Bar &b : w)
        sv += b.v;
      CHECK(sv == 6, "vecbar-iter");
    } // w hancur di sini: 3 elemen + sementara Bar(3) = dtor berjalan
    CHECK(dtor_runs >= 3, "vecbar-dtor");
    printf("[phase6] vector-nontrivial ok\n");
  }

  // ---- memory: unique_ptr ----
  {
    dtor_runs = 0;
    {
      auto p = std::make_unique<Bar>(41);
      CHECK(p.get() != nullptr, "up-get");
      CHECK((*p).v == 41 && p->v == 41, "up-deref");
      CHECK((bool)p, "up-bool");
      std::unique_ptr<Bar> q = std::move(p); // move
      CHECK(p.get() == nullptr && !p, "up-move-src");
      CHECK(q->v == 41, "up-move-dst");
      Bar *raw = q.release();
      CHECK(raw != nullptr && !q, "up-release");
      CHECK(raw->v == 41, "up-release-val");
      q.reset(raw);
      CHECK(q.get() == raw, "up-reset");
      q.reset();
      CHECK(q.get() == nullptr, "up-reset-null");
      CHECK(dtor_runs == 1, "up-reset-dtor"); // dtor dari reset()
      auto r = std::make_unique<Bar>(7);
      (void)r;
    } // r hancur → dtor ke-2
    CHECK(dtor_runs == 2, "up-scope-dtor");
    // unique_ptr<int> (trivial).
    {
      auto i = std::make_unique<int>(1234);
      CHECK(*i == 1234, "up-int");
    }
    printf("[phase6] memory ok\n");
  }

  // ---- algorithm ----
  {
    std::vector<int> v = {5, 1, 4, 2, 3};
    std::sort(v.begin(), v.end());
    const int want[] = {1, 2, 3, 4, 5};
    CHECK(std::equal(v.begin(), v.end(), want), "alg-sort");
    auto it = std::find(v.begin(), v.end(), 4);
    CHECK(it != v.end() && *it == 4, "alg-find");
    CHECK(std::find(v.begin(), v.end(), 99) == v.end(), "alg-find-miss");
    auto jt = std::find_if(v.begin(), v.end(), [](int x) { return x > 3; });
    CHECK(jt != v.end() && *jt == 4, "alg-find-if");
    std::array<int, 5> dst = {};
    std::copy(v.begin(), v.end(), dst.begin());
    CHECK(dst[0] == 1 && dst[4] == 5, "alg-copy");
    std::fill(dst.begin(), dst.end(), 9);
    CHECK(dst[0] == 9 && dst[4] == 9, "alg-fill");
    CHECK(std::min(3, 7) == 3 && std::max(3, 7) == 7, "alg-minmax");
    CHECK(std::min({9, 2, 5}) == 2 && std::max({9, 2, 5}) == 9,
          "alg-minmax-list");
    int x = 1, y = 2;
    std::swap(x, y);
    CHECK(x == 2 && y == 1, "alg-swap");
    std::string s = "dcba";
    std::sort(s.begin(), s.end());
    CHECK(s == "abcd", "alg-sort-str");
    printf("[phase6] algorithm ok\n");
  }

  // ---- Koeksistensi C + argv ----
  {
    CHECK(argc == 2, "argc");
    CHECK(argv != nullptr && argv[1] != nullptr, "argv-null");
    CHECK(strcmp(argv[1], "hello") == 0, "argv1");
    CHECK(strlen("kyuzen") == 6, "strlen");
    char *m = (char *)malloc(64);
    CHECK(m != nullptr, "malloc-null");
    strcpy(m, "c-and-cxx");
    CHECK(strcmp(m, "c-and-cxx") == 0, "malloc-str");
    std::string mixed = m; // C → C++
    CHECK(mixed == "c-and-cxx", "c-to-cxx");
    free(m);
    printf("[phase6] argv0=%s\n", argv[0]);
    printf("[phase6] c coexistence ok\n");
  }

  printf("[phase6] PASS\n");
  return 0;
}
