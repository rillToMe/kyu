// Kyuzen C++ SDK example: strings (examples/cpp/strings/strings.cpp).
//
// Memakai subset Phase 6 lewat boundary publik: string/string_view plus
// helper entry `kyuzen::run`. Startup tetap milik CRT Phase 5.

#include <cstdio>
#include <string>
#include <string_view>

#include <kyuzen/app.hpp>
#include <kyuzen/config.hpp>

namespace {

int body(int argc, char** argv) {
  (void)argc;
  (void)argv;
  static_assert(kyuzen::sdk_major == 7, "SDK C++ Phase 7 diharapkan");

  std::string name("kyuzen");
  name += "-os";
  std::string_view view(name);
  if (view.size() != 9)
    return 1;
  if (view.substr(7) != "os")
    return 2;
  if (name.find('-') != 6)
    return 3;

  // Melewati SSO agar alokasi heap C++ terpakai lewat jalur SDK.
  std::string heap("0123456789abcdefghijklmnopqrstuvwxyz");
  if (heap.size() != 36 || heap.back() != 'z')
    return 4;

  printf("strings: %s (%u chars)\n", name.c_str(),
         static_cast<unsigned>(name.size()));
  return 0;
}

} // namespace

extern "C" int main(int argc, char** argv) {
  return kyuzen::run(body, argc, argv);
}
