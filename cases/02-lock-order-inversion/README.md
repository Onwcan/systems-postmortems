# 02 — Lock order inversion

Two threads, two mutexes, taken in opposite orders. Once a day, everything
stops.

## Symptom

> "The transfer service hangs. Not often — maybe once a day under load — and
> when it does, every thread is stuck and nothing in the logs says why.
> Restarting clears it."

## Hypothesis

A hang with no CPU usage and no log line is a deadlock until proven otherwise.
Once a day under load means it needs a specific interleaving, so the window is
narrow — which points at two locks acquired in an order that depends on
something variable.

## Evidence

`evidence/02-broken-tsan.txt`:

```
DEADLOCK -- no progress for 2 s. Wait-for graph:

  transfer(alice, bob)   holds alice  waiting for bob
  transfer(bob, alice)   holds bob    waiting for alice

  cycle: alice -> bob -> alice
```

**The tool that should have produced this is ThreadSanitizer's deadlock
detector, and it is not available here.** That detector is a Clang feature;
GCC's libtsan does not implement it. Building this case with
`g++ -fsanitize=thread` and setting `TSAN_OPTIONS=detect_deadlocks=1` produces
no report at all — the option is silently inert and the program still hangs.
Only GCC is installed on this machine and installing Clang needs root.

So the program carries a watchdog instead: a `TracedLock` wrapper that records
which lock each thread holds and which it is blocked on, and a thread that after
two seconds without progress prints the cycle and exits. It is a miniature,
hand-rolled version of what the sanitizer would have given for free.

The difference between the two is worth being clear about, because it is not
just convenience. **TSan reports the inverted order the first time it observes
it, on runs where no deadlock occurs.** The watchdog can only report a deadlock
that has actually happened. Against a bug that appears once a day, that gap is
the entire problem — a detector that needs the bad interleaving to occur is
back to waiting a day.

## Root cause

```cpp
void transfer(Account& from, Account& to, long amount) {
  std::lock_guard<std::mutex> first(from.lock);
  ...
  std::lock_guard<std::mutex> second(to.lock);
```

Each call locks its arguments in the order they arrived in. `transfer(alice,
bob)` takes alice then bob; `transfer(bob, alice)` takes bob then alice. Run
concurrently, each holds what the other needs.

Nothing is wrong with this function. Read on its own it is correct, and it
passes review because there is nothing in it to object to. The defect exists
only in the relationship between two calls, which no single-function review and
no unit test of `transfer` can see.

## Fix

`std::scoped_lock lock(from.lock, to.lock);`

`scoped_lock` acquires several mutexes with a deadlock-avoidance algorithm
(try, and on failure release everything and retry in a different order), so the
acquisition order at the source level stops mattering.

The alternative fix is a **lock hierarchy**: give every mutex a rank and require
that locks are only ever acquired in increasing rank order — here, by comparing
account addresses or IDs. That scales to more than two locks and can be asserted
at run time, but it demands a discipline everyone has to know about.
`scoped_lock` demands nothing and is right here.

## Regression test

`scripts/run-all.sh` requires the broken variant to print the wait-for cycle,
and the fixed variant to complete with balances intact. The outer `timeout` is a
backstop; the watchdog is the mechanism.

## What made it hard

That the function is correct. Every reviewer who looked at `transfer` was
looking at the right lines and there was nothing to see — the ordering that
matters is established by the *caller*, and there is no single place in the
source where both orders appear together.

Second: the frequency. Once a day means you cannot iterate. You get one
observation per day, so the investigation is over before it starts unless you
can make it reproduce — which here means widening the window with a sleep
between the two acquisitions, at which point it happens every time.
