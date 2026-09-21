// ============================================================
// KyuzenOS — explicit instantiation slice untuk __sort non-FP.
//
// KONTEKS: <algorithm> hulu memakai extern-template declarations untuk
// __sort, sehingga app tidak mengemisikannya dan mengharapkan dari library.
// Rumah hulu-nya (libcxx/src/algorithm.cpp) mengexplicit-instantiate JUGA
// varian float/double/long-double — yang mengemisikan instruksi FP
// hardware (x87/SSE) dan melanggar kebijakan 0-FP SDK. File itu tidak
// dapat dikompilasi parsial.
//
// ISI FILE INI: definisi template __sort + baris instantiation non-FP,
// keduanya disalin VERBATIM dari libcxx/src/algorithm.cpp (hulu). Yang
// DIBUANG hanya baris instantiation float/double/long-double (mengemisikan
// x87). Bukan implementasi ulang — slicing TU. Hasilnya: std::sort untuk
// char/integer/short/long bekerja; sort float/double/long-double = link
// error yang jujur (FP memang ditunda). Simbol yang dihasilkan identik
// dengan yang dihasilkan algorithm.cpp hulu untuk varian non-FP (mangle +
// ABI sama; diverifikasi 0 undefined di link Phase 6).
//
// Bila kebijakan FP berubah, hapus file ini dan kompilasi algorithm.cpp
// hulu apa adanya.
// ============================================================
#include <algorithm>
#include <bit>

_LIBCPP_BEGIN_NAMESPACE_STD

template <class Comp, class RandomAccessIterator>
void __sort(RandomAccessIterator first, RandomAccessIterator last, Comp comp) {
  if (first == last) // log(0) is undefined, so don't try computing the depth
    return;

  auto depth_limit = 2 * std::__bit_log2(static_cast<size_t>(last - first));

  // Only use bitset partitioning for arithmetic types.  We should also check
  // that the default comparator is in use so that we are sure that there are no
  // branches in the comparator.
  std::__introsort<_ClassicAlgPolicy,
                   ranges::less,
                   RandomAccessIterator,
                   __use_branchless_sort<ranges::less, RandomAccessIterator>>(first, last, ranges::less{}, depth_limit);
}

// clang-format off
template void __sort<__less<char>&, char*>(char*, char*, __less<char>&);
#if _LIBCPP_HAS_WIDE_CHARACTERS
template void __sort<__less<wchar_t>&, wchar_t*>(wchar_t*, wchar_t*, __less<wchar_t>&);
#endif
template void __sort<__less<signed char>&, signed char*>(signed char*, signed char*, __less<signed char>&);
template void __sort<__less<unsigned char>&, unsigned char*>(unsigned char*, unsigned char*, __less<unsigned char>&);
template void __sort<__less<short>&, short*>(short*, short*, __less<short>&);
template void __sort<__less<unsigned short>&, unsigned short*>(unsigned short*, unsigned short*, __less<unsigned short>&);
template void __sort<__less<int>&, int*>(int*, int*, __less<int>&);
template void __sort<__less<unsigned>&, unsigned*>(unsigned*, unsigned*, __less<unsigned>&);
template void __sort<__less<long>&, long*>(long*, long*, __less<long>&);
template void __sort<__less<unsigned long>&, unsigned long*>(unsigned long*, unsigned long*, __less<unsigned long>&);
template void __sort<__less<long long>&, long long*>(long long*, long long*, __less<long long>&);
template void __sort<__less<unsigned long long>&, unsigned long long*>(unsigned long long*, unsigned long long*,
                                                                       __less<unsigned long long>&);
// clang-format on

_LIBCPP_END_NAMESPACE_STD
