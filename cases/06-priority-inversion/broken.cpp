// SPDX-License-Identifier: Apache-2.0
//
// Symptom as reported: "the control loop misses its deadline, but only when the
// diagnostics thread is busy. Diagnostics runs at the lowest priority we have,
// so it should not be able to affect anything."
//
// It is not affecting it directly. It holds a lock the control loop needs, and
// it cannot release that lock because something of *middle* priority is using
// the CPU. The low-priority thread blocks the high-priority one through an
// intermediary that never touches the lock at all -- which is why nobody
// reading the two functions that share the mutex can see the problem.
//
// This is priority inversion. It is the failure that put Mars Pathfinder into a
// reset loop in 1997.
//
// It requires real priorities. If SCHED_FIFO is refused -- the default on WSL2,
// on most containers, and anywhere RLIMIT_RTPRIO is 0 -- then all three threads
// are ordinary time-shared threads, and what this program measures is plain
// lock contention that happens to look similar. That distinction is the whole
// difference between a demonstration and a coincidence, so this program checks
// and says which one you got.

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_shared;  // guards a resource both ends touch
std::atomic<bool> g_running{true};
std::atomic<std::int64_t> g_worst_block_us{0};
std::atomic<int> g_rt_threads{0};
std::atomic<std::uint64_t> g_critical_iterations{0};
std::atomic<std::uint64_t> g_sink{0};

/// Requests a policy and reads it back. Requesting is not getting: every
/// pthread_setschedparam here can fail, and a demo that ignores the return
/// value reports the time-sharing scheduler's behaviour as though it were the
/// real-time scheduler's.
bool setPriority(int policy, int priority) {
  sched_param param{};
  param.sched_priority = priority;
  if (::pthread_setschedparam(::pthread_self(), policy, &param) != 0) {
    return false;
  }
  int actual_policy = 0;
  sched_param actual{};
  if (::pthread_getschedparam(::pthread_self(), &actual_policy, &actual) != 0) {
    return false;
  }
  const bool granted = actual_policy == policy && actual.sched_priority == priority;
  if (granted) {
    g_rt_threads.fetch_add(1, std::memory_order_relaxed);
  }
  return granted;
}

/// A fixed amount of *work*, not a wall-clock interval.
///
/// This distinction is the mechanism of the whole case. A critical section
/// written as "spin until 5 ms have elapsed" finishes 5 ms after it started no
/// matter how little CPU it received -- so it cannot be stretched by
/// preemption, and no amount of interference from the hog will lengthen it. An
/// earlier version of this case did exactly that and measured nothing.
///
/// Work that must actually be *performed* takes longer in wall time when the
/// thread doing it is not running. That is what a waiter on the lock pays for.
void spinWork(std::uint64_t iterations) {
  // A dependency chain, deliberately. Each step needs the previous step's
  // result, so the loop cannot be vectorised and -- more importantly -- cannot
  // be reduced to a closed form.
  //
  // The obvious version of this function, `accumulator += i * k`, is a
  // polynomial in i, and GCC recognises it: at -O2 with a compile-time
  // iteration count it replaces the whole loop with a Gauss-sum formula that
  // runs in constant time. The first version of this case did exactly that.
  // Calibration timed 20 million iterations at 219 ns, concluded the machine
  // ran 91324 iterations per nanosecond, and asked for 456 billion iterations
  // to fill 5 ms. The program never finished.
  std::uint64_t state = g_sink.load(std::memory_order_relaxed) | 1ULL;
  for (std::uint64_t i = 0; i < iterations; ++i) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
  }
  g_sink.store(state, std::memory_order_relaxed);
}

void busyFor(std::chrono::microseconds duration) {
  const auto deadline = Clock::now() + duration;
  while (Clock::now() < deadline) {
  }
}

