// Kyuzen C++ SDK example: hello (examples/cpp/hello/hello.cpp).
//
// Dibangun hanya lewat wrapper publik `kyuzen-c++`; tidak ada path LLVM,
// CRT, arsip, atau linker script di perintah build. `main` tetap ber-linkage
// C karena CRT yang memanggilnya.

#include <cstdio>

#include <kyuzen/config.hpp>

static_assert(kyuzen::sdk_major == 7, "contoh ini memakai SDK C++ Phase 7");
static_assert(kyuzen::sdk_minor == 0, "versi minor SDK tak terduga");

extern "C" int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("Hello from Kyuzen C++ SDK %d.%d\n", kyuzen::sdk_major,
         kyuzen::sdk_minor);
  return 0;
}
