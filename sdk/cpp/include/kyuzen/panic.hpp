// Kyuzen C++ SDK — fail-hard panic terminal (Phase 7).
//
// SDK memakai `-fno-exceptions`: panic adalah kegagalan terminal, bukan
// mekanisme error yang bisa ditangkap. Implementasi memakai backend stdio C
// yang sudah ada lalu jalur abort runtime yang sudah ada; tanpa syscall
// baru dan tanpa framework error.

#pragma once

#include <stdio.h>
#include <stdlib.h>

namespace kyuzen {

[[noreturn]] inline void panic(const char* message) {
  if (message != nullptr && message[0] != '\0')
    printf("kyuzen panic: %s\n", message);
  abort();
}

} // namespace kyuzen
