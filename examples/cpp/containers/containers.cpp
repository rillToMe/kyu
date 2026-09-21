// Kyuzen C++ SDK example: containers (examples/cpp/containers/containers.cpp).
//
// Memakai subset Phase 6 lewat boundary publik: array/vector/sort/unique_ptr
// plus helper entry `kyuzen::run`. Startup tetap milik CRT Phase 5.

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <vector>

#include <kyuzen/app.hpp>
#include <kyuzen/config.hpp>

namespace {

struct Item {
  int value;
  explicit Item(int x) : value(x) {}
};

int body(int argc, char** argv) {
  (void)argc;
  (void)argv;
  static_assert(kyuzen::sdk_major == 7, "SDK C++ Phase 7 diharapkan");

  std::array<int, 5> fixed = {5, 1, 4, 2, 3};
  std::vector<int> values(fixed.begin(), fixed.end());
  std::sort(values.begin(), values.end());
  if (values.front() != 1 || values.back() != 5)
    return 1;

  std::vector<Item> items;
  items.reserve(3);
  items.emplace_back(values[0]);
  items.emplace_back(values[2]);
  items.emplace_back(values[4]);
  int total = 0;
  for (const Item& item : items)
    total += item.value;
  if (total != 9)
    return 2;

  auto label = std::make_unique<int>(total);
  if (label.get() == nullptr || *label != 9)
    return 3;

  printf("containers: sorted %d..%d, total %d\n", values.front(),
         values.back(), *label);
  return 0;
}

} // namespace

extern "C" int main(int argc, char** argv) {
  return kyuzen::run(body, argc, argv);
}
