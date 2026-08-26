// SPDX-License-Identifier: Apache-2.0
//
// Symptom as reported: "we added a second worker thread and throughput went
// DOWN. Not 'failed to scale' -- actually slower than one thread. Each worker
// touches its own counter, so there is no contention to speak of."
//
// There is no *logical* contention. There is severe physical contention, and
// nothing in the source shows it, because the sharing is a property of memory
// layout rather than of the program's structure.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kIterations = 50'000'000;

// Four counters, one per worker, adjacent in memory.
//
// Adjacent means they share a cache line. Every increment by one worker
// invalidates that line in every other core's cache, so each of the other
// workers takes a coherence miss on its *own* private counter. The hardware is
// bouncing one line between four cores tens of millions of times.
struct Counters {
  std::atomic<std::uint64_t> value[4];
};

Counters g_counters;

void worker(int index) {
  for (std::uint64_t i = 0; i < kIterations; ++i) {
    g_counters.value[index].fetch_add(1, std::memory_order_relaxed);
  }
}

double runWith(int threads) {
  for (int i = 0; i < 4; ++i) {
    g_counters.value[i].store(0, std::memory_order_relaxed);
  }
  const auto started = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back(worker, i);
  }
  for (std::thread& w : workers) {
    w.join();
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const double seconds =
      std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1e6;
  return (static_cast<double>(kIterations) * threads) / seconds / 1e6;
}

}  // namespace

int main() {
  std::printf("counters are %zu bytes apart\n",
              reinterpret_cast<const char*>(&g_counters.value[1]) -
                  reinterpret_cast<const char*>(&g_counters.value[0]));
  const double one = runWith(1);
  const double four = runWith(4);
  std::printf("1 thread : %8.1f M increments/s\n", one);
  std::printf("4 threads: %8.1f M increments/s  (%.2fx)\n", four, four / one);
  std::printf("%s\n", four < one * 2.0
                          ? "FOUR THREADS BARELY BEAT ONE -- this is false sharing"
                          : "scaled");
  return 0;
}