// The duty cycles below are chosen, and getting them wrong makes the case
// measure nothing. Two constraints pull in opposite directions.
//
// The hog's idle window must be SHORTER than the critical section. The holder
// runs at the lowest priority, so it can only ever *acquire* the lock during a
// window when the hog is asleep -- it is not running at any other time. If that
// window is long enough to finish the critical section in, the holder always
// finishes before the hog returns and there is no inversion to observe. An
// earlier version of this case had a 15 ms idle window against a 5 ms critical
// section and reported "no significant inversion" every run, correctly. A 3 ms
// window against 5 ms of work guarantees the holder is caught mid-section.
//
// Total real-time utilisation must stay under 95%. Linux throttles real-time
// tasks: sched_rt_runtime_us / sched_rt_period_us is 950000/1000000 by default,
// and once SCHED_FIFO threads exceed that share of a CPU the kernel suspends
// all of them for the rest of the period. An earlier version sat at ~96% and
// every measurement included a 50 ms kernel-imposed stall that had nothing to
// do with priority inversion.
//
// Hog 10 ms per 13 ms (77%) plus holder 5 ms per 45 ms (11%) satisfies both:
// the window is too short to finish in, and the total is near 88%.
constexpr auto kCriticalSection = std::chrono::milliseconds(5);
constexpr auto kHogBurst = std::chrono::milliseconds(10);
constexpr auto kHogIdle = std::chrono::milliseconds(3);
constexpr auto kHolderIdle = std::chrono::milliseconds(30);

struct Calibration {
  std::uint64_t iterations;
  std::int64_t measured_us;  ///< what those iterations actually cost
};

/// Calibrates spinWork so the critical section costs `target` of CPU time, then
/// measures the result to confirm it.
///
/// The confirmation is not ceremony. A calibration is a prediction, and this one
/// was wrong by a factor of twenty thousand until the loop above was changed.
/// Predicting is cheap; checking the prediction is what makes it a number worth
/// printing.
///
/// Runs before the real-time threads start, while nothing is competing, so it
/// measures the machine rather than the contention.
Calibration calibrate(std::chrono::microseconds target) {
  // volatile so the probe count is not a compile-time constant: a constant lets
  // the optimiser specialise the very loop it is supposed to be timing.
  volatile std::uint64_t probe = 20'000'000;
  const std::uint64_t probe_iterations = probe;

  const auto start = Clock::now();
  spinWork(probe_iterations);
  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
  if (elapsed_ns <= 0) {
    return {probe_iterations, 0};
  }

  const double per_ns =
      static_cast<double>(probe_iterations) / static_cast<double>(elapsed_ns);
  const auto iterations =
      static_cast<std::uint64_t>(per_ns * static_cast<double>(target.count()) * 1000.0);

  const auto verify_start = Clock::now();
  spinWork(iterations);
  const auto measured_us =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - verify_start)
          .count();
  return {iterations, measured_us};
}

/// Low priority: takes the lock and does slow work while holding it.
void lowPriorityDiagnostics() {
  setPriority(SCHED_FIFO, 10);
  const std::uint64_t iterations = g_critical_iterations.load(std::memory_order_relaxed);
  while (g_running.load(std::memory_order_relaxed)) {
    {
      const std::lock_guard<std::mutex> held(g_shared);
      // Holding the lock while doing real work. Not unusual, not obviously
      // wrong in isolation, and the whole problem.
      spinWork(iterations);
    }
    std::this_thread::sleep_for(kHolderIdle);
  }
}

/// Middle priority: never touches the lock. Just uses the CPU, in long runs.
///
/// The burst length is what makes the inversion unbounded in practice. A hog
/// that yields every couple of milliseconds lets the lock holder finish anyway
/// and the effect nearly disappears; a hog that runs 20 ms at a stretch keeps
/// the holder off the CPU for that whole stretch. The waiting control loop pays
/// the hog's burst, not the holder's critical section -- and the hog can be
/// made longer without either lock-using function changing at all.
void mediumPriorityHog() {
  setPriority(SCHED_FIFO, 50);
  while (g_running.load(std::memory_order_relaxed)) {
    busyFor(kHogBurst);
    std::this_thread::sleep_for(kHogIdle);
  }
}

