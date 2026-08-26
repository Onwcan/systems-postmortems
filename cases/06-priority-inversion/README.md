# 06 — Priority inversion

The lowest-priority thread blocks the highest-priority one, through a thread
that never touches the lock.

## Symptom

> "The control loop misses its deadline, but only when the diagnostics thread is
> busy. Diagnostics runs at the lowest priority we have, so it should not be
> able to affect anything."

## Hypothesis

A low-priority thread cannot delay a high-priority one by running — the
scheduler prevents that. It can delay one by *holding a resource it needs*. And
if the holder is itself being kept off the CPU by something in between, the
delay is bounded by that third thread's behaviour rather than by the critical
section.

That is priority inversion, and it is what put Mars Pathfinder into a reset loop
in 1997.

## Evidence

`evidence/06-broken-timing.txt`:

```
mutex:              std::mutex (no priority inheritance)
RLIMIT_RTPRIO:      soft 99, hard 99
pinned to one CPU:  yes
critical section:   5284763 iterations = 4215 us measured (target 5000 us)
SCHED_FIFO granted: 3 of 3 threads
worst lock wait:    15246 us

WAITED FAR LONGER THAN THE CRITICAL SECTION -- priority inversion.
```

`evidence/06-fixed-timing.txt`:

```
mutex:              pthread PTHREAD_PRIO_INHERIT (enabled)
SCHED_FIFO granted: 3 of 3 threads
worst lock wait:    5216 us

BOUNDED by one critical section.
```

**15246 µs against 5216 µs.** The threshold is not a round number: the critical
section holds the lock for ~5 ms, so a waiter blocked for *longer than the
critical section* cannot be waiting for the work inside it. It is waiting for
the holder to be scheduled at all. That is the whole diagnosis, and it is why
both programs measure and print their own critical-section length.

### The programs refuse to draw a conclusion without real priorities

Both variants check that `SCHED_FIFO` was actually granted, by reading the
policy back with `pthread_getschedparam` rather than trusting
`pthread_setschedparam` to have worked. If it was not granted they print
`INCONCLUSIVE` and exit 2.

This is not defensive padding. An earlier version ignored the return value, and
on WSL2 — where the *hard* `RLIMIT_RTPRIO` is 0 and cannot be raised — it
happily reported "priority inversion" at 6398 µs while the supposedly fixed
version reported 6224 µs. Both numbers were ordinary lock contention under the
time-sharing scheduler. There were no priorities, so nothing was inverted, and
the demonstration was measuring nothing while appearing to succeed.

### What actually grants SCHED_FIFO in a container

`evidence/06-scheduling-matrix.txt`:

```
docker flags                       RLIMIT_RTPRIO    SCHED_FIFO
---------------------------------- ---------------- -------------
--cap-add=SYS_NICE --ulimit rtprio soft 99, hard 99 3 of 3 threads
--cap-drop=ALL     --ulimit rtprio soft 99, hard 99 3 of 3 threads
--cap-add=SYS_NICE                 soft 0, hard 0   3 of 3 threads
--cap-drop=ALL                     soft 0, hard 0   0 of 3 threads
(docker defaults)                  soft 0, hard 0   0 of 3 threads
```

**Either `CAP_SYS_NICE` or a non-zero `RLIMIT_RTPRIO` is sufficient.** Docker's
default capability set provides neither.

This table exists because the obvious guess was wrong. The harness originally
asserted in a comment that `--cap-drop=ALL` makes real-time scheduling
impossible; the first run disproved it — row two gets all three threads. The
capability is needed to *exceed* the rlimit, so raising the rlimit removes the
need for the capability.

## Root cause

Three threads pinned to one CPU:

| thread | priority | behaviour |
|---|---|---|
| diagnostics | 10 | takes the lock, does 5 ms of work while holding it |
| hog | 50 | **never touches the lock**, uses the CPU in 10 ms bursts |
| control loop | 90 | wants the lock briefly, every cycle |

The control loop blocks on the lock. The holder is priority 10, so the hog at
priority 50 outranks it and runs instead. The holder cannot release what it
cannot finish. The highest-priority thread in the system is now waiting on the
lowest, and the delay is set by the hog's burst length — code that neither
lock-using function has ever heard of.

## Fix

`pthread_mutexattr_setprotocol(&attributes, PTHREAD_PRIO_INHERIT)`.

The kernel temporarily raises the holder to the priority of the highest-priority
waiter. The diagnostics thread is boosted to 90 while the control loop waits, so
it preempts the hog, finishes, releases, and drops back down.

Two things this costs:

**`std::mutex` cannot express it.** The standard provides no way to set the
attribute, so this drops to `pthread_mutex_t` directly, wrapped in a small RAII
`PriorityLock` so that giving up `std::lock_guard` does not also mean giving up
exception safety.

**It is the second-best fix.** The better one is not holding a lock across slow
work at all. Inheritance *bounds* the damage — the waiter now pays one critical
section instead of an unrelated thread's burst — but a 5 ms critical section is
a design problem that inheritance merely makes survivable. Read 5216 µs as
"bounded", not as "fast".

## Regression test

`scripts/run-all.sh` runs both variants and requires the broken one to exceed
twice the critical section and the fixed one to stay under it. On a host that
cannot grant `SCHED_FIFO` it reports SKIP and **leaves the committed evidence
untouched**, rather than overwriting a conclusive container run with an
inconclusive local one.

Real-time scheduling comes from `scripts/run-case-06-container.sh`.

## What made it hard

Getting the *experiment* right took three attempts, and each failure looked like
a result.

1. **No priorities.** `SCHED_FIFO` was refused and both variants reported the
   same lock contention. Fixed by reading the policy back and refusing to
   conclude.

2. **A critical section that could not be stretched.** The holder's work was
   written as "spin until 5 ms have elapsed", which finishes 5 ms after it
   starts no matter how little CPU it received. Preemption could not lengthen
   it, so there was nothing to observe. The critical section has to be a fixed
   amount of *work*, so that a thread that is not running takes longer to finish
   it.

3. **A hog whose idle window was too long.** The holder runs at the lowest
   priority, so it can only *acquire* the lock while the hog is asleep. With a
   15 ms idle window it always finished its 5 ms before the hog returned, and
   the program correctly reported "no significant inversion" every run. The
   idle window must be shorter than the critical section — 3 ms against 5 ms —
   to guarantee the holder is caught mid-section.

Two further platform constraints shape the numbers:

- **The kernel's real-time throttle.** `sched_rt_runtime_us` /
  `sched_rt_period_us` defaults to 950000/1000000, so `SCHED_FIFO` threads
  exceeding 95% of a CPU are all suspended for the rest of the period. An
  earlier configuration sat at ~96% and every measurement included a 50 ms
  kernel-imposed stall unrelated to the bug. The duty cycles here total ~88%.

- **Calibration that lied by a factor of 20,000.** The work loop was originally
  `accumulator += i * k` — a polynomial in `i`, which GCC recognises and
  replaces at `-O2` with a closed-form Gauss sum running in constant time.
  Calibration timed 20 million iterations at 219 ns, concluded the machine ran
  91,324 iterations per nanosecond, and asked for 456 billion iterations to fill
  5 ms. The program never finished. The loop is now a dependency chain that
  cannot be reduced, the probe count passes through `volatile` so it is not a
  compile-time constant, and **the calibration measures its own result and
  aborts if it is off by more than 2×** — a calibration is a prediction, and
  this one was wrong until it was checked.

The underlying bug, by contrast, is four lines. Reproducing a scheduling defect
honestly is harder than fixing it.
