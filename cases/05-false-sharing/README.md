# 05 — False sharing

Four threads, four separate counters, no shared state. Four threads are slower
than one.

## Symptom

> "We added a second worker thread and throughput went DOWN. Not 'failed to
> scale' — actually slower than one thread. Each worker touches its own
> counter, so there is no contention to speak of."

## Hypothesis

Negative scaling on independent data means the independence is not real at some
level below the source. The counters are logically separate, so the sharing must
be physical: they are adjacent in memory and therefore on the same cache line.

## Evidence

No sanitizer finds this. There is no race, no undefined behaviour, no incorrect
result — the program is right, and slow. Only measurement shows it.

`evidence/05-broken-timing.txt`:

```
counters are 8 bytes apart
1 thread :    242.7 M increments/s
4 threads:    110.7 M increments/s  (0.46x)
FOUR THREADS BARELY BEAT ONE -- this is false sharing
```

`evidence/05-fixed-timing.txt`:

```
counters are 64 bytes apart
1 thread :    239.7 M increments/s
4 threads:    940.5 M increments/s  (3.92x)
scales
```

**0.46× becomes 3.92×.** Single-threaded throughput is unchanged (242.7 vs
239.7 M/s), which matters: it shows the fix did not make the work itself faster,
it removed an interaction that only exists between threads.

The right tool for this would be `perf stat -e cache-misses,LLC-load-misses`, or
`perf c2c`, which is built specifically to attribute cache-line contention to
source lines. Neither is available here — `perf` is not installed,
`perf_event_paranoid` is 2, and installing needs root. So the measurement is the
evidence: the programs report the byte distance between counters alongside the
throughput, which is the same conclusion by a cruder route.

## Root cause

```cpp
struct Counters {
  std::atomic<std::uint64_t> value[4];   // 8 bytes apart -> same 64-byte line
};
```

Cache coherence operates on lines, not variables. When one core writes to any
byte of a line, that line is invalidated in every other core's cache. So each
worker's write to its *own* private counter forces the other three to take a
coherence miss on *their* own private counters. One line ping-pongs between four
cores fifty million times.

The threads never touch each other's data. The hardware cannot tell.

## Fix

```cpp
struct alignas(64) PaddedCounter {
  std::atomic<std::uint64_t> value{0};
  char padding[64 - sizeof(std::atomic<std::uint64_t>)]{};
};
static_assert(sizeof(PaddedCounter) == 64);
```

The algorithm is unchanged. The memory ordering is unchanged. `alignas` plus
explicit padding is the entire fix.

Both are needed: `alignas` alone guarantees each object *starts* on a line, but
the `static_assert` on `sizeof` is what guarantees an array of them keeps one
per line — alignment without size padding leaves the compiler free to produce a
type whose array elements share lines again.

A literal `64` is used rather than `std::hardware_destructive_interference_size`,
which is ABI-sensitive and which GCC warns about when its value could differ
between translation units.

The cost is 64 bytes per counter instead of 8. For four counters that is 256
bytes to recover a 4× speedup. It would be a bad trade for a million counters,
and that is the real design question: **pad the few contended things, not
everything.**

## Regression test

`scripts/run-all.sh` checks that the unpadded counters are 8 bytes apart and
show the failure-to-scale symptom, then checks that the padded counters are 64
bytes apart and deliver at least twice the unpadded four-thread throughput. The
comparison holds the thread count constant, so the changed memory layout is the
variable under test.

The script deliberately does not require the padded four-thread run to exceed
its own one-thread throughput by a fixed ratio. A hosted runner with roughly
one CPU's worth of effective parallelism cannot demonstrate four-core scaling
even when the padding is correct. Absolute throughput and one-to-four-thread
scale-up describe the machine; the layout and the paired
padded-versus-unpadded result are the regression claim.

## What made it hard

Nothing in the source is wrong. There is no incorrect line to find. Code review
cannot catch this, because the defect is in the memory layout the compiler
chose, and the layout is not written down anywhere in the program.

The symptom also actively misleads. "Adding threads made it slower" reads as a
scheduling or oversubscription problem, so the investigation goes to thread
counts, affinity, and the scheduler — all of which are innocent, and all of
which will absorb days.

And it is invisible at small scale in the other direction from case 03: with two
threads the effect is present but modest enough to look like ordinary
imperfect scaling.