/// High priority: the control loop. Wants the lock briefly, every cycle.
void highPriorityControl() {
  setPriority(SCHED_FIFO, 90);
  for (int cycle = 0; cycle < 600; ++cycle) {
    const auto asked = Clock::now();
    {
      const std::lock_guard<std::mutex> held(g_shared);
      const auto blocked =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - asked)
              .count();
      if (blocked > g_worst_block_us.load(std::memory_order_relaxed)) {
        g_worst_block_us.store(blocked, std::memory_order_relaxed);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void reportRealtimeLimit() {
  rlimit limit{};
  if (::getrlimit(RLIMIT_RTPRIO, &limit) == 0) {
    std::printf("RLIMIT_RTPRIO:      soft %llu, hard %llu\n",
                static_cast<unsigned long long>(limit.rlim_cur),
                static_cast<unsigned long long>(limit.rlim_max));
  }
}

}  // namespace

int main() {
  // Line-buffered: under `docker run` without a TTY stdout is block-buffered, so
  // a program that hangs shows nothing at all about where it hung.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  // A plain std::mutex has no priority inheritance. When the high-priority
  // thread blocks on it, the low-priority holder keeps its own low priority,
  // the medium-priority hog outranks it, so the holder does not run, so the
  // lock is never released.
  std::printf("mutex:              std::mutex (no priority inheritance)\n");
  reportRealtimeLimit();

  // Pin everything to one CPU. Priority inversion needs contention for a
  // processor; on an idle 24-core machine the medium-priority thread simply
  // runs somewhere else and nothing is demonstrated. Threads inherit the
  // affinity mask of the thread that creates them, so setting it here covers
  // all three.
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  const bool pinned = ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
  std::printf("pinned to one CPU:  %s\n", pinned ? "yes" : "NO -- results meaningless");

  const Calibration calibration = calibrate(kCriticalSection);
  g_critical_iterations.store(calibration.iterations, std::memory_order_relaxed);
  const auto target_us =
      std::chrono::duration_cast<std::chrono::microseconds>(kCriticalSection).count();
  std::printf("critical section:   %llu iterations = %lld us measured (target %lld us)\n",
              static_cast<unsigned long long>(calibration.iterations),
              static_cast<long long>(calibration.measured_us),
              static_cast<long long>(target_us));
  if (calibration.measured_us < target_us / 2 ||
      calibration.measured_us > target_us * 2) {
    std::printf(
        "\nCALIBRATION FAILED -- the critical section is not the length this\n"
        "case depends on, so the result below would not mean what it claims.\n");
    return 3;
  }

  std::thread low(lowPriorityDiagnostics);
  std::thread medium(mediumPriorityHog);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::thread high(highPriorityControl);

  high.join();
  g_running.store(false, std::memory_order_relaxed);
  low.join();
  medium.join();

  const int rt = g_rt_threads.load();
  const std::int64_t worst = g_worst_block_us.load();
  std::printf("SCHED_FIFO granted: %d of 3 threads\n", rt);
  std::printf("worst lock wait:    %lld us\n", static_cast<long long>(worst));
  std::printf("\n");

  if (rt < 3) {
    std::printf(
        "INCONCLUSIVE -- no priorities, so nothing was inverted.\n"
        "All three threads ran under the time-sharing scheduler, where a 5 ms\n"
        "critical section blocks a waiter for milliseconds all by itself. The\n"
        "number above is ordinary lock contention and it proves nothing here.\n"
        "\n"
        "Re-run with real-time scheduling available:\n"
        "  scripts/run-case-06-container.sh\n");
    return 2;
  }

  // The critical section costs 5 ms of CPU. A waiter blocked for substantially
  // longer than that cannot be waiting for the work inside it -- it is waiting
  // for the holder to be scheduled at all. That is the whole diagnosis, which
  // is why the threshold is expressed in terms of the critical section rather
  // than as a round number.
  const auto critical_us =
      std::chrono::duration_cast<std::chrono::microseconds>(kCriticalSection).count();
  const bool inverted = worst > 2 * critical_us;
  std::printf(
      "\n%s\n",
      inverted ? "WAITED FAR LONGER THAN THE CRITICAL SECTION -- priority inversion.\n"
                 "The holder was ready but not running: a medium-priority thread that\n"
                 "never touches this lock was using the CPU instead."
               : "no significant inversion observed this run");
  return inverted ? 1 : 0;
}
