// SPDX-License-Identifier: Apache-2.0
//
// Fix: pad each counter onto its own cache line.
//
// The algorithm is unchanged. The memory ordering is unchanged. The only
// difference is `alignas`, and that is the entire fix -- which is what makes
// this class of bug so hard to find by reading code. Nothing about the source
// of the broken version looks wrong.
//
// The cost is 64 bytes per counter instead of 8. For four counters that is 256
// bytes to recover most of a 4x speedup, which is not a difficult trade. It
// would be a bad trade for a million counters, and that is the real design
// question: padding is for the few contended things, not for everything.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kIterations = 50'000'000;
// A literal rather than std::hardware_destructive_interference_size, which is
// ABI-sensitive and which GCC warns about across translation units.
constexpr std::size_t kCacheLine = 64;

struct alignas(kCacheLine) PaddedCounter {
  std::atomic<std::uint64_t> value{0};
  // Explicit padding so the size, not just the alignment, is a whole line.
  char padding[kCacheLine - sizeof(std::atomic<std::uint64_t>)]{};
};
static_assert(sizeof(PaddedCounter) == kCacheLine);

PaddedCounter g_counters[4];

void worker(int index) {
  for (std::uint64_t i = 0; i < kIterations; ++i) {
    g_counters[index].value.fetch_add(1, std::memory_order_relaxed);
  }
}

double runWith(int threads) {
  for (auto& counter : g_counters) {
    counter.value.store(0, std::memory_order_relaxed);
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
              reinterpret_cast<const char*>(&g_counters[1]) -
                  reinterpret_cast<const char*>(&g_counters[0]));
  const double one = runWith(1);
  const double four = runWith(4);
  std::printf("1 thread : %8.1f M increments/s\n", one);
  std::printf("4 threads: %8.1f M increments/s  (%.2fx)\n", four, four / one);
  std::printf("%s\n", four > one * 2.0 ? "scales" : "STILL NOT SCALING");
  return 0;
}
