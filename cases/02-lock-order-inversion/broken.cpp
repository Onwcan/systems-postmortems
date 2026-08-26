// SPDX-License-Identifier: Apache-2.0
//
// Symptom as reported: "the transfer service hangs. Not often -- maybe once a
// day under load -- and when it does, every thread is stuck and nothing in the
// logs says why. Restarting clears it."
//
// A hang with no log line and no CPU usage is a deadlock until proven
// otherwise. The once-a-day frequency says it needs a specific interleaving.
//
// The right tool for this is ThreadSanitizer's deadlock detector, which reports
// the inverted lock order the first time it *observes* it -- on runs where no
// deadlock occurs at all. That detector is a Clang feature. GCC's libtsan does
// not implement it: building this file with g++ -fsanitize=thread and setting
// TSAN_OPTIONS=detect_deadlocks=1 produces no report and the program still
// hangs. Verified, on the toolchain in evidence/. Only GCC is available here,
// so this case does what you do when the tool is missing: it instruments the
// thing it needs to see.
//
// The watchdog below is a miniature version of what TSan would have given for
// free. It records which lock each thread holds and which it is waiting for,
// and after two seconds of no progress it prints the cycle and exits, rather
// than hanging until an external timeout kills it with nothing to show.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

struct Account {
  std::mutex lock;
  long balance = 1000;
  const char* name = "";
};

Account g_alice{{}, 1000, "alice"};
Account g_bob{{}, 1000, "bob"};

/// What one thread holds and what it is blocked on. Enough to find a cycle.
struct ThreadState {
  std::atomic<const char*> label{nullptr};
  std::atomic<const char*> holding{nullptr};
  std::atomic<const char*> waiting{nullptr};
};

ThreadState g_threads[2];
thread_local ThreadState* t_state = nullptr;

/// std::lock_guard that also records the wait-for graph.
class TracedLock {
 public:
  TracedLock(Account& account, ThreadState& state)
      : lock_(account.lock, std::defer_lock), state_(state), name_(account.name) {
    state_.waiting.store(name_, std::memory_order_release);
    lock_.lock();
    state_.waiting.store(nullptr, std::memory_order_release);
    // Record only the first lock held: that is the one the other thread is
    // blocked on, and so the one that appears in the cycle.
    const char* expected = nullptr;
    state_.holding.compare_exchange_strong(expected, name_, std::memory_order_acq_rel);
  }

  ~TracedLock() {
    const char* expected = name_;
    state_.holding.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
  }

  TracedLock(const TracedLock&) = delete;
  TracedLock& operator=(const TracedLock&) = delete;

 private:
  std::unique_lock<std::mutex> lock_;
  ThreadState& state_;
  const char* name_;
};

// Locks `from` then `to`. Correct in isolation, and the bug is invisible in
// review of this function alone -- it exists only in the relationship between
// two concurrent calls with swapped arguments.
void transfer(Account& from, Account& to, long amount) {
  TracedLock first(from, *t_state);
  // Widen the window so the interleaving happens reliably. In production this
  // is whatever real work sits between the two acquisitions.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  TracedLock second(to, *t_state);
  from.balance -= amount;
  to.balance += amount;
}

/// Reports a two-thread wait-for cycle, if one is still standing after a while.
void watchdog() {
  std::this_thread::sleep_for(std::chrono::seconds(2));

  const char* a_holds = g_threads[0].holding.load(std::memory_order_acquire);
  const char* a_wants = g_threads[0].waiting.load(std::memory_order_acquire);
  const char* b_holds = g_threads[1].holding.load(std::memory_order_acquire);
  const char* b_wants = g_threads[1].waiting.load(std::memory_order_acquire);

  if (a_holds == nullptr || a_wants == nullptr || b_holds == nullptr ||
      b_wants == nullptr) {
    return;  // not stuck; let the program finish on its own
  }

  std::printf("\nDEADLOCK -- no progress for 2 s. Wait-for graph:\n\n");
  std::printf("  %-22s holds %-6s waiting for %s\n",
              g_threads[0].label.load(std::memory_order_acquire), a_holds, a_wants);
  std::printf("  %-22s holds %-6s waiting for %s\n",
              g_threads[1].label.load(std::memory_order_acquire), b_holds, b_wants);
  std::printf("\n  cycle: %s -> %s -> %s\n\n", a_holds, b_holds, a_holds);
  std::printf(
      "Both threads take two locks. Neither takes them in the same order,\n"
      "because each takes them in the order its own arguments arrived in.\n");
  std::fflush(stdout);

  // Exit hard: the two worker threads are never going to be joinable, and
  // running static destructors from here would touch the held mutexes.
  std::_Exit(1);
}

}  // namespace

int main() {
  std::printf("starting two transfers in opposite directions...\n");
  std::fflush(stdout);

  std::thread(watchdog).detach();

  std::thread one([] {
    t_state = &g_threads[0];
    t_state->label.store("transfer(alice, bob)", std::memory_order_release);
    transfer(g_alice, g_bob, 10);
  });
  std::thread two([] {
    t_state = &g_threads[1];
    t_state->label.store("transfer(bob, alice)", std::memory_order_release);
    transfer(g_bob, g_alice, 10);
  });

  one.join();
  two.join();
  std::printf("completed -- no deadlock this run\n");
  return 0;
}
