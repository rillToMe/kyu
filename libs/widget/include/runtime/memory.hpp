// libs/widget/include/runtime/memory.hpp — deklarasi runtime shim toolkit.
// Implementasi: src/runtime/runtime.cpp. Dulu fungsi-fungsi ini berada di
// anonymous namespace apps/libui.cpp; setelah dipecah mereka berlinkage C++
// biasa supaya bisa dipakai lintas file (dulu namespace anonim = satu TU).
// operator new/delete + __cxa_pure_virtual SENGAJA tidak dideklarasikan di
// sini — keduanya tetap didefinisikan persis SEKALI di runtime.cpp (ODR).
#ifndef KWIDGET_RUNTIME_MEMORY_HPP
#define KWIDGET_RUNTIME_MEMORY_HPP

#include "runtime/platform.hpp"

void  _ui_free(void* p);
void* _ui_alloc(unsigned n);
int   _ui_strlen(const char* s);
int   _ui_strncmp(const char* a, const char* b, int n);
char* _ui_strdup(const char* s);

#endif // KWIDGET_RUNTIME_MEMORY_HPP
