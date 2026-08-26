// SPDX-License-Identifier: Apache-2.0
//
// Symptom as reported: "the last sensor in the list occasionally reads as
// garbage -- huge values, sometimes zero. Only when we have a lot of sensors
// configured. With four or five it never happens."
//
// "Only with a lot of them" is the clue. Something about growth, not about the
// data.

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

  // A reference into the vector. Valid right now.
  Sensor& first = sensors.front();

  // ...and invalidated here. push_back may reallocate, which moves every
  // element to new storage and leaves `first` pointing at freed memory. The
  // reference itself gives no hint: it still looks like a live object, and on
  // a small vector with spare capacity it usually still *is* one, which is
  // exactly why this only shows up "with a lot of sensors".
  for (int i = 0; i < 64; ++i) {
    sensors.push_back({"aux" + std::to_string(i), static_cast<double>(i)});
  }

  std::printf("first sensor: %s = %.1f\n", first.name.c_str(), first.reading);
  return 0;
}
