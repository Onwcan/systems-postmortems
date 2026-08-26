// SPDX-License-Identifier: Apache-2.0
//
// Symptom as reported: "our request counter under-reports by a few percent
// whenever the service is busy. At low load the number is exact."
//
// The load-dependence is the tell. A logic error would be wrong at any load; a
// race only loses when two threads collide, and collisions scale with load.

#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kWorkers = 4;
constexpr std::uint64_t kPerWorker = 200'000;

struct Metrics {
  // Looks harmless. It is a plain integer written from several threads with
  // no synchronisation at all, which is a data race and therefore undefined
  // behaviour -- not merely "occasionally loses an update".
  std::uint64_t requests_handled = 0;
};

Metrics g_metrics;

void worker() {
  for (std::uint64_t i = 0; i < kPerWorker; ++i) {
    ++g_metrics.requests_handled;
  }
}

}  // namespace

int main() {
  std::vector<std::thread> workers;
  workers.reserve(kWorkers);
  for (int i = 0; i < kWorkers; ++i) {
    workers.emplace_back(worker);
  }
  for (std::thread& w : workers) {
    w.join();
  }

  const std::uint64_t expected = kWorkers * kPerWorker;
  std::printf("expected %llu, counted %llu", static_cast<unsigned long long>(expected),
              static_cast<unsigned long long>(g_metrics.requests_handled));
  if (g_metrics.requests_handled != expected) {
    std::printf("  -- LOST %llu (%.2f%%)\n",
                static_cast<unsigned long long>(expected - g_metrics.requests_handled),
                100.0 * static_cast<double>(expected - g_metrics.requests_handled) /
                    static_cast<double>(expected));
    return 1;
  }
  std::printf("  -- exact this run (the race is still there)\n");
  return 0;
}
