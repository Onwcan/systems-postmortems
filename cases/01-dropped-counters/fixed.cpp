// SPDX-License-Identifier: Apache-2.0
//
// Fix: the counter is an atomic, incremented with relaxed ordering.
//
// Relaxed is correct here and not a shortcut. The counter carries no
// happens-before relationship with any other data -- nobody reads it and then
// concludes something about memory written before it -- so all that is required
// is that the increments do not interleave destructively. Reaching for
// seq_cst would add a full barrier per increment on the hot path to buy an
// ordering guarantee nothing uses.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kWorkers = 4;
constexpr std::uint64_t kPerWorker = 200'000;

struct Metrics {
  std::atomic<std::uint64_t> requests_handled{0};
};

Metrics g_metrics;

void worker() {
  for (std::uint64_t i = 0; i < kPerWorker; ++i) {
    g_metrics.requests_handled.fetch_add(1, std::memory_order_relaxed);
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
  const std::uint64_t counted = g_metrics.requests_handled.load();
  std::printf("expected %llu, counted %llu -- %s\n",
              static_cast<unsigned long long>(expected),
              static_cast<unsigned long long>(counted),
              counted == expected ? "exact" : "STILL LOSING");
  return counted == expected ? 0 : 1;
}
