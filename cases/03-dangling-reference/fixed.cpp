// SPDX-License-Identifier: Apache-2.0
//
// Fix: do not hold a reference across an operation that can reallocate.
//
// Two ways to do that, and the choice matters.
//
//   * `reserve()` up front so no reallocation happens. Fast, and *fragile*:
//     the correctness of the reference now depends on a capacity computation
//     somewhere else, and the next person to add a sensor past the reserved
//     count reintroduces the bug with no warning.
//
//   * Re-fetch the reference after mutating, or hold an index instead. Slightly
//     more typing, and correct regardless of what happens to the container.
//
// The second is chosen here. `reserve` is an optimisation; it is not a
// lifetime guarantee, and using it as one is how this bug comes back.

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Sensor {
  std::string name;
  double reading = 0.0;
};

}  // namespace

int main() {
  std::vector<Sensor> sensors;
  sensors.push_back({"temperature", 21.5});

  // An index, not a reference. Survives any amount of reallocation.
  const std::size_t first_index = 0;

  for (int i = 0; i < 64; ++i) {
    sensors.push_back({"aux" + std::to_string(i), static_cast<double>(i)});
  }

  const Sensor& first = sensors[first_index];
  std::printf("first sensor: %s = %.1f\n", first.name.c_str(), first.reading);
  return 0;
}
