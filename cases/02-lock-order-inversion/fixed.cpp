// SPDX-License-Identifier: Apache-2.0
//
// Fix: acquire both locks in one operation with std::scoped_lock, which uses a
// deadlock-avoidance algorithm rather than a fixed order.
//
// The alternative fix is to impose a global lock order -- always take the
// lower address first, say. That works, and it is what you do when the locks
// cannot be acquired together. It is worse here because it is a convention:
// nothing enforces it, and the next person to write a two-account operation has
// to know about it and remember. scoped_lock makes the property structural
// instead of remembered.

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

struct Account {
  std::mutex lock;
  long balance = 1000;
};

Account g_alice;
Account g_bob;

void transfer(Account& from, Account& to, long amount) {
  // Both, atomically, in whatever order avoids deadlock.
  std::scoped_lock both(from.lock, to.lock);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  from.balance -= amount;
  to.balance += amount;
}

}  // namespace

int main() {
  std::printf("starting two transfers in opposite directions...\n");
  std::fflush(stdout);

  std::thread one([] { transfer(g_alice, g_bob, 10); });
  std::thread two([] { transfer(g_bob, g_alice, 10); });

  one.join();
  two.join();
  std::printf("completed -- alice=%ld bob=%ld\n", g_alice.balance, g_bob.balance);
  return 0;
}
