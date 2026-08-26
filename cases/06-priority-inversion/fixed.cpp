// SPDX-License-Identifier: Apache-2.0
//
// Fix: a mutex with priority inheritance.
//
// PTHREAD_PRIO_INHERIT makes the kernel temporarily raise the lock holder to
// the priority of the highest-priority waiter. The low-priority diagnostics
// thread is boosted to 90 while the control loop waits, so it now outranks the
// medium-priority hog, finishes its critical section, releases the lock, and
// drops back down.
//
// std::mutex cannot do this. The standard provides no way to set the attribute,
// so this drops to pthread_mutex_t directly. That is a real cost: giving up the
// portable type to obtain a property the portable type cannot express.
//
// And it is the second-best fix. The better one is not to hold a lock across
// slow work at all. Priority inheritance *bounds* the damage -- the waiter now
// waits one critical section instead of an unrelated thread's burst length --
// but it does not remove it, and a 5 ms critical section is a design problem
// that inheritance merely makes survivable. Read the number below as
// "bounded", not as "fast".
//
// Everything except the mutex type is identical to broken.cpp. If the workload
// differed, the comparison would not be a comparison.

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

pthread_mutex_t g_shared;
std::atomic<bool> g_running{true};
std::atomic<std::int64_t> g_worst_block_us{0};
std::atomic<int> g_rt_threads{0};
std::atomic<std::uint64_t> g_critical_iterations{0};
std::atomic<std::uint64_t> g_sink{0};

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

/// A fixed amount of work, not a wall-clock interval. See broken.cpp for why
/// that distinction is the mechanism of the entire case.
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

// Identical to the broken version. See broken.cpp for why these particular
// values: the hog's idle window has to be shorter than the critical section,
// and total real-time utilisation has to stay under the kernel's 95% throttle.
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

/// Giving up std::lock_guard should not also mean giving up exception safety.
class PriorityLock {
 public:
  explicit PriorityLock(pthread_mutex_t& mutex) : mutex_(mutex) {
    ::pthread_mutex_lock(&mutex_);
  }
  ~PriorityLock() { ::pthread_mutex_unlock(&mutex_); }
  PriorityLock(const PriorityLock&) = delete;
  PriorityLock& operator=(const PriorityLock&) = delete;
  PriorityLock(PriorityLock&&) = delete;
  PriorityLock& operator=(PriorityLock&&) = delete;

 private:
  pthread_mutex_t& mutex_;
};

void lowPriorityDiagnostics() {
  setPriority(SCHED_FIFO, 10);
  const std::uint64_t iterations = g_critical_iterations.load(std::memory_order_relaxed);
  while (g_running.load(std::memory_order_relaxed)) {
    {
      const PriorityLock held(g_shared);
      spinWork(iterations);
    }
    std::this_thread::sleep_for(kHolderIdle);
  }
}

void mediumPriorityHog() {
  setPriority(SCHED_FIFO, 50);
  while (g_running.load(std::memory_order_relaxed)) {
    busyFor(kHogBurst);
    std::this_thread::sleep_for(kHogIdle);
  }
}

void highPriorityControl() {
  setPriority(SCHED_FIFO, 90);
  for (int cycle = 0; cycle < 600; ++cycle) {
    const auto asked = Clock::now();
    {
      const PriorityLock held(g_shared);
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

  pthread_mutexattr_t attributes;
  ::pthread_mutexattr_init(&attributes);
  const int rc = ::pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT);
  ::pthread_mutex_init(&g_shared, &attributes);
  std::printf("mutex:              pthread PTHREAD_PRIO_INHERIT (%s)\n",
              rc == 0 ? "enabled" : "NOT SUPPORTED on this platform");
  reportRealtimeLimit();

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
  ::pthread_mutex_destroy(&g_shared);
  ::pthread_mutexattr_destroy(&attributes);

  const int rt = g_rt_threads.load();
  const std::int64_t worst = g_worst_block_us.load();
  std::printf("SCHED_FIFO granted: %d of 3 threads\n", rt);
  std::printf("worst lock wait:    %lld us\n", static_cast<long long>(worst));
  std::printf("\n");

  if (rt < 3) {
    std::printf(
        "INCONCLUSIVE -- no priorities, so there was nothing to inherit.\n"
        "This number is ordinary lock contention, the same as the broken\n"
        "version measures under the same conditions. Comparing the two here\n"
        "would show no difference and would mean nothing.\n"
        "\n"
        "Re-run with real-time scheduling available:\n"
        "  scripts/run-case-06-container.sh\n");
    return 2;
  }

  const auto critical_us =
      std::chrono::duration_cast<std::chrono::microseconds>(kCriticalSection).count();
  std::printf("\n");

  // Inheritance does not make the wait short. It makes it *bounded* -- by the
  // critical section, which is the one quantity the people who wrote these two
  // functions can see and control. Without it the bound is the hog's burst
  // length, set by unrelated code they have never read.
  if (worst <= 2 * critical_us) {
    std::printf(
        "BOUNDED by one critical section. The medium-priority hog no longer\n"
        "delays the control loop: the holder is boosted to the waiter's\n"
        "priority, so it preempts the hog and finishes.\n");
    return 0;
  }
  std::printf(
      "STILL EXCEEDS the critical section. Inheritance is not doing what it\n"
      "should -- check that all three threads really got SCHED_FIFO and that\n"
      "the real-time throttle is not firing.\n");
  return 1;
}
